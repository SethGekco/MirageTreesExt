#include "Body.h"

#include <Utilities/Macro.h>

TechnoExt::ExtContainer TechnoExt::ExtMap;

TechnoExt::ExtData::~ExtData()
{
	// Make sure a dying techno never leaves orphaned decoy trees behind. Operate
	// on this ext directly — ExtMap.Find would fail mid-removal — and DEFER the
	// actual frees: deleting objects inside this destructor cascade crashes.
	TechnoExt::ClearMirageTreesFor(this, true);
}

void TechnoExt::ExtData::InvalidatePointer(void* ptr, bool bRemoved)
{
	// A decoy tree destroyed elsewhere (shot down, crushed, map cleanup) must
	// be dropped from our tracking list so we never dereference it.
	auto& trees = this->MirageTrees;
	trees.erase(std::remove(trees.begin(), trees.end(), ptr), trees.end());
}

template <typename T>
void TechnoExt::ExtData::Serialize(T& Stm)
{
	// v1: MirageTrees state is transient (see Body.h). Nothing serialized; the
	// forest is re-laid on the next still-tick after load.
	Stm
		.Process(this->MirageActive)
		.Process(this->MirageAnchor)
		.Process(this->MirageRevealTimer)
		;
}

void TechnoExt::ExtData::LoadFromStream(PhobosStreamReader& Stm)
{
	Extension<TechnoClass>::LoadFromStream(Stm);
	this->Serialize(Stm);
	// Pointers were not saved; start clean so a fresh forest is laid.
	this->MirageTrees.clear();
	this->MirageActive = false;
}

void TechnoExt::ExtData::SaveToStream(PhobosStreamWriter& Stm)
{
	Extension<TechnoClass>::SaveToStream(Stm);
	this->Serialize(Stm);
}

// ============================================================================
// Container
// ============================================================================

TechnoExt::ExtContainer::ExtContainer()
	: Container("TechnoClass")
{ }

TechnoExt::ExtContainer::~ExtContainer() = default;

// ============================================================================
// Container lifecycle hooks — TechnoClass ctor/dtor/save-load.
// Addresses verified from Phobos (develop) and proven by the sibling DLLs.
// ============================================================================

DEFINE_HOOK(0x6F3260, TechnoClass_CTOR_MTExt, 0x5)
{
	GET(TechnoClass*, pItem, ESI);
	TechnoExt::ExtMap.TryAllocate(pItem);
	return 0;
}

DEFINE_HOOK(0x6F4500, TechnoClass_DTOR_MTExt, 0x5)
{
	GET(TechnoClass*, pItem, ECX);
	TechnoExt::ExtMap.Remove(pItem);
	return 0;
}

DEFINE_HOOK_AGAIN(0x70C250, TechnoClass_SaveLoad_Prefix_MTExt, 0x8)
DEFINE_HOOK(0x70BF50, TechnoClass_SaveLoad_Prefix_MTExt, 0x5)
{
	GET_STACK(TechnoClass*, pItem, 0x4);
	GET_STACK(IStream*, pStm, 0x8);
	TechnoExt::ExtMap.PrepareStream(pItem, pStm);
	return 0;
}

DEFINE_HOOK(0x70C249, TechnoClass_Load_Suffix_MTExt, 0x5)
{
	TechnoExt::ExtMap.LoadStatic();
	return 0;
}

DEFINE_HOOK(0x70C264, TechnoClass_Save_Suffix_MTExt, 0x5)
{
	TechnoExt::ExtMap.SaveStatic();
	return 0;
}
