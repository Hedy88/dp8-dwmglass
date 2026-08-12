// dwm_hooks.cpp - MinHook-based hooking implementation
#include "dwm_hooks.h"
#include "MinHook.h"
#include "glass_renderer.h"
#include "pattern_scan.h"
#include <algorithm>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

using namespace glass;

int WINAPI MyDrawTextW(HDC hdc, LPCWSTR lpchText, int cchText, LPRECT lprc,
                       UINT format);
HBITMAP WINAPI MyCreateBitmapW(int nWidth, int nHeight, UINT cPlanes,
                               UINT cBitsPerPel, const void *lpvBits);
HRGN WINAPI MyCreateRoundRectRgn(int left, int top, int right, int bottom,
                                 int width, int height);

static uint64_t __fastcall MyDrawGlass(uint64_t, uint64_t, int64_t, uint64_t);
static uint32_t __fastcall MyDrawVisualTree(int64_t *, int64_t ***, int64_t ***,
                                            float *, uint64_t, char, char);
static uint32_t __fastcall MyRenderDirtyRegion(uint64_t, uint64_t, uint64_t);
static uint32_t __fastcall MyRenderImage(uint64_t, uint64_t, uint64_t,
                                         uint64_t);
static uint32_t __fastcall MyComputeVisibleRegion(uint64_t, uint64_t, uint64_t);
static uint32_t __fastcall MyOcclusionCompute(uint64_t, uint64_t, uint64_t);
static uint32_t __fastcall MyGetOcclusionInfo(uint64_t, uint64_t, uint64_t);
static uint32_t __fastcall MyGetGeometryCurrentValue(uint64_t, uint64_t,
                                                     uint64_t);
static uint32_t __fastcall MyUpdateOcclusionHints(uint64_t, uint64_t, uint64_t);
static uint32_t __fastcall MyAdjustWindowColorization(uint64_t, uint64_t,
                                                      uint64_t);
static uint32_t __fastcall MyValidateResources(uint64_t, uint64_t, uint64_t);

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

  bool ok = true;

  ok &= InstallIATHook(L"udwm.dll", "USER32.dll", "DrawTextW",
                       (void *)MyDrawTextW, (void **)&m_draw_text_w_original);
  ok &= InstallIATHook(L"udwm.dll", "GDI32.dll", "CreateBitmap",
                       (void *)MyCreateBitmapW,
                       (void **)&m_create_bitmap_original);
  ok &= InstallIATHook(L"udwm.dll", "GDI32.dll", "CreateRoundRectRgn",
                       (void *)MyCreateRoundRectRgn,
                       (void **)&m_create_round_rect_rgn_original);

  if (ok) {
    ok &= InstallInlineHooks();
  }

  return ok;
}

void DWMHookManager::RemoveHooks() {
  for (auto &hook : m_hooks) {
    if (hook.installed) {
      MH_RemoveHook(hook.target);
      hook.installed = false;
    }
  }
  m_hooks.clear();
}

bool DWMHookManager::InstallIATHook(const wchar_t *target_module,
                                    const char *from_dll, const char *func_name,
                                    void *replacement, void **original_out) {
  HMODULE hTarget = GetModuleHandleW(target_module);
  if (!hTarget) {
    printf("DWMHookManager: GetModuleHandleW(%ls) failed\n", target_module);
    return false;
  }

  PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)hTarget;
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
    printf("DWMHookManager: Invalid DOS signature\n");
    return false;
  }

  PIMAGE_NT_HEADERS64 nt =
      (PIMAGE_NT_HEADERS64)((BYTE *)hTarget + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) {
    printf("DWMHookManager: Invalid PE signature\n");
    return false;
  }

  DWORD import_rva =
      nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]
          .VirtualAddress;
  if (import_rva == 0) {
    printf("DWMHookManager: No import directory found\n");
    return false;
  }

  PIMAGE_IMPORT_DESCRIPTOR desc =
      (PIMAGE_IMPORT_DESCRIPTOR)((BYTE *)hTarget + import_rva);
  for (; desc->Name; desc++) {
    const char *dll_name = (const char *)((BYTE *)hTarget + desc->Name);
    if (_stricmp(dll_name, from_dll) != 0) {
      continue;
    }

    PIMAGE_THUNK_DATA64 thunk =
        (PIMAGE_THUNK_DATA64)((BYTE *)hTarget + desc->FirstThunk);
    for (; thunk->u1.AddressOfData; thunk++) {
      const char *func = nullptr;
      if (thunk->u1.Ordinal & IMAGE_ORDINAL_FLAG) {
        char ordinal_buf[32];
        sprintf_s(ordinal_buf, "#%u", (unsigned)(thunk->u1.Ordinal & 0xFFFF));
        func = ordinal_buf;
      } else {
        func = (const char *)((BYTE *)hTarget + thunk->u1.AddressOfData + 2);
      }

      if (_stricmp(func, func_name) == 0) {
        void **iat_entry =
            (void **)((BYTE *)hTarget + desc->FirstThunk +
                      ((BYTE *)thunk -
                       (BYTE *)((BYTE *)hTarget + desc->FirstThunk)));

        if (*iat_entry == replacement) {
          printf("DWMHookManager: %s!%s already hooked\n", from_dll, func_name);
          if (original_out)
            *original_out = *iat_entry;
          return true;
        }

        DWORD old_protect;
        VirtualProtect(iat_entry, sizeof(void *), PAGE_READWRITE, &old_protect);
        if (original_out)
          *original_out = *iat_entry;
        *iat_entry = replacement;
        VirtualProtect(iat_entry, sizeof(void *), old_protect, &old_protect);
        FlushInstructionCache(GetCurrentProcess(), nullptr, 0);

        printf("DWMHookManager: %s!%s in %ls -> 0x%p (was 0x%p)\n", from_dll,
               func_name, target_module, replacement, *original_out);
        return true;
      }
    }
  }

  printf("DWMHookManager: %s!%s not found in %ls\n", from_dll, func_name,
         target_module);
  return false;
}

bool DWMHookManager::InstallInlineHooks() {
  printf("DWMHookManager: Installing inline hooks...\n");

  struct InlineHookDef {
    const wchar_t *module;
    const wchar_t *pattern;
    void *replacement;
  };

  static const InlineHookDef hook_defs[] = {
      {L"dwmcore.dll", L"CDrawingContext::DrawGlass", (void *)MyDrawGlass},
      {L"dwmcore.dll", L"CDrawingContext::DrawVisualTree",
       (void *)MyDrawVisualTree},
      {L"dwmcore.dll", L"CHwndRenderTarget::RenderDirtyRegion",
       (void *)MyRenderDirtyRegion},
      {L"dwmcore.dll", L"CWindowNode::RenderImage", (void *)MyRenderImage},
      {L"dwmcore.dll", L"CArrayBasedCoverageSet::ComputeVisibleRegion",
       (void *)MyComputeVisibleRegion},
      {L"dwmcore.dll", L"COcclusionContext::Compute",
       (void *)MyOcclusionCompute},
      {L"dwmcore.dll", L"CVisual::GetOcclusionInfo",
       (void *)MyGetOcclusionInfo},
      {L"dwmcore.dll", L"GetGeometryCurrentValue",
       (void *)MyGetGeometryCurrentValue},
      {L"udwm.dll", L"CTopLevelWindow::UpdateOcclusionHints",
       (void *)MyUpdateOcclusionHints},
      {L"udwm.dll", L"CGlassColorizationParameters::AdjustWindowColorization",
       (void *)MyAdjustWindowColorization},
      {L"udwm.dll", L"CText::ValidateResources", (void *)MyValidateResources},
  };

  for (const auto &def : hook_defs) {
    BYTE *found = PatternScan(def.module, def.pattern);
    if (!found) {
      printf("DWMHookManager: Failed to locate %ls in %ls\n", def.pattern,
             def.module);
      continue;
    }

    HookEntry entry = {};
    entry.module_name = def.module;
    entry.pattern = def.pattern;
    entry.target = found;
    entry.replacement = def.replacement;

    MH_STATUS status = MH_CreateHook(found, def.replacement, &entry.original);
    if (status != MH_OK) {
      printf("DWMHookManager: MH_CreateHook failed for %ls: %d\n", def.pattern,
             status);
      continue;
    }

    status = MH_EnableHook(found);
    if (status != MH_OK) {
      printf("DWMHookManager: MH_EnableHook failed for %ls: %d\n", def.pattern,
             status);
      MH_RemoveHook(found);
      continue;
    }

    entry.installed = true;
    m_hooks.push_back(entry);
    printf("DWMHookManager: Hooked %ls at 0x%p\n", def.pattern, found);
  }

  return true;
}

bool DWMHookManager::InstallInlineHook(const wchar_t *module,
                                       const wchar_t *pattern,
                                       void *replacement, void **original_out) {
  BYTE *found = PatternScan(module, pattern);
  if (!found) {
    printf("DWMHookManager: Pattern not found: %ls in %ls\n", pattern, module);
    return false;
  }

  MH_STATUS status = MH_CreateHook(found, replacement, original_out);
  if (status != MH_OK) {
    printf("DWMHookManager: MH_CreateHook failed: %d\n", status);
    return false;
  }

  status = MH_EnableHook(found);
  if (status != MH_OK) {
    printf("DWMHookManager: MH_EnableHook failed: %d\n", status);
    MH_RemoveHook(found);
    return false;
  }

  printf("DWMHookManager: Installed inline hook for %ls at 0x%p\n", pattern,
         found);
  return true;
}

void *DWMHookManager::GetOriginal(const char *func_name) const {
  if (strcmp(func_name, "DrawTextW") == 0)
    return m_draw_text_w_original;
  if (strcmp(func_name, "CreateBitmap") == 0)
    return m_create_bitmap_original;
  if (strcmp(func_name, "CreateRoundRectRgn") == 0)
    return m_create_round_rect_rgn_original;
  return nullptr;
}

void *DWMHookManager::GetOriginalByPattern(const wchar_t *pattern) const {
  for (const auto &hook : m_hooks) {
    if (hook.installed && hook.pattern && wcscmp(hook.pattern, pattern) == 0) {
      return hook.original;
    }
  }
  return nullptr;
}

DWMHookManager &DWMHookManager::Get() {
  static DWMHookManager instance;
  return instance;
}

// ---------------------------------------------------------------------------
// Replacement function implementations
// ---------------------------------------------------------------------------

int WINAPI MyDrawTextW(HDC hdc, LPCWSTR lpchText, int cchText, LPRECT lprc,
                       UINT format) {
  void *orig = DWMHookManager::Get().GetOriginal("DrawTextW");
  if (!orig)
    return 0;
  return ((int(WINAPI *)(HDC, LPCWSTR, int, LPRECT, UINT))orig)(
      hdc, lpchText, cchText, lprc, format);
}

HBITMAP WINAPI MyCreateBitmapW(int nWidth, int nHeight, UINT cPlanes,
                               UINT cBitsPerPel, const void *lpvBits) {
  void *orig = DWMHookManager::Get().GetOriginal("CreateBitmap");
  if (!orig)
    return nullptr;
  return ((HBITMAP(WINAPI *)(int, int, UINT, UINT, const void *))orig)(
      nWidth, nHeight, cPlanes, cBitsPerPel, lpvBits);
}

HRGN WINAPI MyCreateRoundRectRgn(int left, int top, int right, int bottom,
                                 int width, int height) {
  void *orig = DWMHookManager::Get().GetOriginal("CreateRoundRectRgn");
  if (!orig)
    return nullptr;
  return ((HRGN(WINAPI *)(int, int, int, int, int, int))orig)(
      left, top, right, bottom, width, height);
}

// ---------------------------------------------------------------------------
// Inline hook replacements
// ---------------------------------------------------------------------------

// CDrawingContext::DrawGlass
// From Ghidra: undefined8 FUN_180007bd0(undefined8 param_1,undefined8
// param_2,longlong param_3,undefined8 param_4)
static uint64_t __fastcall MyDrawGlass(uint64_t a1, uint64_t a2, int64_t a3,
                                       uint64_t a4) {
  // Glass colorization setup
  extern GlassRenderer g_renderer;
  g_renderer.SetParams(g_renderer.GetParams());

  // Call original via trampoline
  typedef uint64_t(__fastcall * DrawGlass_t)(uint64_t, uint64_t, int64_t,
                                             uint64_t);
  static DrawGlass_t original =
      (DrawGlass_t)DWMHookManager::Get().GetOriginalByPattern(
          L"CDrawingContext::DrawGlass");
  if (original)
    return original(a1, a2, a3, a4);
  return 0;
}

// CDrawingContext::DrawVisualTree
// From Ghidra: uint FUN_180007630(longlong *param_1,longlong
// ****param_2,longlong ****param_3,float *param_4,undefined8 param_5,char
// param_6,char param_7)
static uint32_t __fastcall MyDrawVisualTree(int64_t *param_1,
                                            int64_t ***param_2,
                                            int64_t ***param_3, float *param_4,
                                            uint64_t param_5, char param_6,
                                            char param_7) {
  extern GlassRenderer g_renderer;

  // If this is the main render pass and glass is enabled
  if (param_6 == 0 && param_7 != 0) {
    GlassParams params = g_renderer.GetParams();
    if (params.enabled) {
      // Get window rect
      HWND hwnd = GetAncestor(GetForegroundWindow(), GA_ROOT);
      if (hwnd) {
        RECT window_rect = {};
        GetWindowRect(hwnd, &window_rect);

        // Compute glass area (top portion of window)
        RECT glass_rect = {window_rect.left, window_rect.top, window_rect.right,
                           window_rect.top +
                               (std::max)(32, params.glass_margins.top)};

        // Render glass
        RECT client_rect = {};
        GetClientRect(hwnd, &client_rect);
        g_renderer.RenderGlass(hwnd, glass_rect, client_rect);
      }
    }
  }

  // Call original via trampoline
  typedef uint32_t(__fastcall * DrawVisualTree_t)(
      int64_t *, int64_t ***, int64_t ***, float *, uint64_t, char, char);
  static DrawVisualTree_t original =
      (DrawVisualTree_t)DWMHookManager::Get().GetOriginalByPattern(
          L"CDrawingContext::DrawVisualTree");
  if (original)
    return original(param_1, param_2, param_3, param_4, param_5, param_6,
                    param_7);
  return 0;
}

// CHwndRenderTarget::RenderDirtyRegion
static uint32_t __fastcall MyRenderDirtyRegion(uint64_t a1, uint64_t a2,
                                               uint64_t a3) {
  typedef uint32_t(__fastcall * RenderDirtyRegion_t)(uint64_t, uint64_t,
                                                     uint64_t);
  static RenderDirtyRegion_t original =
      (RenderDirtyRegion_t)DWMHookManager::Get().GetOriginalByPattern(
          L"CHwndRenderTarget::RenderDirtyRegion");
  if (original)
    return original(a1, a2, a3);
  return 0;
}

// CWindowNode::RenderImage
static uint32_t __fastcall MyRenderImage(uint64_t a1, uint64_t a2, uint64_t a3,
                                         uint64_t a4) {
  typedef uint32_t(__fastcall * RenderImage_t)(uint64_t, uint64_t, uint64_t,
                                               uint64_t);
  static RenderImage_t original =
      (RenderImage_t)DWMHookManager::Get().GetOriginalByPattern(
          L"CWindowNode::RenderImage");
  if (original)
    return original(a1, a2, a3, a4);
  return 0;
}

// CArrayBasedCoverageSet::ComputeVisibleRegion
static uint32_t __fastcall MyComputeVisibleRegion(uint64_t a1, uint64_t a2,
                                                  uint64_t a3) {
  typedef uint32_t(__fastcall * ComputeVisibleRegion_t)(uint64_t, uint64_t,
                                                        uint64_t);
  static ComputeVisibleRegion_t original =
      (ComputeVisibleRegion_t)DWMHookManager::Get().GetOriginalByPattern(
          L"CArrayBasedCoverageSet::ComputeVisibleRegion");
  if (original)
    return original(a1, a2, a3);
  return 0;
}

// COcclusionContext::Compute
static uint32_t __fastcall MyOcclusionCompute(uint64_t a1, uint64_t a2,
                                              uint64_t a3) {
  typedef uint32_t(__fastcall * OcclusionCompute_t)(uint64_t, uint64_t,
                                                    uint64_t);
  static OcclusionCompute_t original =
      (OcclusionCompute_t)DWMHookManager::Get().GetOriginalByPattern(
          L"COcclusionContext::Compute");
  if (original)
    return original(a1, a2, a3);
  return 0;
}

// CVisual::GetOcclusionInfo
static uint32_t __fastcall MyGetOcclusionInfo(uint64_t a1, uint64_t a2,
                                              uint64_t a3) {
  typedef uint32_t(__fastcall * GetOcclusionInfo_t)(uint64_t, uint64_t,
                                                    uint64_t);
  static GetOcclusionInfo_t original =
      (GetOcclusionInfo_t)DWMHookManager::Get().GetOriginalByPattern(
          L"CVisual::GetOcclusionInfo");
  if (original)
    return original(a1, a2, a3);
  return 0;
}

// GetGeometryCurrentValue
static uint32_t __fastcall MyGetGeometryCurrentValue(uint64_t a1, uint64_t a2,
                                                     uint64_t a3) {
  typedef uint32_t(__fastcall * GetGeometryCurrentValue_t)(uint64_t, uint64_t,
                                                           uint64_t);
  static GetGeometryCurrentValue_t original =
      (GetGeometryCurrentValue_t)DWMHookManager::Get().GetOriginalByPattern(
          L"GetGeometryCurrentValue");
  if (original)
    return original(a1, a2, a3);
  return 0;
}

// CTopLevelWindow::UpdateOcclusionHints
static uint32_t __fastcall MyUpdateOcclusionHints(uint64_t a1, uint64_t a2,
                                                  uint64_t a3) {
  typedef uint32_t(__fastcall * UpdateOcclusionHints_t)(uint64_t, uint64_t,
                                                        uint64_t);
  static UpdateOcclusionHints_t original =
      (UpdateOcclusionHints_t)DWMHookManager::Get().GetOriginalByPattern(
          L"CTopLevelWindow::UpdateOcclusionHints");
  if (original)
    return original(a1, a2, a3);
  return 0;
}

// CGlassColorizationParameters::AdjustWindowColorization
static uint32_t __fastcall MyAdjustWindowColorization(uint64_t a1, uint64_t a2,
                                                      uint64_t a3) {
  // From Ghidra: blends accent color into glass base color at 60% strength
  typedef uint32_t(__fastcall * AdjustWindowColorization_t)(uint64_t, uint64_t,
                                                            uint64_t);
  static AdjustWindowColorization_t original =
      (AdjustWindowColorization_t)DWMHookManager::Get().GetOriginalByPattern(
          L"CGlassColorizationParameters::AdjustWindowColorization");

  // Call original to preserve DWM colorization
  if (original) {
    uint32_t result = original(a1, a2, a3);

    // Apply our tint from theme
    extern GlassRenderer g_renderer;
    GlassParams params = g_renderer.GetParams();

    // Note: actual color modification would happen in the params struct
    // The original function modifies the colorization context in place

    return result;
  }
  return 0;
}

// CText::ValidateResources
static uint32_t __fastcall MyValidateResources(uint64_t a1, uint64_t a2,
                                               uint64_t a3) {
  typedef uint32_t(__fastcall * ValidateResources_t)(uint64_t, uint64_t,
                                                     uint64_t);
  static ValidateResources_t original =
      (ValidateResources_t)DWMHookManager::Get().GetOriginalByPattern(
          L"CText::ValidateResources");
  if (original)
    return original(a1, a2, a3);
  return 0;
}
