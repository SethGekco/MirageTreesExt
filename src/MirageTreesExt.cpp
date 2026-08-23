#include "MirageTreesExt.h"

#include <Phobos.h>
#include <Syringe.h>
#include <Utilities/Patch.h>
#include <Utilities/Debug.h>
#include <Utilities/Macro.h>

HANDLE MirageTreesExtDLL::hInstance = nullptr;

char MirageTreesExtDLL::readBuffer[MirageTreesExtDLL::readLength];
wchar_t MirageTreesExtDLL::wideBuffer[MirageTreesExtDLL::readLength];

void MirageTreesExtDLL::ExeRun()
{
    Patch::ApplyStatic();
}

bool __stdcall DllMain(HANDLE hInstance, DWORD dwReason, LPVOID)
{
    if (dwReason == DLL_PROCESS_ATTACH)
    {
        MirageTreesExtDLL::hInstance = hInstance;
        Phobos::hInstance = hInstance; // needed by Patch::ApplyStatic
    }
    return true;
}

SYRINGE_HANDSHAKE(pInfo)
{
    pInfo->Message = const_cast<char*>("MirageTreesExt");
    return S_OK;
}

// Hook into the game's main loop start so our patches apply at the right time
DEFINE_HOOK(0x7CD810, ExeRun, 0x9)
{
    MirageTreesExtDLL::ExeRun();
    return 0;
}

// Trigger deferred debug log flush after command line parse
DEFINE_HOOK(0x52F639, CmdLineParse, 0x5)
{
    Debug::LogDeferredFinalize();
    return 0;
}
