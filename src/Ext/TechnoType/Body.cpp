#include "Body.h"

#include <RulesClass.h>
#include <Utilities/Macro.h>
#include <Utilities/Debug.h>

TechnoTypeExt::ExtContainer TechnoTypeExt::ExtMap;

void TechnoTypeExt::ExtData::LoadFromINIFile(CCINIClass* const pINI)
{
	const char* pSection = this->OwnerObject()->ID;

	if (!pINI->GetSection(pSection))
		return;

	INI_EX exINI(pINI);

	this->MirageDefaultDisguises.Read(exINI, pSection, "Mirage.DefaultDisguises");
	this->MirageAttackCursorOnDisguise.Read(exINI, pSection, "Mirage.AttackCursorOnDisguise");
	this->MirageDistance.Read(exINI, pSection, "Mirage.Distance");
	this->MirageCount.Read(exINI, pSection, "Mirage.Count");
	this->MirageHealth.Read(exINI, pSection, "Mirage.Health");

	// Diagnostic: only for disguise-capable types (rare) so the log stays quiet.
	const auto pType = this->OwnerObject();
	const auto& pool = this->MirageDefaultDisguises.GetElements(
		RulesClass::Instance->DefaultMirageDisguises);
	if (pType->CanDisguise || !pool.empty())
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
	const auto pType = this->OwnerObject();

	// Must be able to disguise while still — this is the trigger condition for
	// laying down the decoy forest, and mirrors the vanilla mirage semantics.
	if (!pType->CanDisguise || !pType->DisguiseWhenStill)
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
		.Process(this->MirageDefaultDisguises)
		.Process(this->MirageAttackCursorOnDisguise)
		.Process(this->MirageDistance)
		.Process(this->MirageCount)
		.Process(this->MirageHealth)
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
