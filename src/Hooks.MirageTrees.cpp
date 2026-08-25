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

#include <TechnoClass.h>
#include <FootClass.h>
#include <BuildingClass.h>
#include <TerrainClass.h>
#include <TerrainTypeClass.h>
#include <CellClass.h>
#include <MapClass.h>
#include <ScenarioClass.h>
#include <RulesClass.h>
#include <Memory.h>

#include <Utilities/Macro.h>
#include <Utilities/Debug.h>

#include <Ext/Techno/Body.h>
#include <Ext/TechnoType/Body.h>

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
	int const poolSize = static_cast<int>(disguises.size());
	if (poolSize <= 0)
		return;

	auto& random = ScenarioClass::Instance->Random;

	CellStruct const anchor = pThis->GetMapCoords();

	Debug::Log("[MirageTreesExt] Spawn %s at (%d,%d) pool=%d\n",
		pThis->GetTechnoType()->ID, anchor.X, anchor.Y, poolSize);

	// Count and per-tree spread bounds (X=min, Y=max).
	int const countMin = pTypeExt->MirageCount.Get().X;
	int const countMax = pTypeExt->MirageCount.Get().Y;
	int const distMin = pTypeExt->MirageDistance.Get().X;
	int const distMax = pTypeExt->MirageDistance.Get().Y;

	int const count = random.RandomRanged(
		std::min(countMin, countMax), std::max(countMin, countMax));

	for (int i = 0; i < count; ++i)
	{
		// Random spread within a box of the chosen radius (random center).
		int const dist = random.RandomRanged(
			std::min(distMin, distMax), std::max(distMin, distMax));
		int const dx = dist > 0 ? random.RandomRanged(-dist, dist) : 0;
		int const dy = dist > 0 ? random.RandomRanged(-dist, dist) : 0;

		CellStruct const target { static_cast<short>(anchor.X + dx),
								  static_cast<short>(anchor.Y + dy) };

		// Never place a decoy on the techno's own cell.
		if (target == anchor)
			continue;

		auto const pCell = MapClass::Instance.TryGetCellAt(target);
		if (!pCell)
			continue;

		// Don't stack on an existing tree.
		if (pCell->GetTerrain(false) != nullptr)
			continue;

		auto const pTerrainType = disguises[random.RandomRanged(0, poolSize - 1)];
		if (!pTerrainType)
			continue;

		// RE-VERIFY #1: ctor @0x71BB90 is expected to place the tree on `target`
		// and register it with the global TerrainClass::Array.
		auto const pTree = CreatePlacedTree(pTerrainType, target);
		if (!pTree)
		{
			Debug::Log("[MirageTreesExt]   alloc FAILED for %s at (%d,%d)\n",
				pTerrainType->ID, target.X, target.Y);
			continue;
		}

		// Mirage.Health: hitpoints per decoy (0 => the TerrainType's own).
		int const health = pTypeExt->MirageHealth > 0
			? pTypeExt->MirageHealth.Get()
			: pTerrainType->Strength;
		pTree->Health = health;

		// RE-VERIFY #1 probe: did the ctor place the tree on the map + Array?
		CellStruct const placed = pTree->GetMapCoords();
		Debug::Log("[MirageTreesExt]   tree %s created@(%d,%d) placed@(%d,%d) "
			"inArray=%d inLimbo=%d hp=%d\n",
			pTerrainType->ID, target.X, target.Y, placed.X, placed.Y,
			TerrainClass::Array.FindItemIndex(pTree) != -1, pTree->InLimbo, health);

		pExt->MirageTrees.push_back(pTree);
	}

	pExt->MirageActive = true;
	pExt->MirageAnchor = anchor;
}

void TechnoExt::ClearMirageTrees(TechnoClass* pThis)
{
	auto const pExt = TechnoExt::ExtMap.Find(pThis);
	if (!pExt)
		return;

	for (auto const pTree : pExt->MirageTrees)
	{
		if (!pTree)
			continue;

		// Only touch trees the engine still knows about (a decoy the player
		// shot down is already gone and was dropped via InvalidatePointer).
		if (TerrainClass::Array.FindItemIndex(pTree) == -1)
			continue;

		// RE-VERIFY #2: detach from the cell then free.
		pTree->Limbo();
		GameDelete(pTree);
	}

	if (!pExt->MirageTrees.empty())
		Debug::Log("[MirageTreesExt] Clear %s: removed %d decoy(s)\n",
			pThis->GetTechnoType()->ID, static_cast<int>(pExt->MirageTrees.size()));

	pExt->MirageTrees.clear();
	pExt->MirageActive = false;
}

// ---------------------------------------------------------------------------
// Per-frame driver
// ---------------------------------------------------------------------------

void TechnoExt::UpdateMirageTrees(TechnoClass* pThis)
{
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

	// Fast reject: nothing to do and nothing laid down.
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
		TechnoExt::ClearMirageTrees(pThis);
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
