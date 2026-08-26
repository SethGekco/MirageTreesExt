#pragma once

#include <Utilities/Container.h>
#include <Utilities/TemplateDef.h>

#include <TechnoTypeClass.h>
#include <TerrainTypeClass.h>

// Standalone MirageTreesExt TechnoType extension. Holds the per-TechnoType
// Mirage.* configuration for the decoy-forest system. Uses Container<T> in
// unordered_map mode (Canary defined, NO ExtPointerOffset) so we claim no
// pointer slot inside TechnoTypeClass and never collide with Phobos/Ares/
// Antares extension storage. Same coexistence trick the sibling DLLs use.
class TechnoTypeExt
{
public:
	using base_type = TechnoTypeClass;

	// Unique canary (distinct from Phobos 0x11111111, TechnoAttachmentExt
	// 0x0A77AC77, AITriggerTypeExt's, etc.).
	static constexpr DWORD Canary = 0x14173E37;
	// No ExtPointerOffset -> Container uses the unordered_map path.

	class ExtData final : public Extension<TechnoTypeClass>
	{
	public:
		// The two effects are INDEPENDENT — a type can enable either or both:
		//
		// Mirage.Decoys : spawn separate real tree objects (the decoy forest)
		//   around the techno. Works for any TechnoType.
		Valueable<bool> MirageDecoys;
		//
		// Mirage.Disguise : the techno itself renders AS a tree (vanilla mirage
		//   self-disguise). Units do this natively; infantry via the shared
		//   disguise draw; buildings need the dedicated building draw hook.
		Valueable<bool> MirageDisguise;

		// Mirage.DefaultDisguises=TREE01,TREE02,... : pool of TerrainTypes the
		// decoy trees are drawn from. Falls back to the vanilla
		// [General]DefaultMirageDisguises when unset (via GetElements).
		NullableVector<TerrainTypeClass*> MirageDefaultDisguises;

		// Mirage.AttackCursorOnDisguise : whether the disguised techno still
		// shows an attack cursor (so the player can still command it).
		Valueable<bool> MirageAttackCursorOnDisguise;

		// Mirage.Distance=min,max : spread radius (in cells) of the decoy trees
		// around the techno. X=min, Y=max.
		Valueable<Point2D> MirageDistance;

		// Mirage.Count=min,max : how many decoy trees to spawn. X=min, Y=max.
		Valueable<Point2D> MirageCount;

		// Mirage.Health : hitpoints of each decoy tree (0 => use the
		// TerrainType's own Strength). "damage before they vanish".
		Valueable<int> MirageHealth;

		// --- Fade rendering (owner/allies see decoys as translucent/pulsing so
		//     they can tell their own fakes from real trees; enemies see solid).
		//     All customizable; feature is OFF by default (FadeStyle=none). ---

		// Mirage.FadeAudience : who sees the fade. none|owner|allies|all.
		//   0=none 1=owner 2=owner+allies 3=all
		Valueable<int> MirageFadeAudience;

		// Mirage.FadeStyle : how the fade looks. none|pulse|translucent|spawn.
		//   0=none(solid) 1=pulse(oscillate) 2=translucent(fixed) 3=spawn(fade-in)
		Valueable<int> MirageFadeStyle;

		// Mirage.FadeOpacity : target opacity 0..100 for translucent/spawn
		//   (100=solid). Snapped to the engine's 25/50/75 translucency steps.
		Valueable<int> MirageFadeOpacity;

		// Mirage.FadePulseRate : frames per translucency step when pulsing.
		Valueable<int> MirageFadePulseRate;

		ExtData(TechnoTypeClass* OwnerObject) : Extension<TechnoTypeClass>(OwnerObject)
			, MirageDecoys { false }
			, MirageDisguise { false }
			, MirageDefaultDisguises {}
			, MirageAttackCursorOnDisguise { true }
			, MirageDistance { { 0, 0 } }
			, MirageCount { { 1, 1 } }
			, MirageHealth { 0 }
			, MirageFadeAudience { 2 }   // owner + allies
			, MirageFadeStyle { 0 }      // off (solid) unless configured
			, MirageFadeOpacity { 50 }
			, MirageFadePulseRate { 15 }
		{ }

		virtual ~ExtData() = default;

		virtual void LoadFromINIFile(CCINIClass* pINI) override;
		virtual void Initialize() override { }
		virtual void InvalidatePointer(void* ptr, bool bRemoved) override { }

		virtual void LoadFromStream(PhobosStreamReader& Stm) override;
		virtual void SaveToStream(PhobosStreamWriter& Stm) override;

		// True if this TechnoType participates in the Mirage-tree decoy system:
		// it can disguise, disguises when still, and has a non-empty disguise
		// pool (own or the vanilla global fallback).
		bool HasMirageTrees() const;

	private:
		template <typename T>
		void Serialize(T& Stm);
	};

	class ExtContainer final : public Container<TechnoTypeExt>
	{
	public:
		ExtContainer();
		~ExtContainer();
	};

	static ExtContainer ExtMap;
};
