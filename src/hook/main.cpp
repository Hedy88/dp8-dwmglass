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
static BOOL g_veh_registered = FALSE;

static LONG WINAPI CrashLogger(EXCEPTION_POINTERS *ep);

__declspec(thread) int g_dp8glass_tls_dummy;

extern "C" {
#pragma section(".CRT$XLB", long, read)

static void NTAPI TlsLoadCallback(PVOID hModule, DWORD reason, PVOID reserved) {
  if (reason != DLL_PROCESS_ATTACH)
    return;
  (void)hModule;
  (void)reserved;

  if (!g_veh_registered) {
    AddVectoredExceptionHandler(1, CrashLogger);
    g_veh_registered = TRUE;
  }

  HMODULE self = GetModuleHandleW(L"dp8-dwmglass.dll");
  wchar_t path[MAX_PATH] = {0};
  if (self && GetModuleFileNameW(self, path, ARRAYSIZE(path))) {
    wchar_t *slash = wcsrchr(path, L'\\');
    if (slash) {
      *(slash + 1) = L'\0';
      wcscat_s(path, ARRAYSIZE(path), L"dp8dwmglass_crash.log");
    }
  } else {
    wcscpy_s(path, ARRAYSIZE(path),
             L"C:\\Users\\Public\\dp8dwmglass_crash.log");
  }

  HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f != INVALID_HANDLE_VALUE) {
    static const char kMsg[] =
        "[TLS callback] loader reached TLS stage before entry point\r\n";
    DWORD written = 0;
    WriteFile(f, kMsg, sizeof(kMsg) - 1, &written, nullptr);
    CloseHandle(f);
  }
}

__declspec(allocate(".CRT$XLB")) PIMAGE_TLS_CALLBACK g_dp8glass_tls_callback =
    TlsLoadCallback;
} // extern "C"

#pragma comment(linker, "/include:__tls_used")

static LONG WINAPI CrashLogger(EXCEPTION_POINTERS *ep) {
  const EXCEPTION_RECORD *er = ep->ExceptionRecord;

  char line[256] = {0};
  HMODULE self = GetModuleHandleW(L"dp8-dwmglass.dll");
  DWORD_PTR offset = self ? (DWORD_PTR)er->ExceptionAddress - (DWORD_PTR)self
                          : (DWORD_PTR)er->ExceptionAddress;
  _snprintf(line, sizeof(line),
            "dp8-dwmglass: exception 0x%08lx at 0x%p (module offset 0x%p) "
            "pid %lu\n",
            er->ExceptionCode, er->ExceptionAddress, (void *)offset,
            GetCurrentProcessId());

  wchar_t path[MAX_PATH] = {0};
  if (self && GetModuleFileNameW(self, path, ARRAYSIZE(path))) {
    wchar_t *slash = wcsrchr(path, L'\\');
    if (slash)
      *(slash + 1) = L'\0';
    wcscat_s(path, ARRAYSIZE(path), L"dp8dwmglass_crash.log");
  } else {
    wcscpy_s(path, ARRAYSIZE(path),
             L"C:\\Users\\Public\\dp8dwmglass_crash.log");
  }

  HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    wcscpy_s(path, ARRAYSIZE(path),
             L"C:\\Users\\Public\\dp8dwmglass_crash.log");
    f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  }
  if (f != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    WriteFile(f, line, (DWORD)strlen(line), &written, nullptr);
    CloseHandle(f);
  }

  OutputDebugStringA(line);
  return EXCEPTION_CONTINUE_SEARCH;
}

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
    if (!g_veh_registered) {
      AddVectoredExceptionHandler(1, CrashLogger);
      g_veh_registered = TRUE;
    }
    Log("DllMain: DLL_PROCESS_ATTACH");
    Log("dp8-dwmglass: log file: %ls", LogFilePath());
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
    RemoveVectoredExceptionHandler(CrashLogger);
    g_veh_registered = FALSE;
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
                     window_rect.top +
                         g_renderer.GetParams().glass_margins.top};
  g_renderer.SetGlassRect(glass_rect);
  g_renderer.RenderGlassFrame();
}
