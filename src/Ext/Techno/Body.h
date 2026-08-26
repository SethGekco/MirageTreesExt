#pragma once

#include <vector>

#include <TechnoClass.h>
#include <TerrainClass.h>
#include <GeneralStructures.h> // CellStruct

#include <Utilities/Container.h>
#include <Utilities/TemplateDef.h>

// Standalone MirageTreesExt Techno (instance) extension. Tracks the live decoy
// tree objects a disguised techno has spawned around itself. Uses Container<T>
// in unordered_map mode (Canary defined, NO ExtPointerOffset) — coexistence
// safe with Phobos/Ares/Antares.
class TechnoExt
{
public:
	using base_type = TechnoClass;

	static constexpr DWORD Canary = 0x14173EC7;
	// No ExtPointerOffset -> Container uses the unordered_map path.

	class ExtData final : public Extension<TechnoClass>
	{
	public:
		// Live decoy tree objects this techno currently owns. Transient — not
		// serialized in v1; the forest is re-laid on the next still-tick after
		// a load. Pointers are validated against TerrainClass::Array before use
		// (a tree the player destroys is deleted by the engine out from under
		// us).
		std::vector<TerrainClass*> MirageTrees;

		// Whether a decoy forest is currently laid down.
		bool MirageActive;

		// Whether we currently have the techno self-disguised as a tree. Tracked
		// separately from MirageActive so both effects can be on at once.
		bool MirageDisguiseActive;

		// Cell the techno occupied when the forest was laid, so we can detect
		// that it has moved and tear the forest down.
		CellStruct MirageAnchor;

		// Diagnostic: log the "seen a mirage-capable techno" line once only.
		bool MirageDiagLogged;

		ExtData(TechnoClass* OwnerObject) : Extension<TechnoClass>(OwnerObject)
			, MirageTrees {}
			, MirageActive { false }
			, MirageDisguiseActive { false }
			, MirageAnchor {}
			, MirageDiagLogged { false }
		{ }

		virtual ~ExtData() override;

		virtual void InvalidatePointer(void* ptr, bool bRemoved) override;
		virtual void LoadFromStream(PhobosStreamReader& Stm) override;
		virtual void SaveToStream(PhobosStreamWriter& Stm) override;

	private:
		template <typename T>
		void Serialize(T& Stm);
	};

	class ExtContainer final : public Container<TechnoExt>
	{
	public:
		ExtContainer();
		~ExtContainer();
	};

	static ExtContainer ExtMap;

	// ---- Mirage-tree runtime API (implemented in Hooks.MirageTrees.cpp) ----

	// True when this techno should currently be showing a decoy forest:
	// its type HasMirageTrees() and it is disguised/still and alive.
	static bool ShouldHaveMirage(TechnoClass* pThis);

	// Lay down the decoy forest around pThis (idempotent-safe: no-op if already
	// active). Deterministic via ScenarioClass::Random for MP sync.
	static void SpawnMirageTrees(TechnoClass* pThis);

	// Tear down and delete every decoy tree this techno owns.
	static void ClearMirageTrees(TechnoClass* pThis);

	// Same teardown, but operating directly on the ExtData. Used by the ExtData
	// destructor, which cannot ExtMap.Find itself while it is being removed —
	// that lookup returns null, so a tank destroyed while parked (never moving,
	// so the move-triggered clear never ran) would otherwise leak its decoys.
	// deferDelete=true (the dtor path) queues the trees for freeing next frame
	// instead of deleting inline, which would crash the techno's own dtor cascade.
	static void ClearMirageTreesFor(ExtData* pExt, bool deferDelete = false);

	// Per-frame driver: spawn/despawn as the techno's state changes.
	static void UpdateMirageTrees(TechnoClass* pThis);
};
