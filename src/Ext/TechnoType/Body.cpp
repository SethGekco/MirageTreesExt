#include "Body.h"

#include <cstring>

#include <RulesClass.h>
#include <Utilities/Macro.h>
#include <Utilities/Debug.h>

// Map a small set of INI keywords to our enum ints. Case-insensitive; unknown
// values keep the supplied default.
static int ParseKeyword(CCINIClass* pINI, const char* pSection, const char* pKey,
	int fallback, std::initializer_list<const char*> words)
{
	char buffer[32];
	if (pINI->ReadString(pSection, pKey, "", buffer, sizeof(buffer)) <= 0)
		return fallback;

	int i = 0;
	for (auto const word : words)
	{
		if (_stricmp(buffer, word) == 0)
			return i;
		++i;
	}
	return fallback;
}

TechnoTypeExt::ExtContainer TechnoTypeExt::ExtMap;

void TechnoTypeExt::ExtData::LoadFromINIFile(CCINIClass* const pINI)
{
	const char* pSection = this->OwnerObject()->ID;

	if (!pINI->GetSection(pSection))
		return;

	INI_EX exINI(pINI);

	this->MirageDecoys.Read(exINI, pSection, "Mirage.Decoys");
	this->MirageDisguise.Read(exINI, pSection, "Mirage.Disguise");
	this->MirageDefaultDisguises.Read(exINI, pSection, "Mirage.DefaultDisguises");
	this->MirageAttackCursorOnDisguise.Read(exINI, pSection, "Mirage.AttackCursorOnDisguise");
	this->MirageDistance.Read(exINI, pSection, "Mirage.Distance");
	this->MirageCount.Read(exINI, pSection, "Mirage.Count");
	this->MirageHealth.Read(exINI, pSection, "Mirage.Health");

	// Fade config. Keyword enums parsed leniently; numeric knobs via Valueable.
	this->MirageFadeAudience = ParseKeyword(pINI, pSection, "Mirage.FadeAudience",
		this->MirageFadeAudience, { "none", "owner", "allies", "all" });
	this->MirageFadeStyle = ParseKeyword(pINI, pSection, "Mirage.FadeStyle",
		this->MirageFadeStyle, { "none", "pulse", "translucent", "spawn" });
	this->MirageFadeOpacity.Read(exINI, pSection, "Mirage.FadeOpacity");
	this->MirageFadePulseRate.Read(exINI, pSection, "Mirage.FadePulseRate");

	// Diagnostic: only for types that actually participate, so the log stays
	// quiet (the global DefaultMirageDisguises fallback otherwise matches all).
	const auto pType = this->OwnerObject();
	const auto& pool = this->MirageDefaultDisguises.GetElements(
		RulesClass::Instance->DefaultMirageDisguises);
	if (this->HasMirageTrees())
	{
		Debug::Log("[MirageTreesExt] parsed %s: canDisguise=%d disguiseWhenStill=%d "
			"pool=%d count=(%d,%d) dist=(%d,%d) health=%d hasMirage=%d\n",
			pSection, pType->CanDisguise, pType->DisguiseWhenStill,
			static_cast<int>(pool.size()),
			this->MirageCount.Get().X, this->MirageCount.Get().Y,
			this->MirageDistance.Get().X, this->MirageDistance.Get().Y,
			this->MirageHealth.Get(), this->HasMirageTrees());
	}
}

bool TechnoTypeExt::ExtData::HasMirageTrees() const
{
	// Participates if either effect is opted in. Explicit only — otherwise the
	// global DefaultMirageDisguises fallback would make every TechnoType qualify.
	if (!this->MirageDecoys && !this->MirageDisguise)
		return false;

	// Need at least one candidate TerrainType (own pool or vanilla fallback).
	const auto& disguises = this->MirageDefaultDisguises.GetElements(
		RulesClass::Instance->DefaultMirageDisguises);

	return !disguises.empty();
}

template <typename T>
void TechnoTypeExt::ExtData::Serialize(T& Stm)
{
	Stm
		.Process(this->MirageDecoys)
		.Process(this->MirageDisguise)
		.Process(this->MirageDefaultDisguises)
		.Process(this->MirageAttackCursorOnDisguise)
		.Process(this->MirageDistance)
		.Process(this->MirageCount)
		.Process(this->MirageHealth)
		.Process(this->MirageFadeAudience)
		.Process(this->MirageFadeStyle)
		.Process(this->MirageFadeOpacity)
		.Process(this->MirageFadePulseRate)
		;
}

void TechnoTypeExt::ExtData::LoadFromStream(PhobosStreamReader& Stm)
{
	Extension<TechnoTypeClass>::LoadFromStream(Stm);
	this->Serialize(Stm);
}

void TechnoTypeExt::ExtData::SaveToStream(PhobosStreamWriter& Stm)
{
	Extension<TechnoTypeClass>::SaveToStream(Stm);
	this->Serialize(Stm);
}

// ============================================================================
// Container
// ============================================================================

TechnoTypeExt::ExtContainer::ExtContainer()
	: Container("TechnoTypeClass")
{ }

TechnoTypeExt::ExtContainer::~ExtContainer() = default;

// ============================================================================
// Container lifecycle hooks — TechnoTypeClass ctor/dtor/save-load/INI.
// Addresses verified from Phobos (develop) and proven by the sibling DLLs.
// map-mode container: TryAllocate on ctor, Remove on dtor, Prepare/Static on
// save/load, LoadFromINI on the type's INI read.
// ============================================================================

DEFINE_HOOK(0x711835, TechnoTypeClass_CTOR_MTExt, 0x5)
{
	GET(TechnoTypeClass*, pItem, ESI);
	TechnoTypeExt::ExtMap.TryAllocate(pItem);
	return 0;
}

DEFINE_HOOK(0x711AE0, TechnoTypeClass_DTOR_MTExt, 0x5)
{
	GET(TechnoTypeClass*, pItem, ECX);
	TechnoTypeExt::ExtMap.Remove(pItem);
	return 0;
}

DEFINE_HOOK_AGAIN(0x716DC0, TechnoTypeClass_SaveLoad_Prefix_MTExt, 0x5)
DEFINE_HOOK(0x7162F0, TechnoTypeClass_SaveLoad_Prefix_MTExt, 0x6)
{
	GET_STACK(TechnoTypeClass*, pItem, 0x4);
	GET_STACK(IStream*, pStm, 0x8);
	TechnoTypeExt::ExtMap.PrepareStream(pItem, pStm);
	return 0;
}

DEFINE_HOOK(0x716DAC, TechnoTypeClass_Load_Suffix_MTExt, 0xA)
{
	TechnoTypeExt::ExtMap.LoadStatic();
	return 0;
}

DEFINE_HOOK(0x717094, TechnoTypeClass_Save_Suffix_MTExt, 0x5)
{
	TechnoTypeExt::ExtMap.SaveStatic();
	return 0;
}

DEFINE_HOOK(0x716123, TechnoTypeClass_LoadFromINI_MTExt, 0x5)
{
	GET(TechnoTypeClass*, pItem, EBP);
	GET_STACK(CCINIClass*, pINI, 0x380);
	TechnoTypeExt::ExtMap.LoadFromINI(pItem, pINI);
	return 0;
}
