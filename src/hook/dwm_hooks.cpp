// dwm_hooks.cpp - MinHook-based hooking implementation
#include "dwm_hooks.h"
#include "MinHook.h"
#include "glass_renderer.h"
#include "logging.h"
#include "symfetch_lib.h"
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
    Log("DWMHookManager: MinHook initialization failed");
    return false;
  }

  // Route symfetch_lib diagnostics into the glass logger so they land in the
  // dp8dwmglass.log file alongside everything else.
  sym::SetLogger(&Log);

  m_initialized = true;
  Log("DWMHookManager: Initialized");
  return true;
}

void DWMHookManager::Shutdown() {
  if (!m_initialized)
    return;

  RemoveHooks();
  MH_Uninitialize();

  m_initialized = false;
  Log("DWMHookManager: Shutdown");
}

bool DWMHookManager::InstallHooks() {
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
  Log("DWMHookManager: Installing inline hooks...");

  // rva == 0 means "resolve at install time". A nonzero rva is a pinned
  // override (hand-derived from a specific build) that skips resolution.
  struct InlineHookDef {
    const wchar_t *module;
    const wchar_t *symbol;
    DWORD64 rva;
    void *replacement;
    void **original_out;
  };

  static const InlineHookDef hook_defs[] = {
      {L"dwmcore.dll", L"CDrawingContext::DrawVisualTree", 0,
       (void *)MyDrawVisualTree, (void **)&g_draw_visual_tree_original},
  };

  for (const auto &def : hook_defs) {
    HMODULE module = GetModuleHandleW(def.module);
    if (!module) {
      Log("DWMHookManager: %ls not loaded, skipping", def.module);
      continue;
    }

    DWORD64 rva = def.rva;
    if (rva == 0) {
      if (!sym::ResolveSymbolRva(def.module, def.symbol, &rva)) {
        Log("DWMHookManager: failed to resolve %ls!%ls", def.module,
            def.symbol);
        continue;
      }
    }

    BYTE *found = (BYTE *)module + rva;
    Log("DWMHookManager: %ls!%ls -> RVA 0x%llx, target 0x%p", def.module,
        def.symbol, rva, found);

    HookEntry entry = {};
    entry.module_name = def.module;
    entry.symbol = def.symbol;
    entry.rva = rva;
    entry.target = found;
    entry.replacement = def.replacement;

    MH_STATUS status = MH_CreateHook(found, def.replacement, &entry.original);
    if (status != MH_OK) {
      Log("DWMHookManager: MH_CreateHook failed for %ls: %d", def.symbol,
          (int)status);
      continue;
    }

    status = MH_EnableHook(found);
    if (status != MH_OK) {
      Log("DWMHookManager: MH_EnableHook failed for %ls: %d", def.symbol,
          (int)status);
      MH_RemoveHook(found);
      continue;
    }

    *def.original_out = entry.original;
    entry.installed = true;
    m_hooks.push_back(entry);
    Log("DWMHookManager: Hooked %ls at 0x%p (RVA 0x%llx)", def.symbol, found,
        rva);
  }

  Log("DWMHookManager: InstallInlineHooks done (%d hooks)",
      (int)m_hooks.size());
  return true;
}

DWMHookManager &DWMHookManager::Get() {
  static DWMHookManager instance;
  return instance;
}

// Inline hook replacements

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
