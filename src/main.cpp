#include "dwm_hooks.h"
#include "glass_renderer.h"
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

  printf("dp8-dwmglass: Initializing...\n");

  if (!g_theme_reader.LoadCurrentTheme()) {
    printf("dp8-dwmglass: WARNING - could not load theme, using defaults\n");
  }

  // parse msstyles for DP Aero theme colors
  std::wstring theme_path = g_theme_reader.GetCurrentThemePath();
  if (g_msstyles_parser.Open(theme_path)) {
    g_msstyles_parser.ParseResources();
    printf("dp8-dwmglass: Parsed msstyles theme: %ls\n", theme_path.c_str());
  }

  // initialize glass renderer
  if (!g_renderer.Initialize()) {
    printf("dp8-dwmglass: Failed to initialize glass renderer\n");
    return false;
  }

  // apply theme colors
  ThemeColors colors = g_theme_reader.GetThemeColors();
  g_renderer.SetThemeColors(colors);

  // apply glass params
  GlassParams params = g_theme_reader.GetGlassParams();
  g_renderer.SetParams(params);

  // install DWM hooks
  if (!DWMHookManager::Get().Initialize()) {
    printf("dp8-dwmglass: Failed to initialize hook manager\n");
    g_renderer.Shutdown();
    return false;
  }

  if (!DWMHookManager::Get().InstallHooks()) {
    printf("dp8-dwmglass: Failed to install hooks\n");
    DWMHookManager::Get().Shutdown();
    g_renderer.Shutdown();
    return false;
  }

  g_initialized = true;
  printf("dp8-dwmglass: Initialization complete\n");
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
    DisableThreadLibraryCalls(hModule);
    if (!InitializeGlass()) {
      printf("dp8-dwmglass: Initialization failed\n");
      return FALSE;
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

  RECT client_rect = {0};
  GetClientRect(hwnd, &client_rect);

  GlassParams params = g_renderer.GetParams();
  params.glass_margins.left = 8;
  params.glass_margins.top = 31;
  params.glass_margins.right = 8;
  params.glass_margins.bottom = 8;
  g_renderer.SetParams(params);

  RECT glass_rect = {window_rect.left, window_rect.top, window_rect.right,
                     window_rect.top + params.glass_margins.top};

  g_renderer.RenderGlass(hwnd, glass_rect, client_rect);
}
