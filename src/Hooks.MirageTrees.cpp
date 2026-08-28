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

#include <Utilities/Macro.h>
#include <Utilities/Debug.h>

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

	return MirageShouldShow(pThis);
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
	DirtyDecoyArea(cell);
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

	bool const shouldShow = TechnoExt::ShouldHaveMirage(pThis);

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

	// Units self-disguise via the native field (renders as a real tree, safe).
	if (pTypeExt->MirageDisguise && pThis->WhatAmI() == AbstractType::Unit)
		UpdateMirageDisguise(pThis, pExt, pTypeExt);

	// Trees: scattered decoys (Mirage.Decoys) and/or covering disguise for
	// non-units (Mirage.Disguise) share the same spawn/clear machinery.
	bool const coveringDisguise = pTypeExt->MirageDisguise && pThis->WhatAmI() != AbstractType::Unit;
	if (pTypeExt->MirageDecoys || coveringDisguise)
		UpdateDecoyForest(pThis, pExt, pTypeExt);

	// Render-swap disguise (non-units): flag whether the techno should currently
	// be hidden from enemies (still + eligible). The draw hook reads this. The
	// covering tree (placed above) is what the enemy then sees in its place.
	if (coveringDisguise)
		pExt->MirageDisguiseActive = TechnoExt::ShouldHaveMirage(pThis);
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
			// Dirty the whole 3x3 block so the tree's overhang repaints, not just
			// its base cell (which left "half the image" stale).
			DirtyDecoyArea(pTree->GetMapCoords());
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

DEFINE_HOOK(0x705E15, TechnoClass_DrawObject_MirageDisguise, 0x5)
{
	GET(TechnoClass*, pThis, ESI);
	if (MirageHiddenFromViewer(pThis))
		return 0x706602; // skip the entire draw for this enemy viewer
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
