#pragma once

#include <string>
#include <vector>
#include <windows.h>

namespace glass {

struct HookEntry {
  const wchar_t *module_name;
  const wchar_t *symbol;
  DWORD64 rva;
  void *target;
  void *replacement;
  void *original;
  bool installed = false;
};

class DWMHookManager {
public:
  DWMHookManager();
  ~DWMHookManager();

  bool Initialize();
  void Shutdown();

  bool InstallHooks();
  void RemoveHooks();

  static DWMHookManager &Get();

private:
  bool InstallInlineHooks();

private:
  std::vector<HookEntry> m_hooks;
  bool m_initialized = false;
};

} // namespace glass
