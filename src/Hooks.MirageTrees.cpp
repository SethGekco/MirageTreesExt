// ============================================================================
// MirageTreesExt — decoy-forest runtime.
//
// When a TechnoType with the Mirage.* config becomes still, it lays down a
// cluster of REAL, individually-damageable TerrainClass tree objects around
// itself (random count, random spread, per-tree health). Works for ANY
// TechnoType (unit / infantry / aircraft / building) — the vanilla mirage is
// UnitClass-only and single-tree; this replaces that with a real decoy forest.
//
// All randomness goes through ScenarioClass::Instance->Random for MP sync.
//
// Two engine behaviours are asserted here on the strength of the map loader's
// use of `new TerrainClass(type, cell)`; both are flagged RE-VERIFY and are the
// first things to confirm in-game:
//   RE-VERIFY #1: GameCreate<TerrainClass>(type, cell) (ctor @0x71BB90) both
//                 constructs AND places the tree on the cell + global Array.
//   RE-VERIFY #2: pTree->Limbo() + GameDelete(pTree) fully removes a decoy.
// ============================================================================

#include <algorithm>
#include <unordered_map>
#include <vector>

#include <TechnoClass.h>
#include <FootClass.h>
#include <BuildingClass.h>
#include <TerrainClass.h>
#include <TerrainTypeClass.h>
#include <CellClass.h>
#include <MapClass.h>
#include <ScenarioClass.h>
#include <RulesClass.h>
#include <HouseClass.h>
#include <Unsorted.h>
#include <Fundamentals.h>
#include <Memory.h>
#include <Surface.h>
#include <TacticalClass.h>
#include <FileSystem.h>

#include <Utilities/Macro.h>
#include <Utilities/Debug.h>
#include <Helpers/Cast.h>

#include <Ext/Techno/Body.h>
#include <Ext/TechnoType/Body.h>

// ---------------------------------------------------------------------------
// Fade rendering — owner/allies see decoys as translucent/pulsing so they can
// tell their own fakes from real trees; enemies see solid trees. Opt-in via
// Mirage.FadeStyle (default off).
//
// TerrainClass trees are ownerless, so we keep a side registry of the decoys we
// spawned (owner house + the fade config captured at spawn time). At terrain
// draw (0x71C2BC/0x71C2DC, inside TerrainClass::Draw) we look up the tree and,
// for an in-audience observer, OR a TransLucent flag into the blit flags before
// the tree SHP is drawn.
// ---------------------------------------------------------------------------

namespace
{
	enum FadeAudience { AUD_NONE = 0, AUD_OWNER = 1, AUD_ALLIES = 2, AUD_ALL = 3 };
	enum FadeStyle { STY_NONE = 0, STY_PULSE = 1, STY_TRANSLUCENT = 2, STY_SPAWN = 3 };

	struct DecoyInfo
	{
		HouseClass* Owner;
		int Audience;
		int Style;
		int Opacity;
		int PulseRate;
		int SpawnFrame;
	};

	std::unordered_map<TerrainClass*, DecoyInfo> DecoyRegistry;

	// Decoys queued for deletion on the next safe frame. Deleting a TerrainClass
	// from inside a techno's destructor cascade broadcasts pointer-invalidation
	// (BuildingClass::Detach etc.) to the half-destroyed techno and crashes
	// (C0000005 @ 0x44E9AA when a mirage building dies). So the destructor path
	// defers here and the per-frame driver frees them outside any dtor.
	std::vector<TerrainClass*> PendingDecoyDeletes;

	// Blit flags for the currently-drawing decoy (set in the stash hook, read in
	// the flags hook — same synchronous draw call, single-threaded renderer).
	BlitterFlags CurrentDecoyBlit = BlitterFlags::None;

	// A tree's SHP overhangs its own cell (foliage draws upward on screen), so
	// dirtying only its cell repaints just part of it — leaving "half the image"
	// stale and ghosts (a lingering tree-top) when it is removed. Dirty a block
	// around it. radius 1 (3x3) suffices for spawn/animation; removal uses a
	// larger radius so the tall foliage overhang is fully cleared, not just a
	// sliver left behind.
	void DirtyDecoyArea(CellStruct center, int radius = 1)
	{
		for (int dy = -radius; dy <= radius; ++dy)
			for (int dx = -radius; dx <= radius; ++dx)
			{
				CellStruct const c { static_cast<short>(center.X + dx),
									 static_cast<short>(center.Y + dy) };
				if (auto const pC = MapClass::Instance.TryGetCellAt(c))
					pC->MarkForRedraw();
			}
	}

	// Free everything queued by the destructor path. Safe: runs from the
	// per-frame driver, never inside an object's destruction.
	void ProcessPendingDecoyDeletes()
	{
		if (PendingDecoyDeletes.empty())
			return;

		for (auto const pTree : PendingDecoyDeletes)
		{
			if (!pTree || TerrainClass::Array.FindItemIndex(pTree) == -1)
				continue;
			CellStruct const cell = pTree->GetMapCoords();
			LogicClass::Instance.RemoveObject(pTree);
			pTree->Limbo();
			GameDelete(pTree);
			DirtyDecoyArea(cell, 2); // larger: clear the tall foliage overhang
		}
		PendingDecoyDeletes.clear();
	}

	// Opacity 0..100 (100 = solid) → nearest engine translucency step.
	BlitterFlags OpacityToBlit(int opacity)
	{
		if (opacity >= 88) return BlitterFlags::None;          // effectively solid
		if (opacity >= 63) return BlitterFlags::TransLucent25;
		if (opacity >= 38) return BlitterFlags::TransLucent50;
		return BlitterFlags::TransLucent75;                    // most transparent
	}

	bool ObserverInAudience(const DecoyInfo& d)
	{
		auto const pObs = HouseClass::CurrentPlayer;
		if (!pObs || !d.Owner)
			return false;

		switch (d.Audience)
		{
		case AUD_ALL:    return true;
		case AUD_OWNER:  return pObs == d.Owner;
		case AUD_ALLIES: return pObs == d.Owner || pObs->IsAlliedWith(d.Owner);
		default:         return false; // AUD_NONE
		}
	}

	BlitterFlags ComputeDecoyBlit(TerrainClass* pTree)
	{
		auto const it = DecoyRegistry.find(pTree);
		if (it == DecoyRegistry.end())
			return BlitterFlags::None;

		auto const& d = it->second;
		if (d.Style == STY_NONE || !ObserverInAudience(d))
			return BlitterFlags::None;

		int const rate = d.PulseRate > 0 ? d.PulseRate : 15;

		switch (d.Style)
		{
		case STY_TRANSLUCENT:
			return OpacityToBlit(d.Opacity);

		case STY_SPAWN:
		{
			// Fade in from most-transparent to solid over the first few steps
			// after spawning, then stay solid.
			static const BlitterFlags ramp[3] = {
				BlitterFlags::TransLucent75, BlitterFlags::TransLucent50, BlitterFlags::TransLucent25 };
			int const step = (Unsorted::CurrentFrame - d.SpawnFrame) / rate;
			return step >= 3 ? BlitterFlags::None : ramp[step];
		}

		case STY_PULSE:
		default:
		{
			// Oscillate solid → …→ most transparent → …→ solid (period 6 steps).
			static const BlitterFlags cycle[6] = {
				BlitterFlags::None, BlitterFlags::TransLucent25, BlitterFlags::TransLucent50,
				BlitterFlags::TransLucent75, BlitterFlags::TransLucent50, BlitterFlags::TransLucent25 };
			return cycle[(Unsorted::CurrentFrame / rate) % 6];
		}
		}
	}
}

// ---------------------------------------------------------------------------
// State helpers
// ---------------------------------------------------------------------------

// Should the decoy forest currently be shown for this techno?
//
// For UnitClass mirage (the Mirage Tank), the engine already maintains the
// authoritative "am I showing my tree disguise right now" state in
// TechnoClass::Disguised — it applies the settle delay when the unit stops and
// clears the instant it moves. Gating on that makes the forest appear/disappear
// exactly in step with the vanilla disguise, which is far more reliable than
// sampling locomotor speed (which reads non-zero while decelerating and can read
// zero for a frame mid-path). Buildings are always still. Infantry/aircraft have
// no engine-managed mirage state, so they fall back to the still heuristic.
static bool MirageShouldShow(TechnoClass* pThis)
{
	switch (pThis->WhatAmI())
	{
	case AbstractType::Unit:
		return pThis->Disguised;
	case AbstractType::Building:
		return true;
	case AbstractType::Infantry:
	case AbstractType::Aircraft:
	{
		auto const pFoot = static_cast<FootClass*>(pThis);
		return pFoot->GetCurrentSpeed() == 0 && pFoot->Destination == nullptr;
	}
	default:
		return false;
	}
}

// Create a real, PLACED tree on the given cell.
//
// The game's TerrainClass ctor (0x71BB90) takes the cell as a CellStruct* and
// dereferences it, computing lepton coords and putting the tree on the map. YRpp
// declares the ctor with the cell BY VALUE, so GameCreate<TerrainClass>(type,
// cell) pushes the packed coords inline; the ctor then treats that value as a
// pointer and dereferences it — landing the tree in limbo (unplaced) at best and
// an access-violation crash at worst (confirmed: EIP 0x71BC31, ECX = a packed
// cell value). So we allocate on the game heap and call the ctor directly with a
// real pointer. Same "call the game address, don't trust the YRpp wrapper" trap
// as R0/JMP_THIS mismatches.
static TerrainClass* CreatePlacedTree(TerrainTypeClass* pType, CellStruct cell)
{
	auto const pTree = static_cast<TerrainClass*>(
		YRMemory::AllocateChecked(sizeof(TerrainClass)));
	if (!pTree)
		return nullptr;

	reinterpret_cast<TerrainClass* (__thiscall*)(TerrainClass*, TerrainTypeClass*, CellStruct*)>
		(0x71BB90)(pTree, pType, &cell);

	return pTree;
}

bool TechnoExt::ShouldHaveMirage(TechnoClass* pThis)
{
	if (!pThis || pThis->InLimbo || !pThis->IsAlive || pThis->Health <= 0)
		return false;

	auto const pType = pThis->GetTechnoType();
	auto const pTypeExt = TechnoTypeExt::ExtMap.Find(pType);

	if (!pTypeExt || !pTypeExt->HasMirageTrees())
		return false;

	// Infantry/aircraft: gate on the debounced still counter (driven once/frame by
	// UpdateMirageTrees), NOT the raw instantaneous read — the locomotor speed can
	// read 0 for a frame mid-walk, and a single such frame would otherwise morph a
	// walking unit into a tree and drop it off enemy targeting. Units (native flag)
	// and buildings (always still) are reliable and use the raw check.
	switch (pThis->WhatAmI())
	{
	case AbstractType::Infantry:
	case AbstractType::Aircraft:
	{
		auto const pExt = TechnoExt::ExtMap.Find(pThis);
		return pExt && pExt->MirageStillFrames >= pTypeExt->MirageStillDelay;
	}
	default:
		return MirageShouldShow(pThis);
	}
}

// True while a techno is in its post-fire "blink" window (disguise dropped).
static bool MirageRevealed(TechnoExt::ExtData* pExt)
{
	return pExt && pExt->MirageRevealTimer > 0;
}

// Blink-on-fire: when a disguised techno fires, drop its disguise for a short
// window so it briefly reveals to enemies (auto-target + hover name), matching a
// real mirage tank's muzzle blink. The disguise driver counts the timer down and
// re-disguises when it hits 0. TechnoClass::Fire entry (0x6FDD50, this = ECX);
// Phobos's Fire hooks live at 0x6FDD7D+, so the entry is uncontended.
DEFINE_HOOK(0x6FDD50, TechnoClass_Fire_MirageBlink, 0x6)
{
	GET(TechnoClass*, pThis, ECX);
	GET_STACK(AbstractClass*, pTargetAbs, 0x4); // Fire(target, weaponIdx): arg1 = target

	// Our own disguised unit firing → drop its disguise (reveal) for a window. This
	// now applies to BUILDINGS too: with the object-layer render + the "reveal while
	// it has a target" latch below, a firing pillbox stays revealed for the whole
	// engagement and re-disguises when idle (no per-shot flicker like the old
	// skip+post-pass render caused).
	if (auto const pExt = TechnoExt::ExtMap.Find(pThis))
	{
		auto const pTypeExt = TechnoTypeExt::ExtMap.Find(pThis->GetTechnoType());
		if (pTypeExt && pTypeExt->MirageDisguise && pTypeExt->MirageBlinkOnFire > 0)
			pExt->MirageRevealTimer = pTypeExt->MirageBlinkOnFire;
	}

	// Drop a stale AUTO-target lock. EvaluateObject only blocks NEW acquisition, so
	// an enemy that locked this unit BEFORE it disguised keeps hammering it. But only
	// drop once the target has STAYED disguised a while (MirageDisguisedFrames >=
	// MirageLockDropDelay) — otherwise a unit that merely pauses for a moment made
	// its attacker drop the lock and then lag to re-acquire when it moved again,
	// leaving visible in-range units un-shot. If the firer is auto-attacking (NOT an
	// explicit Attack/force-fire order) a durably-disguised enemy, clear its target
	// so it stops and re-evaluates. Explicit orders (Mission::Attack, incl. force-
	// fire) are left intact so the player can still Ctrl-fire a disguise.
	constexpr int MirageLockDropDelay = 45; // ~sustained disguise before dropping
	if (auto const pTgt = abstract_cast<TechnoClass*>(pTargetAbs))
	{
		auto const pTgtExt = TechnoExt::ExtMap.Find(pTgt);
		if (pTgtExt && pTgtExt->MirageDisguiseActive
			&& pTgtExt->MirageDisguisedFrames >= MirageLockDropDelay
			&& pThis->CurrentMission != Mission::Attack
			&& pThis->Owner && pTgt->Owner
			&& pThis->Owner != pTgt->Owner
			&& !pThis->Owner->IsAlliedWith(pTgt->Owner))
		{
			pThis->SetTarget(nullptr);
		}
	}

	return 0;
}

// ---------------------------------------------------------------------------
// Spawn / clear
// ---------------------------------------------------------------------------

// Place one tree on `cell` and wire it into all our tracking (fade registry,
// logic layer, redraw). Returns true if a tree was placed. Templated on the
// disguise container because GetElements() yields a Phobos Iterator, not a
// std::vector.
template <typename TDisguises>
static bool PlaceMirageTree(TechnoClass* pThis, TechnoExt::ExtData* pExt,
	TechnoTypeExt::ExtData* pTypeExt, CellStruct cell,
	const TDisguises& disguises)
{
	auto const pCell = MapClass::Instance.TryGetCellAt(cell);
	if (!pCell || pCell->GetTerrain(false) != nullptr) // off-map or already treed
		return false;

	auto& random = ScenarioClass::Instance->Random;
	auto const pTerrainType = disguises[random.RandomRanged(0, static_cast<int>(disguises.size()) - 1)];
	if (!pTerrainType)
		return false;

	auto const pTree = CreatePlacedTree(pTerrainType, cell);
	if (!pTree)
		return false;

	pTree->Health = pTypeExt->MirageHealth > 0
		? pTypeExt->MirageHealth.Get()
		: pTerrainType->Strength;

	pExt->MirageTrees.push_back(pTree);
	DecoyRegistry[pTree] = DecoyInfo {
		pThis->Owner,
		pTypeExt->MirageFadeAudience, pTypeExt->MirageFadeStyle,
		pTypeExt->MirageFadeOpacity, pTypeExt->MirageFadePulseRate,
		Unsorted::CurrentFrame };
	LogicClass::Instance.AddObject(pTree, false);
	pTree->Mark(MarkType::ChangeRedraw);
	DirtyDecoyArea(cell, 2); // larger: paint the full tree incl. tall overhang
	return true;
}

void TechnoExt::SpawnMirageTrees(TechnoClass* pThis)
{
	auto const pExt = TechnoExt::ExtMap.Find(pThis);
	if (!pExt || pExt->MirageActive)
		return;

	auto const pTypeExt = TechnoTypeExt::ExtMap.Find(pThis->GetTechnoType());
	if (!pTypeExt)
		return;

	auto const& disguises = pTypeExt->MirageDefaultDisguises.GetElements(
		RulesClass::Instance->DefaultMirageDisguises);
	if (disguises.empty())
		return;

	auto& random = ScenarioClass::Instance->Random;
	CellStruct const anchor = pThis->GetMapCoords();

	// COVERING (disguise mode, non-units): put tree(s) directly ON the techno so
	// it reads as a tree. Infantry = its own cell; buildings = footprint cells up
	// to Mirage.Count. Units use the native field-based disguise instead.
	bool const cover = pTypeExt->MirageDisguise && pThis->WhatAmI() != AbstractType::Unit;
	if (cover)
	{
		if (pThis->WhatAmI() == AbstractType::Building)
		{
			int limit = std::max(pTypeExt->MirageCount.Get().X, pTypeExt->MirageCount.Get().Y);
			if (limit <= 0)
				limit = 64;
			int placed = 0;
			// Foundation data is a list of relative cell offsets, terminated by a
			// {0x7FFF,...} sentinel. Cap iterations defensively.
			if (auto pFound = pThis->GetFoundationData(false))
			{
				for (int guard = 0; guard < 64 && placed < limit
					&& pFound->X != 0x7FFF && pFound->Y != 0x7FFF; ++pFound, ++guard)
				{
					CellStruct const c { static_cast<short>(anchor.X + pFound->X),
										 static_cast<short>(anchor.Y + pFound->Y) };
					if (PlaceMirageTree(pThis, pExt, pTypeExt, c, disguises))
						++placed;
				}
			}
			if (placed == 0) // 0-cell foundation fallback: cover the origin
				PlaceMirageTree(pThis, pExt, pTypeExt, anchor, disguises);
		}
		else // infantry / aircraft
		{
			PlaceMirageTree(pThis, pExt, pTypeExt, anchor, disguises);
		}
	}

	// SCATTER (decoy mode): random count/spread around the techno, never on its
	// own cell.
	if (pTypeExt->MirageDecoys)
	{
		int const countMin = pTypeExt->MirageCount.Get().X;
		int const countMax = pTypeExt->MirageCount.Get().Y;
		int const distMin = pTypeExt->MirageDistance.Get().X;
		int const distMax = pTypeExt->MirageDistance.Get().Y;
		int const count = random.RandomRanged(std::min(countMin, countMax), std::max(countMin, countMax));

		for (int i = 0; i < count; ++i)
		{
			int const dist = random.RandomRanged(std::min(distMin, distMax), std::max(distMin, distMax));
			int const dx = dist > 0 ? random.RandomRanged(-dist, dist) : 0;
			int const dy = dist > 0 ? random.RandomRanged(-dist, dist) : 0;
			CellStruct const target { static_cast<short>(anchor.X + dx),
									  static_cast<short>(anchor.Y + dy) };
			if (target == anchor)
				continue;
			PlaceMirageTree(pThis, pExt, pTypeExt, target, disguises);
		}
	}

	Debug::Log("[MirageTreesExt] Spawn %s at (%d,%d): %d trees (cover=%d)\n",
		pThis->GetTechnoType()->ID, anchor.X, anchor.Y,
		static_cast<int>(pExt->MirageTrees.size()), cover);

	pExt->MirageActive = true;
	pExt->MirageAnchor = anchor;
}

void TechnoExt::ClearMirageTreesFor(TechnoExt::ExtData* pExt, bool deferDelete)
{
	if (!pExt)
		return;

	for (auto const pTree : pExt->MirageTrees)
	{
		if (!pTree)
			continue;

		DecoyRegistry.erase(pTree);

		// Only touch trees the engine still knows about (a decoy the player
		// shot down is already gone and was dropped via InvalidatePointer).
		if (TerrainClass::Array.FindItemIndex(pTree) == -1)
			continue;

		if (deferDelete)
		{
			// Destructor path: don't free now (that would crash the techno's own
			// destruction cascade). Queue it; the per-frame driver frees it.
			PendingDecoyDeletes.push_back(pTree);
			continue;
		}

		// Normal (move-triggered) path: safe to free inline. Detach from the
		// logic layer and cell, free, and dirty the vacated area so the stale
		// tree image (which overhangs its cell) is fully repainted away.
		CellStruct const cell = pTree->GetMapCoords();
		LogicClass::Instance.RemoveObject(pTree);
		pTree->Limbo();
		GameDelete(pTree);
		DirtyDecoyArea(cell, 2); // larger: clear the tall foliage overhang
	}

	pExt->MirageTrees.clear();
	pExt->MirageActive = false;

	// If this techno was disguised as a tree (pure morph), its last tree frame was
	// painted through its own draw and overhangs its cell. When it dies its own cell
	// is repainted (rubble) but the overhang cells are not, leaving a tree stuck on
	// screen until the view moves. Dirty the block so the ghost clears immediately.
	if (pExt->MirageDisguiseActive)
	{
		pExt->MirageDisguiseActive = false;
		if (auto const pThis = pExt->OwnerObject())
			DirtyDecoyArea(pThis->GetMapCoords(), 2);
	}
}

void TechnoExt::ClearMirageTrees(TechnoClass* pThis)
{
	TechnoExt::ClearMirageTreesFor(TechnoExt::ExtMap.Find(pThis));
}

// Disguise mode: the techno itself renders AS a tree, using the vanilla mirage
// machinery (set Disguise to a TerrainType + Disguised, exactly like a Mirage
// Tank). Units already do this natively, so we skip them. Infantry MAY render it
// through the shared disguise draw; buildings ignore Disguise in their draw, so
// they need the dedicated building draw hook (next phase) to show anything.
static void UpdateMirageDisguise(TechnoClass* pThis, TechnoExt::ExtData* pExt,
	TechnoTypeExt::ExtData* pTypeExt)
{
	// The vanilla terrain-disguise (render self AS a tree) is safe ONLY for
	// UnitClass. Putting a TerrainType into an infantryman's Disguise field
	// crashes InfantryClass::ReceiveDamage (0x518E08 — it indexes type data off a
	// null pointer), and buildings ignore Disguise in their draw entirely. So this
	// field-based path is units-only; infantry/building self-disguise needs a
	// custom draw hook (renders the tree SHP over the techno for non-allies),
	// which is the next phase.
	if (pThis->WhatAmI() != AbstractType::Unit)
		return;

	bool const shouldShow = TechnoExt::ShouldHaveMirage(pThis) && !MirageRevealed(pExt);

	if (shouldShow && !pExt->MirageDisguiseActive)
	{
		auto const& disguises = pTypeExt->MirageDefaultDisguises.GetElements(
			RulesClass::Instance->DefaultMirageDisguises);
		int const size = static_cast<int>(disguises.size());
		if (size <= 0)
			return;

		auto const pTree = disguises[ScenarioClass::Instance->Random.RandomRanged(0, size - 1)];
		if (!pTree)
			return;

		pThis->Disguise = pTree;                 // TerrainTypeClass* -> ObjectTypeClass*
		pThis->DisguisedAsHouse = pThis->Owner;
		pThis->Disguised = true;
		pExt->MirageDisguiseActive = true;
		pThis->Mark(MarkType::ChangeRedraw);

		Debug::Log("[MirageTreesExt] disguise ON %s as %s\n",
			pThis->GetTechnoType()->ID, pTree->ID);
	}
	else if (!shouldShow && pExt->MirageDisguiseActive)
	{
		pThis->Disguised = false;
		pThis->Disguise = nullptr;
		pExt->MirageDisguiseActive = false;
		pThis->Mark(MarkType::ChangeRedraw);
	}
}

// ---------------------------------------------------------------------------
// Per-frame driver
// ---------------------------------------------------------------------------

static void UpdateDecoyForest(TechnoClass* pThis, TechnoExt::ExtData* pExt,
	TechnoTypeExt::ExtData* pTypeExt);

void TechnoExt::UpdateMirageTrees(TechnoClass* pThis)
{
	// Free any decoys queued by a destroyed techno's dtor last frame — done here
	// (outside any destruction cascade) so it is safe.
	ProcessPendingDecoyDeletes();

	auto const pExt = TechnoExt::ExtMap.Find(pThis);
	if (!pExt)
		return;

	// Count down the post-fire blink; while it runs the disguise stays dropped.
	if (pExt->MirageRevealTimer > 0)
		--pExt->MirageRevealTimer;

	// One-time diagnostic per mirage-capable instance: proves the driver hook
	// fires, the type parsed, and reports the live "still" gate.
	if (!pExt->MirageDiagLogged)
	{
		auto const pTypeExt = TechnoTypeExt::ExtMap.Find(pThis->GetTechnoType());
		if (pTypeExt && pTypeExt->HasMirageTrees())
		{
			pExt->MirageDiagLogged = true;
			Debug::Log("[MirageTreesExt] seen %s still=%d shouldHave=%d\n",
				pThis->GetTechnoType()->ID, MirageShouldShow(pThis),
				TechnoExt::ShouldHaveMirage(pThis));
		}
	}

	auto const pTypeExt = TechnoTypeExt::ExtMap.Find(pThis->GetTechnoType());
	if (!pTypeExt)
		return;

	// Debounce the flickery infantry/aircraft "still" read: accumulate consecutive
	// still frames (capped at the delay), reset instantly on any movement. The
	// disguise gates on this counter via ShouldHaveMirage, so a walking unit never
	// briefly morphs into a tree or drops off enemy targeting. (Harmless for
	// units/buildings, whose ShouldHaveMirage ignores the counter.)
	if (MirageShouldShow(pThis))
	{
		if (pExt->MirageStillFrames < pTypeExt->MirageStillDelay)
			++pExt->MirageStillFrames;
	}
	else
		pExt->MirageStillFrames = 0;

	// Keep the disguise dropped for as long as the unit holds a target, not just for
	// one blink per shot. Now includes BUILDINGS: a pillbox fires many times a second,
	// so latching the reveal on Target keeps it a visible structure for the whole
	// engagement and re-disguises BlinkOnFire frames after the target drops — with the
	// object-layer render this is a clean reveal, not the old per-shot flicker.
	if (pTypeExt->MirageBlinkOnFire > 0 && pThis->Target)
		pExt->MirageRevealTimer = pTypeExt->MirageBlinkOnFire;

	// Units self-disguise via the native field (renders as a real tree, safe).
	if (pTypeExt->MirageDisguise && pThis->WhatAmI() == AbstractType::Unit)
		UpdateMirageDisguise(pThis, pExt, pTypeExt);

	// Non-unit disguise = PURE MORPH: no separate tree object. Pick a tree when it
	// activates and flag it; the draw hook renders that TerrainType's sprite in the
	// techno's place for enemy viewers (and skips the techno's own draw).
	if (pTypeExt->MirageDisguise && pThis->WhatAmI() != AbstractType::Unit)
	{
		bool const shouldDisguise = TechnoExt::ShouldHaveMirage(pThis) && !MirageRevealed(pExt);
		if (shouldDisguise && !pExt->MirageDisguiseActive)
		{
			// Keep the same tree across a blink so re-disguising doesn't visibly
			// swap species (a tell); only roll a fresh one when we have none.
			if (!pExt->MirageDisguiseTree)
			{
				auto const& disguises = pTypeExt->MirageDefaultDisguises.GetElements(
					RulesClass::Instance->DefaultMirageDisguises);
				if (disguises.size() > 0)
					pExt->MirageDisguiseTree = disguises[ScenarioClass::Instance->Random.RandomRanged(
						0, static_cast<int>(disguises.size()) - 1)];
			}
			if (pExt->MirageDisguiseTree)
			{
				pExt->MirageDisguiseActive = true;
				pThis->Mark(MarkType::ChangeRedraw); // repaint: unit -> tree
				// The tree sprite overhangs its cell (foliage rises north on screen);
				// Mark only dirties the unit's own cell, leaving the overhang cells
				// stale (tree drawn only partially until you nudge the view). Dirty a
				// block so the whole tree paints, and later fully clears.
				DirtyDecoyArea(pThis->GetMapCoords(), 2);
			}
		}
		else if (!shouldDisguise && pExt->MirageDisguiseActive)
		{
			pExt->MirageDisguiseActive = false;
			// A blink keeps the tree (re-disguise as the same one); any other
			// deactivation (moved, died) forgets it so the next disguise re-rolls.
			if (!MirageRevealed(pExt))
				pExt->MirageDisguiseTree = nullptr;
			pThis->Mark(MarkType::ChangeRedraw); // repaint: tree -> unit
			DirtyDecoyArea(pThis->GetMapCoords(), 2); // clear the tree's overhang
		}

		// The morph tree is only (re)drawn on frames the techno's own DrawObject
		// runs — every frame for foot units (they animate), but buildings only
		// redraw when their cell is dirtied, so a disguised BUILDING's tree blinks
		// on and off. Keep a disguised building marked for redraw each frame so its
		// tree is painted every frame, steady like an infantryman's.
		if (pExt->MirageDisguiseActive && pThis->WhatAmI() == AbstractType::Building)
			pThis->Mark(MarkType::ChangeRedraw);
	}

	// Track how long the disguise has been continuously active (for the auto-lock-
	// drop's sustained-disguise gate). Reset the instant it is not disguised.
	if (pExt->MirageDisguiseActive)
	{
		if (pExt->MirageDisguisedFrames < 0x7FFF)
			++pExt->MirageDisguisedFrames;

		// The owner pulse flips the render unit<->tree on a 90-frame cycle (tree in
		// [75,90)); on those two toggle frames dirty the overhang so the tall tree
		// doesn't leave a partial top or a ghost when it swaps. (Cheap: 2 frames/90;
		// harmless for the steady enemy-view tree.)
		int const phase = Unsorted::CurrentFrame % 90;
		if (phase == 0 || phase == 75)
			DirtyDecoyArea(pThis->GetMapCoords(), 2);
	}
	else
		pExt->MirageDisguisedFrames = 0;

	// Decoys: separate scattered tree objects (independent of disguise).
	if (pTypeExt->MirageDecoys)
		UpdateDecoyForest(pThis, pExt, pTypeExt);
}

// The decoy-forest half of the per-frame driver (spawn / tear down / animate).
static void UpdateDecoyForest(TechnoClass* pThis, TechnoExt::ExtData* pExt,
	TechnoTypeExt::ExtData* pTypeExt)
{
	// Nothing laid down yet: lay it when the techno settles.
	if (!pExt->MirageActive)
	{
		if (TechnoExt::ShouldHaveMirage(pThis))
			TechnoExt::SpawnMirageTrees(pThis);
		return;
	}

	// A forest is down. Tear it back up if the techno moved or stopped being
	// eligible (moving, destroyed, undisguised); it will be re-laid when it
	// next settles.
	if (!TechnoExt::ShouldHaveMirage(pThis) || pThis->GetMapCoords() != pExt->MirageAnchor)
	{
		TechnoExt::ClearMirageTrees(pThis);
		return;
	}

	// Animated fade styles (pulse, spawn fade-in) need their decoy cells
	// repainted every frame — static terrain is otherwise drawn once and cached,
	// which would freeze the animation. Cheap: a few cells per techno.
	if (pTypeExt->MirageFadeStyle == STY_PULSE || pTypeExt->MirageFadeStyle == STY_SPAWN)
	{
		for (auto const pTree : pExt->MirageTrees)
		{
			if (!pTree || TerrainClass::Array.FindItemIndex(pTree) == -1)
				continue;
			// Dirty a block so the tree's tall overhang repaints, not just its
			// base cell (which left "half the image" stale).
			DirtyDecoyArea(pTree->GetMapCoords(), 2);
		}
	}
}

// ---------------------------------------------------------------------------
// Hooks — per-frame update points (registers verified from Phobos / sibling).
//   0x4DA8A0 FootClass::Update  (ESI = FootClass*)  — infantry/units/aircraft
//   0x43FE69 BuildingClass_AI   (ESI = BuildingClass*) — buildings.
//     NOTE: 0x43FE69 is a shared chain point (Phobos BuildingClass_AI,
//     Antares/Ares BuildingClass_Update_SensorArray, all 0xA). Syringe chains
//     same-address hooks, so co-hooking is safe; size matched to 0xA.
// ---------------------------------------------------------------------------

DEFINE_HOOK(0x4DA8A0, FootClass_Update_MirageTrees, 0x6)
{
	GET(FootClass* const, pThis, ESI);
	TechnoExt::UpdateMirageTrees(pThis);
	return 0;
}

DEFINE_HOOK(0x43FE69, BuildingClass_AI_MirageTrees, 0xA)
{
	GET(BuildingClass* const, pThis, ESI);
	TechnoExt::UpdateMirageTrees(pThis);
	return 0;
}

// ---------------------------------------------------------------------------
// Render-swap disguise — hide the techno's own art from ENEMY viewers so they
// see only the tree on its cell. Pure visual (unlike cloak): the techno stays
// fully functional — fires, is targetable — because TechnoClass::DrawObject is
// rendering-only. This is how vanilla mirage works (a draw swap), without the
// cloak side effects (can't-fire-while-cloaked, targeting/perf problems).
//
// 0x705E00 = TechnoClass::DrawObject(this=ECX, arg1); hook after the prologue
// (ESI=this) and, for a disguised techno viewed by an enemy, redirect to the
// function's clean epilogue at 0x706602 (pop regs; add esp,0x44; ret 0x40) to
// skip the whole draw. Owner/allies fall through and see it normally.
// ---------------------------------------------------------------------------

static bool MirageHiddenFromViewer(TechnoClass* pThis)
{
	// Units use the native field disguise (they draw their own tree); don't skip.
	if (pThis->WhatAmI() == AbstractType::Unit)
		return false;

	auto const pExt = TechnoExt::ExtMap.Find(pThis);
	if (!pExt || !pExt->MirageDisguiseActive)
		return false;

	auto const pObserver = HouseClass::CurrentPlayer;
	if (!pObserver || !pThis->Owner)
		return false;

	// Owner and allies see the real techno; everyone else sees the tree instead.
	return pObserver != pThis->Owner && !pObserver->IsAlliedWith(pThis->Owner);
}

// Whether the CURRENT viewer should see this techno DRAWN as the tree. Used only by
// the object-layer sprite-swap + the DrawExtras skip — NOT by targeting/tooltip/
// selection (those stay enemy-only via MirageHiddenFromViewer, so the owner can
// still select and command a disguised unit while it visually pulses).
//   - enemies: always a tree.
//   - owner / allies (per Mirage.FadeAudience): a brief periodic PULSE to the tree,
//     ~1s of every ~5s, so the player can tell the unit is miraged — mirroring how
//     a vanilla mirage tank periodically shows its owner the tree.
static bool MirageRenderAsTree(TechnoClass* pThis)
{
	if (pThis->WhatAmI() == AbstractType::Unit)
		return false; // units use the native field disguise

	auto const pExt = TechnoExt::ExtMap.Find(pThis);
	if (!pExt || !pExt->MirageDisguiseActive)
		return false;

	auto const pObs = HouseClass::CurrentPlayer;
	if (!pObs || !pThis->Owner)
		return false;

	if (pObs != pThis->Owner && !pObs->IsAlliedWith(pThis->Owner))
		return true; // enemy: always a tree

	auto const pTypeExt = TechnoTypeExt::ExtMap.Find(pThis->GetTechnoType());
	if (!pTypeExt)
		return false;

	int const aud = pTypeExt->MirageFadeAudience;
	bool const inAudience =
		aud == AUD_ALL ||
		(aud == AUD_OWNER && pObs == pThis->Owner) ||
		(aud == AUD_ALLIES && (pObs == pThis->Owner || pObs->IsAlliedWith(pThis->Owner)));
	if (!inAudience)
		return false;

	// ~1s tree in a ~5s cycle (frame-based, scales with game speed).
	return (Unsorted::CurrentFrame % 90) >= 75;
}

// Give a disguised techno the "no interaction" cursor of a tree instead of the
// selectable-object cursor, for the enemy viewers who see it as a tree. The hover
// cursor's SELECT action is gated on ObjectClass::CanBeSelected — whose shared core
// is 0x5F6C30 (FootClass::CanBeSelected 0x4DFA50 tail-jumps here after its own
// +0x6AD check; BuildingClass uses it directly), so this ONE hook covers infantry,
// buildings and aircraft. Return false (not selectable) while the techno is hidden-
// as-a-tree from the current viewer. The owner (who sees the real unit) is exempt
// via MirageHiddenFromViewer, and the enemy's ATTACK cursor + force-fire don't use
// CanBeSelected, so they still work — you just no longer get the tell-tale select
// cursor on the fake trees. Suppress path jumps to the bare `ret` at 0x4DFA5C.
DEFINE_HOOK(0x5F6C30, TechnoClass_CanBeSelected_MirageHide, 0x9)
{
	GET(TechnoClass*, pThis, ECX);
	if (MirageHiddenFromViewer(pThis))
	{
		R->EAX(0);        // AL = 0: not selectable
		return 0x4DFA5C;  // a lone `ret` — returns false to the caller
	}
	return 0;
}

// Blank the display NAME of a disguised techno for enemy viewers, so hovering it
// shows nothing — a tree has no name. The map tooltip AND the attack-cursor target
// name both call the object's GetUIName, so blanking it at the source covers every
// path (more robust than hooking one tooltip site). GetUIName is per-class:
// InfantryClass 0x51F2C0, BuildingClass 0x459ED0. Units use the native disguise
// (their own GetUIName handles the tree name) and are excluded via
// MirageHiddenFromViewer. Return a static empty string via the lone `ret` at
// 0x459ED9 (both hooks borrow it; at each entry the stack is just [retaddr]).
namespace { const wchar_t MirageNoName[1] = { L'\0' }; }

DEFINE_HOOK(0x51F2C0, InfantryClass_GetUIName_MirageHide, 0x9)
{
	GET(TechnoClass*, pThis, ECX);
	if (MirageHiddenFromViewer(pThis))
	{
		R->EAX(reinterpret_cast<DWORD>(&MirageNoName[0]));
		return 0x459ED9; // a lone `ret`
	}
	return 0;
}

DEFINE_HOOK(0x459ED0, BuildingClass_GetUIName_MirageHide, 0x6)
{
	GET(TechnoClass*, pThis, ECX);
	if (MirageHiddenFromViewer(pThis))
	{
		R->EAX(reinterpret_cast<DWORD>(&MirageNoName[0]));
		return 0x459ED9; // this function's own `ret`
	}
	return 0;
}

// Suppress the map-hover NAME tooltip for a disguised unit, so hovering an enemy
// disguise reveals nothing (like hovering a real tree). DisplayClass::SetAction
// builds the hovered object's tooltip at 0x4ABC31 via `mov eax,[ecx]; call
// [eax+0x90]` (ECX = the hovered object = GetUIName's `this`), then feeds the name
// to the tooltip-text setter. When ECX == null the game already skips that block
// and continues at 0x4ABC46 (both paths converge there). We take the same skip for
// our disguised-from-viewer technos. Verified in objdump. WhatAmI()/ExtMap.Find are
// safe on any ObjectClass, so a non-techno hovered object falls through harmlessly.
DEFINE_HOOK(0x4ABC31, DisplayClass_SetAction_MirageTooltip, 0xA)
{
	GET(TechnoClass*, pObj, ECX);
	if (pObj && MirageHiddenFromViewer(pObj))
		return 0x4ABC46; // skip the name → no tooltip, exactly like a tree
	return 0;
}

// The DELAYED map hover-name tooltip (DisplayClass::GetToolTip) is a SEPARATE path
// that also names the hovered object. At 0x4AE668 it loads the object from
// [esp+0xC] into ECX then calls its GetUIName ([edx+0x90]) at 0x4AE672. We can't
// blank it via the getter for buildings — Antares owns BuildingClass::GetUIName
// (EnemyUIName) and wins the hook chain — so intercept the CALL SITE instead:
// for a disguised-from-viewer object, jump to the function's own "no object"
// return (0x4AE69B: xor eax,eax; ret) so no name is produced. Covers every class.
DEFINE_HOOK(0x4AE668, DisplayClass_GetToolTip_MirageName, 0x8)
{
	GET_STACK(TechnoClass*, pObj, 0xC);
	if (pObj && MirageHiddenFromViewer(pObj))
		return 0x4AE69B; // returns null tooltip text
	return 0;
}

// #3 OBJECT-LAYER DISGUISE. Instead of skipping the techno's draw and painting the
// tree in a later top pass (which drew over the shroud and wasn't occluded by front
// objects), we let the techno draw ITSELF but swap the sprite it blits to the tree.
// The tree then renders in the object's own layer — occluded and shrouded exactly
// like a real tree, and baked into the cell (no flash, correct fog). The disguised
// techno's DrawObject sets these; the CC_Draw_Shape hook substitutes them (frame 0
// so the object's facing/sequence frame index can't read past the tree's frames);
// the DrawObject epilogue clears them.
namespace
{
	SHPStruct* MirageMorphSHP = nullptr;
	ConvertClass* MirageMorphPalette = nullptr;
}

DEFINE_HOOK(0x705E15, TechnoClass_DrawObject_MirageDisguise, 0x5)
{
	GET(TechnoClass*, pThis, ESI);

	MirageMorphSHP = nullptr; // default: this object draws itself normally
	if (MirageRenderAsTree(pThis)) // enemies always; owner/allies pulse periodically
	{
		auto const pExt = TechnoExt::ExtMap.Find(pThis);
		auto const pTreeType = pExt ? pExt->MirageDisguiseTree : nullptr;
		auto const pImage = pTreeType ? pTreeType->GetImage() : nullptr;
		auto const pCell = pThis->GetCell();
		// Only morph where the viewer has revealed the cell (skip black shroud). The
		// object's own draw handles depth/layer/shroud/occlusion; we only swap pixels.
		if (pImage && pCell && !pCell->IsShrouded())
		{
			MirageMorphSHP = pImage;
			MirageMorphPalette = pCell->LightConvert
				? reinterpret_cast<ConvertClass*>(pCell->LightConvert)
				: FileSystem::UNITx_PAL;
		}
	}
	return 0; // let the techno draw itself; CC_Draw_Shape paints it as the tree
}

// Clear the swap when the techno's DrawObject returns (0x706602 = its epilogue:
// pop edi/esi/ebp/ebx; add esp,0x44; ret 0x40), so the swap only applies to the
// disguised object's own sprite blits and never leaks to later draws.
DEFINE_HOOK(0x706602, TechnoClass_DrawObject_MirageDisguise_End, 0x7)
{
	MirageMorphSHP = nullptr;
	return 0;
}

// The sprite-swap itself. CC_Draw_Shape (0x4AED70, __fastcall): ECX=surface,
// EDX=palette, then stack args: [esp+4]=SHP, [esp+8]=FrameIndex, [esp+0xC]=Position,
// [esp+0x10]=Bounds, [esp+0x14]=Flags, [esp+0x18]=Remap, [esp+0x1C]=ZAdjust,
// [esp+0x20]=ZGradient, [esp+0x24]=Brightness, [esp+0x28]=TintColor,
// [esp+0x2C]=ZShape, [esp+0x30]=ZShapeFrame. While a disguised techno draws, paint
// the tree SHP (frame 0) with the cell palette in place of the unit sprite, and make
// it look/behave like real terrain rather than the unit:
//   - zero Remap + TintColor  (the unit's house colour was tinting the tree),
//   - drop the per-pixel Alpha flag (it made the tree look translucent),
//   - feed the tree SHP as the ZShape so the tree's OWN silhouette writes depth —
//     otherwise the unit's small footprint was used and the cell behind clipped the
//     tree's overhang ("part of the tree cut to the shape of the cell behind").
DEFINE_HOOK(0x4AED70, CC_Draw_Shape_MirageSwap, 0x6)
{
	if (MirageMorphSHP)
	{
		auto const shp = reinterpret_cast<DWORD>(MirageMorphSHP);
		R->Stack<DWORD>(0x4, shp);                                     // SHP
		R->Stack<int>(0x8, 0);                                         // FrameIndex
		R->EDX(reinterpret_cast<DWORD>(MirageMorphPalette));           // Palette
		R->Stack<DWORD>(0x14, R->Stack<DWORD>(0x14)
			& ~static_cast<DWORD>(BlitterFlags::Alpha));               // Flags: drop Alpha
		R->Stack<int>(0x18, 0);                                        // Remap
		R->Stack<int>(0x28, 0);                                        // TintColor
		// NOTE: do NOT set ZShape here — feeding the tree SHP as the Z mask made the
		// blit cull itself (the disguise went fully invisible). The overhang-clipped-
		// by-the-cell-behind issue is handled another way (TODO), not via ZShape.
		(void)shp;
	}
	return 0;
}

// Make a disguised techno non-targetable by ENEMY auto-acquisition, like a real
// mirage (and like the decoy trees, which aren't targetable) — so the enemy can't
// pick the real unit out. TechnoClass::EvaluateObject (0x6F7CA0, __thiscall):
// EDI = the scanning techno, [esp+0x4C] = the candidate (arg4). When the candidate
// is one of our disguised units and the scanner is an enemy (not owner/allied),
// jump to the function's own reject exit 0x6F894F (returns AL=0 = "no target").
// Owner/allies fall through and target it normally. Verified in Ghidra.
// SIZE MUST BE 6, NOT 4. The instruction at 0x6F7CB1 is only 4 bytes
// (8B 74 24 4C = mov esi,[esp+0x4C]), but Syringe always writes a 5-byte
// E9 rel32 and always resumes at addr + max(size,5). With size 4 it wrote over
// the 8B of `mov edx,[edi]` at 0x6F7CB5 and resumed at 0x6F7CB6 — the orphaned
// 17 byte, which decodes as `pop ss` and #GPs on every `return 0` (i.e. almost
// every call). Size 6 subsumes both instructions: the stub copies them and
// resumes at 0x6F7CB7 (`push esi`), an instruction boundary.
DEFINE_HOOK(0x6F7CB1, TechnoClass_EvaluateObject_MirageUntarget, 0x6)
{
	GET(TechnoClass*, pScanner, EDI);
	GET_STACK(TechnoClass*, pCandidate, 0x4C);

	if (!pCandidate || !pScanner)
		return 0;

	auto const pExt = TechnoExt::ExtMap.Find(pCandidate);
	if (!pExt || !pExt->MirageDisguiseActive)
		return 0; // not one of our disguised units

	auto const pScanHouse = pScanner->Owner;
	auto const pCandHouse = pCandidate->Owner;
	if (!pScanHouse || !pCandHouse || pScanHouse == pCandHouse
		|| pScanHouse->IsAlliedWith(pCandHouse))
		return 0; // owner/allies target it normally

	return 0x6F894F; // enemy: the disguised unit is not a valid target
}

// (The old post-pass tree flush at 0x6D95AF is gone — #3 draws the tree inside the
//  object's own draw via CC_Draw_Shape, so there is nothing to flush afterwards.)

// The chevrons/pips/health-bar are drawn in a SEPARATE pass (DrawExtras), which
// the DrawObject skip above misses — enemies could still see the veterancy
// chevron over an "invisible" disguised unit. Skip DrawExtras too.
// 0x6F5190 = TechnoClass::DrawExtras(this=ECX->EBP); hook after its prologue
// (EBP=this) and, for a hidden techno, jump to its clean epilogue 0x6F5EE3
// (pop edi/esi/ebp; add esp,0x8c; ret 8) — the same exit the game's own early
// check uses.
DEFINE_HOOK(0x6F519B, TechnoClass_DrawExtras_MirageDisguise, 0x6)
{
	GET(TechnoClass*, pThis, EBP);
	if (MirageRenderAsTree(pThis)) // hide extras whenever it's drawn as a tree
		return 0x6F5EE3; // skip chevrons/pips/health bar (incl. the owner pulse)
	return 0;
}

// ---------------------------------------------------------------------------
// Fade rendering hooks — inside TerrainClass::Draw.
//   0x71C2BC: ESI = the tree about to be drawn (also a Phobos palette hook —
//             same-address chaining is safe). Decide the decoy fade here while
//             ESI still holds the tree.
//   0x71C2DC: the blit flags have just converged into ESI (0x2E00 / 0x4E00),
//             right before the tree's SHP blit at 0x71C304 (the later 0x71C34E
//             blit is the shadow). OR in the translucency for a fading decoy.
// ---------------------------------------------------------------------------

DEFINE_HOOK(0x71C2BC, TerrainClass_Draw_MirageStash, 0x6)
{
	GET(TerrainClass*, pThis, ESI);

	// Only OUR decoys are in the registry; real map trees fall straight through.
	if (DecoyRegistry.find(pThis) != DecoyRegistry.end())
	{
		// Don't render a decoy in a cell the current viewer can't actually see.
		// Decoys are real TerrainClass objects, so without this they draw through
		// black shroud, gap-generator shroud, and grey fog (and linger as ghosts
		// after an area re-shrouds). Rendering only where there is live vision keeps
		// them from appearing at the enemy's base / over the shroud. Redirect to the
		// draw's own epilogue (0x71C353: pop edi/esi/ebp/ebx; add esp,0x14; ret 8) —
		// the stack here is exactly that frame, so this cleanly skips tree + shadow.
		auto const pCell = pThis->GetCell();
		if (!pCell || pCell->IsShrouded() || pCell->IsFogged())
			return 0x71C353;
	}

	CurrentDecoyBlit = ComputeDecoyBlit(pThis);
	return 0;
}

DEFINE_HOOK(0x71C2DC, TerrainClass_Draw_MirageBlit, 0x6)
{
	if (CurrentDecoyBlit != BlitterFlags::None)
	{
		GET(DWORD, flags, ESI);
		// Add our translucency AND drop the base Alpha flag (0x800): a per-pixel
		// alpha-lighting pass combined with a translucency level renders parts of
		// the tree wrong ("top-right quarter covered by the cell behind"). Only
		// the faded (owner) trees hit this path; solid trees are untouched.
		R->ESI((flags & ~static_cast<DWORD>(BlitterFlags::Alpha))
			| static_cast<DWORD>(CurrentDecoyBlit));
	}
	return 0;
}

// The shadow blit (0x71C34E) reuses the tree's flags in ESI, so our translucency
// leaks onto the shadow and wrecks its darken palette. Strip the translucency
// bits (TransLucent25/50/75 all live in mask 0x6) just AFTER the tree blit
// (0x71C304) and before the shadow path, so only the tree fades.
//
// Hooked at the 5-byte `mov al,[0x822cf1]` right after the tree blit and return
// 0 (safe continuation) — NOT the cramped 3-byte `or esi,1` at 0x71C325, whose
// addr+size return landed on 0x71C328 and crashed (C0000005 @ 0x71C328).
DEFINE_HOOK(0x71C309, TerrainClass_Draw_MirageShadowFix, 0x5)
{
	if (CurrentDecoyBlit != BlitterFlags::None)
	{
		GET(DWORD, flags, ESI);
		R->ESI(flags & ~0x6u); // clear our translucency; game then ORs in Darken
	}
	return 0;
}

// Drop a decoy from the fade registry when the engine destroys it (e.g. shot
// down), so a later tree reusing its address is not mistaken for a decoy.
// Same address as Phobos's TerrainClass NowDead hook — chaining is safe.
DEFINE_HOOK(0x71BB2C, TerrainClass_NowDead_MirageErase, 0x6)
{
	GET(TerrainClass*, pThis, ESI);
	DecoyRegistry.erase(pThis);
	// Also drop it from the deferred-delete queue so we never free it twice.
	auto& q = PendingDecoyDeletes;
	q.erase(std::remove(q.begin(), q.end(), pThis), q.end());
	return 0;
}
