#pragma once

#include <string>
#include <vector>
#include <windows.h>

namespace glass {

struct HookEntry {
  const wchar_t *module_name;
  const wchar_t *pattern;
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

  bool InstallIATHook(const wchar_t *target_module, const char *from_dll,
                      const char *func_name, void *replacement,
                      void **original_out);

  bool InstallInlineHook(const wchar_t *module, const wchar_t *pattern,
                         void *replacement, void **original_out);

  void *GetOriginal(const char *func_name) const;
  void *GetOriginalByPattern(const wchar_t *pattern) const;

  static DWMHookManager &Get();

private:
  bool ScanForFunctions();
  bool InstallInlineHooks();

private:
  std::vector<HookEntry> m_hooks;
  bool m_initialized = false;

  void *m_draw_text_w_original = nullptr;
  void *m_create_bitmap_original = nullptr;
  void *m_create_round_rect_rgn_original = nullptr;
};

} // namespace glass
