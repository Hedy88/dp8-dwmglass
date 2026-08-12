#include "dwm_hooks.h"
#include "glass_renderer.h"
#include "logging.h"
#include "msstyles_parser.h"
#include "theme_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

using namespace glass;
using namespace theme;

GlassRenderer g_renderer;
static ThemeReader g_theme_reader;
static msstyles::MsstylesParser g_msstyles_parser;

static bool g_initialized = false;

static bool InitializeGlass() {
  if (g_initialized)
    return true;

  Log("=== InitializeGlass begin (pid %d) ===", GetCurrentProcessId());

  if (!g_theme_reader.LoadCurrentTheme()) {
    printf("dp8-dwmglass: WARNING - could not load theme, using defaults\n");
    Log("ThemeReader: LoadCurrentTheme FAILED, using defaults");
  }
  Log("ThemeReader: LoadCurrentTheme done");

  // parse msstyles for DP Aero theme colors
  std::wstring theme_path = g_theme_reader.GetCurrentThemePath();
  if (g_msstyles_parser.Open(theme_path)) {
    g_msstyles_parser.ParseResources();
    printf("dp8-dwmglass: Parsed msstyles theme: %ls\n", theme_path.c_str());
    Log("MsstylesParser: parsed theme OK");
  } else {
    Log("MsstylesParser: failed to parse theme");
  }

  // initialize glass renderer
  Log("GlassRenderer: Initialize...");
  if (!g_renderer.Initialize()) {
    printf("dp8-dwmglass: Failed to initialize glass renderer\n");
    Log("GlassRenderer: Initialize FAILED");
    return false;
  }
  Log("GlassRenderer: Initialize done");

  // apply theme colors
  ThemeColors colors = g_theme_reader.GetThemeColors();
  g_renderer.SetThemeColors(colors);

  // apply glass params, tinting the glass with the theme accent color
  GlassParams params = g_theme_reader.GetGlassParams();
  params.tint_color.r = GetRValue(colors.accent) / 255.0f;
  params.tint_color.g = GetGValue(colors.accent) / 255.0f;
  params.tint_color.b = GetBValue(colors.accent) / 255.0f;
  params.tint_color.a = 0.35f;
  g_renderer.SetParams(params);

  // feed the DP Aero glass frame pieces (msstyles parts 11-14) into the
  // renderer so the composite uses the actual theme frame artwork
  GlassRenderer::FrameImageData frames[GlassRenderer::kFramePartCount] = {};
  for (int i = 0; i < GlassRenderer::kFramePartCount; i++) {
    const msstyles::ImageResource *img =
        g_msstyles_parser.GetImage((WORD)(11 + i));
    if (img && !img->data.empty()) {
      frames[i].pixels = img->data.data();
      frames[i].width = img->width;
      frames[i].height = img->height;
    }
  }
  g_renderer.SetFrameImages(frames, GlassRenderer::kFramePartCount);

  // default glass region: full desktop
  RECT screen = {0, 0, GetSystemMetrics(SM_CXSCREEN),
                 GetSystemMetrics(SM_CYSCREEN)};
  g_renderer.SetGlassRect(screen);

  // install DWM hooks
  Log("DWMHookManager: Initialize...");
  if (!DWMHookManager::Get().Initialize()) {
    printf("dp8-dwmglass: Failed to initialize hook manager\n");
    Log("DWMHookManager: Initialize FAILED");
    g_renderer.Shutdown();
    return false;
  }
  Log("DWMHookManager: Initialize done");

  Log("DWMHookManager: InstallHooks...");
  if (!DWMHookManager::Get().InstallHooks()) {
    printf("dp8-dwmglass: Failed to install hooks\n");
    Log("DWMHookManager: InstallHooks FAILED");
    DWMHookManager::Get().Shutdown();
    g_renderer.Shutdown();
    return false;
  }
  Log("DWMHookManager: InstallHooks done");

  g_initialized = true;
  printf("dp8-dwmglass: Initialization complete\n");
  Log("=== InitializeGlass complete ===");
  return true;
}

static void ShutdownGlass() {
  if (!g_initialized)
    return;

  printf("dp8-dwmglass: Shutting down...\n");

  DWMHookManager::Get().RemoveHooks();
  DWMHookManager::Get().Shutdown();

  g_renderer.Shutdown();
  g_msstyles_parser.Close();
  g_theme_reader.Unload();

  g_initialized = false;
  printf("dp8-dwmglass: Shutdown complete\n");
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call,
                      LPVOID lpReserved) {
  switch (ul_reason_for_call) {
  case DLL_PROCESS_ATTACH:
    Log("DllMain: DLL_PROCESS_ATTACH");
    DisableThreadLibraryCalls(hModule);
    if (!InitializeGlass()) {
      printf("dp8-dwmglass: Initialization failed\n");
      Log("DllMain: InitializeGlass FAILED, staying resident for debugging");
    }
    break;

  case DLL_PROCESS_DETACH:
    if (lpReserved == nullptr) {
      ShutdownGlass();
    }
    break;

  case DLL_THREAD_ATTACH:
  case DLL_THREAD_DETACH:
    break;
  }
  return TRUE;
}

extern "C" __declspec(dllexport) void __stdcall RunGlass(HWND hwnd) {
  if (!g_initialized) {
    printf("dp8-dwmglass: Not initialized\n");
    return;
  }

  RECT window_rect = {0};
  GetWindowRect(hwnd, &window_rect);

  RECT glass_rect = {window_rect.left, window_rect.top, window_rect.right,
                     window_rect.top + g_renderer.GetParams().glass_margins.top};
  g_renderer.SetGlassRect(glass_rect);
  g_renderer.RenderGlassFrame();
}
