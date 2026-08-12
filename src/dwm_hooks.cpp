// dwm_hooks.cpp - MinHook-based hooking implementation
#include "dwm_hooks.h"
#include "MinHook.h"
#include "glass_renderer.h"
#include "logging.h"
#include "pattern_scan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

using namespace glass;

// CDrawingContext::DrawVisualTree
// From Ghidra: uint FUN_180007630(longlong *param_1,longlong
// ****param_2,longlong ****param_3,float *param_4,undefined8 param_5,char
// param_6,char param_7)
typedef uint32_t(__fastcall * DrawVisualTreeFn)(int64_t *, int64_t ***,
                                                int64_t ***, float *, uint64_t,
                                                char, char);
static uint32_t __fastcall MyDrawVisualTree(int64_t *, int64_t ***, int64_t ***,
                                            float *, uint64_t, char, char);

// Originals are captured once at install time and stored in plain globals.
// This keeps the render path free of lazy static lookup and makes
// RemoveHooks() -> InstallHooks() cycles safe.
static DrawVisualTreeFn g_draw_visual_tree_original = nullptr;

DWMHookManager::DWMHookManager() {}

DWMHookManager::~DWMHookManager() { Shutdown(); }

bool DWMHookManager::Initialize() {
  if (m_initialized)
    return true;

  if (MH_Initialize() != MH_OK) {
    printf("DWMHookManager: MinHook initialization failed\n");
    return false;
  }

  m_initialized = true;
  printf("DWMHookManager: Initialized\n");
  return true;
}

void DWMHookManager::Shutdown() {
  if (!m_initialized)
    return;

  RemoveHooks();
  MH_Uninitialize();

  m_initialized = false;
  printf("DWMHookManager: Shutdown\n");
}

bool DWMHookManager::InstallHooks() {
  printf("DWMHookManager: Installing hooks...\n");
  Log("DWMHookManager: Installing hooks...");
  return InstallInlineHooks();
}

void DWMHookManager::RemoveHooks() {
  for (auto &hook : m_hooks) {
    if (hook.installed) {
      MH_RemoveHook(hook.target);
      hook.installed = false;
    }
  }
  g_draw_visual_tree_original = nullptr;
  m_hooks.clear();
}

bool DWMHookManager::InstallInlineHooks() {
  printf("DWMHookManager: Installing inline hooks...\n");
  Log("DWMHookManager: Installing inline hooks...");

  struct InlineHookDef {
    const wchar_t *module;
    const wchar_t *pattern;
    void *replacement;
    void **original_out;
  };

  static const InlineHookDef hook_defs[] = {
      {L"dwmcore.dll", L"CDrawingContext::DrawVisualTree",
       (void *)MyDrawVisualTree, (void **)&g_draw_visual_tree_original},
  };

  for (const auto &def : hook_defs) {
    BYTE *found = PatternScan(def.module, def.pattern);
    if (!found) {
      printf("DWMHookManager: Failed to locate %ls in %ls\n", def.pattern,
             def.module);
      Log("DWMHookManager: PatternScan FAILED for %ls in %ls", def.pattern,
          def.module);
      continue;
    }
    Log("DWMHookManager: PatternScan ok: %ls at 0x%p", def.pattern, found);

    HookEntry entry = {};
    entry.module_name = def.module;
    entry.pattern = def.pattern;
    entry.target = found;
    entry.replacement = def.replacement;

    MH_STATUS status = MH_CreateHook(found, def.replacement, &entry.original);
    if (status != MH_OK) {
      printf("DWMHookManager: MH_CreateHook failed for %ls: %d\n", def.pattern,
             status);
      Log("DWMHookManager: MH_CreateHook failed for %ls: %d", def.pattern,
          (int)status);
      continue;
    }

    status = MH_EnableHook(found);
    if (status != MH_OK) {
      printf("DWMHookManager: MH_EnableHook failed for %ls: %d\n", def.pattern,
             status);
      Log("DWMHookManager: MH_EnableHook failed for %ls: %d", def.pattern,
          (int)status);
      MH_RemoveHook(found);
      continue;
    }

    *def.original_out = entry.original;
    entry.installed = true;
    m_hooks.push_back(entry);
    printf("DWMHookManager: Hooked %ls at 0x%p\n", def.pattern, found);
    Log("DWMHookManager: Hooked %ls at 0x%p", def.pattern, found);
  }

  Log("DWMHookManager: InstallInlineHooks done (%d hooks)",
      (int)m_hooks.size());
  return true;
}

DWMHookManager &DWMHookManager::Get() {
  static DWMHookManager instance;
  return instance;
}

// ---------------------------------------------------------------------------
// Inline hook replacements
// ---------------------------------------------------------------------------

// CDrawingContext::DrawVisualTree
// Triggers the glass render for the current frame using cached geometry. No
// user32 calls (GetForegroundWindow/GetWindowRect/GetClientRect) on the DWM
// render thread - the region to glass is supplied via GlassRenderer::SetGlassRect
// instead. Then forwards to the original.
static uint32_t __fastcall MyDrawVisualTree(int64_t *param_1,
                                            int64_t ***param_2,
                                            int64_t ***param_3, float *param_4,
                                            uint64_t param_5, char param_6,
                                            char param_7) {
  if (param_6 == 0 && param_7 != 0) {
    extern GlassRenderer g_renderer;
    if (g_renderer.GetParams().enabled)
      g_renderer.RenderGlassFrame();
  }

  if (g_draw_visual_tree_original) {
    return g_draw_visual_tree_original(param_1, param_2, param_3, param_4,
                                       param_5, param_6, param_7);
  }
  return 0; // S_OK no-op fallback (never reached while hook is installed)
}
