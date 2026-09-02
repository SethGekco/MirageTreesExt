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

	// Our own disguised unit firing → drop its disguise briefly (muzzle blink).
	// Buildings are excluded: a defensive structure fires constantly, so a per-shot
	// reveal just makes it flicker between tree and structure instead of staying a
	// steady tree. Stationary structures keep the disguise up while they fire.
	if (auto const pExt = TechnoExt::ExtMap.Find(pThis))
	{
		auto const pTypeExt = TechnoTypeExt::ExtMap.Find(pThis->GetTechnoType());
		if (pTypeExt && pTypeExt->MirageDisguise && pTypeExt->MirageBlinkOnFire > 0
			&& pThis->WhatAmI() != AbstractType::Building)
			pExt->MirageRevealTimer = pTypeExt->MirageBlinkOnFire;
	}

	// Drop a stale AUTO-target lock. EvaluateObject only blocks NEW acquisition, so
	// an enemy that locked this unit BEFORE it disguised keeps hammering it — the
	// root of the "inconsistent auto-attack". If the firer is auto-attacking (NOT an
	// explicit Attack/force-fire order) a now-disguised enemy, clear its target so
	// it stops and re-evaluates (and can't re-acquire the hidden unit). Explicit
	// orders — Mission::Attack, which is what force-fire uses — are left intact so
	// the player can still Ctrl-fire a disguise.
	if (auto const pTgt = abstract_cast<TechnoClass*>(pTargetAbs))
	{
		auto const pTgtExt = TechnoExt::ExtMap.Find(pTgt);
		if (pTgtExt && pTgtExt->MirageDisguiseActive
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

	// Keep the disguise dropped for as long as the unit holds a target, not just
	// for one blink per shot. A defensive structure (e.g. a pillbox) fires many
	// times a second; blinking per-shot re-added the disguise between shots and
	// flickered constantly. Latching on Target means it stays exposed through the
	// whole engagement and re-disguises BlinkOnFire frames after the target drops.
	if (pTypeExt->MirageBlinkOnFire > 0 && pThis->Target
		&& pThis->WhatAmI() != AbstractType::Building)
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

		// TEMP DIAGNOSTIC: log every disguise state flip for buildings, to measure
		// whether (and how often) a pillbox's disguise is toggling.
		if (pThis->WhatAmI() == AbstractType::Building
			&& shouldDisguise != pExt->MirageDisguiseActive)
		{
			Debug::Log("[MirageBldgDiag] %s frame=%d flip -> %d "
				"(shouldHaveMirage=%d revealed=%d health=%d inLimbo=%d)\n",
				pThis->GetTechnoType()->ID, Unsorted::CurrentFrame,
				shouldDisguise, TechnoExt::ShouldHaveMirage(pThis),
				MirageRevealed(pExt), pThis->Health, pThis->InLimbo);
		}

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
		}

		// The morph tree is only (re)drawn on frames the techno's own DrawObject
		// runs — every frame for foot units (they animate), but buildings only
		// redraw when their cell is dirtied, so a disguised BUILDING's tree blinks
		// on and off. Keep a disguised building marked for redraw each frame so its
		// tree is painted every frame, steady like an infantryman's.
		if (pExt->MirageDisguiseActive && pThis->WhatAmI() == AbstractType::Building)
			pThis->Mark(MarkType::ChangeRedraw);
	}

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

// PURE MORPH: render the chosen tree's sprite at the techno's position, matching
// how the game draws a real tree — DSurface::Temp, the cell's LightConvert palette,
// centered SHP with the terrain blit flags. Then the caller skips the techno's own
// draw so the enemy sees only the tree. (First cut — palette/frame/depth/offset may
// need tuning.)
// Queued disguise-tree sprite draws, flushed AFTER all object layers so nothing
// paints over them (the sprite blits fine — proven by a fixed-position probe —
// but drawing it mid-object-render gets overdrawn by later objects/passes).
namespace
{
	struct MorphDraw { Point2D Pos; SHPStruct* Image; ConvertClass* Palette; };
	std::vector<MorphDraw> PendingMorphDraws;
}

static void DrawMirageTree(TechnoClass* pThis)
{
	auto const pExt = TechnoExt::ExtMap.Find(pThis);
	auto const pTreeType = pExt ? pExt->MirageDisguiseTree : nullptr;
	auto const pImage = pTreeType ? pTreeType->GetImage() : nullptr;
	if (!pImage)
		return;

	auto const client = TacticalClass::Instance->CoordsToClient(pThis->GetCoords());
	if (!client.second)
		return;

	auto const pCell = pThis->GetCell();
	// Don't paint the tree in cells the viewer can't currently see. The tree draws
	// in an on-top post-pass (after the object layers), so without this it shows
	// through both black shroud AND grey fog-of-war — a bright tree floating over
	// unexplored/fogged ground. Skip both states.
	if (!pCell || pCell->IsShrouded() || pCell->IsFogged())
		return;

	auto const pPalette = pCell->LightConvert
		? reinterpret_cast<ConvertClass*>(pCell->LightConvert)
		: FileSystem::UNITx_PAL;

	PendingMorphDraws.push_back({ client.first, pImage, pPalette });
}

DEFINE_HOOK(0x705E15, TechnoClass_DrawObject_MirageDisguise, 0x5)
{
	GET(TechnoClass*, pThis, ESI);
	if (MirageHiddenFromViewer(pThis))
	{
		DrawMirageTree(pThis);   // queue the tree in its place
		return 0x706602;         // skip the techno's own draw for this enemy viewer
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

// Flush queued disguise trees AFTER the tactical object layers are rendered
// (0x6D95AF = right after the 5-layer render loop, before overlays), so the
// sprites are painted on top of the objects instead of being overdrawn.
DEFINE_HOOK(0x6D95AF, TacticalClass_RenderLayers_MorphFlush, 0x5)
{
	if (!PendingMorphDraws.empty())
	{
		RectangleStruct bounds { 0, 0, 4000, 3000 };
		for (auto const& m : PendingMorphDraws)
			DSurface::Temp->DrawSHP(m.Palette, m.Image, 0, &m.Pos, &bounds,
				static_cast<BlitterFlags>(0x0600), 0, 0, ZGradient::Ground, 1000, 0, nullptr, 0, 0, 0);
		PendingMorphDraws.clear();
	}
	return 0;
}

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
	if (MirageHiddenFromViewer(pThis))
		return 0x6F5EE3; // skip chevrons/pips/health bar for this enemy viewer
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
