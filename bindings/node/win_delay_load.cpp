// Electron exports Node-API from its executable rather than node.dll.
#include <windows.h>
#include <delayimp.h>
#include <cstring>
static FARPROC WINAPI resolve_node(unsigned notification, PDelayLoadInfo info) {
  if (notification == dliNotePreLoadLibrary &&
      (std::strcmp(info->szDll, "node.exe") == 0 || std::strcmp(info->szDll, "node.dll") == 0))
    return reinterpret_cast<FARPROC>(GetModuleHandleW(nullptr));
  return nullptr;
}
extern "C" PfnDliHook __pfnDliNotifyHook2 = resolve_node;
