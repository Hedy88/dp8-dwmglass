// symfetch.cpp - DWM symbol fetcher / cache pre-warm CLI
//
// Usage: symfetch.exe [dwmcore|udwm|all]
//
// Downloads the PDB for the target module from the Microsoft symbol server and
// resolves the DWM hook symbols, populating the symfetch_lib cache. Run this
// once on the target machine (with network) before injecting dp8-dwmglass.dll
// so hook installation inside dwm.exe resolves from the local cache/store and
// never touches the network.

#include "symfetch_lib.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

using namespace glass::sym;

static const wchar_t *g_modules[] = {L"dwmcore.dll", L"udwm.dll", nullptr};

static const wchar_t *g_target_symbols[] = {
    L"CDrawingContext::DrawGlass",
    L"CDrawingContext::DrawVisualTree",
    L"CHwndRenderTarget::RenderDirtyRegion",
    L"CWindowNode::RenderImage",
    L"CArrayBasedCoverageSet::ComputeVisibleRegion",
    L"COcclusionContext::Compute",
    L"CVisual::GetOcclusionInfo",
    L"GetGeometryCurrentValue",
    L"CTopLevelWindow::UpdateOcclusionHints",
    L"CGlassColorizationParameters::AdjustWindowColorization",
    L"CText::ValidateResources",
    nullptr};

static void CountLogger(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vprintf(fmt, args);
  va_end(args);
  printf("\n");
}

int wmain(int argc, wchar_t *argv[]) {
  const wchar_t *target = (argc >= 2) ? argv[1] : L"all";

  printf("DWM symbol fetcher / cache pre-warm\n");
  printf("===================================\n");
  printf("Symbol server: https://msdl.microsoft.com/download/symbols\n");
  printf("Cache: %%LOCALAPPDATA%%\\dp8-dwmglass\\symbols.cache\n\n");

  SetLogger(&CountLogger);

  int total_resolved = 0;
  for (int i = 0; g_modules[i]; i++) {
    if (_wcsicmp(target, L"all") != 0 && _wcsicmp(target, g_modules[i]) != 0) {
      continue;
    }

    int n = CacheSymbols(g_modules[i], g_target_symbols);
    printf("\n%ls: %d/%d symbols resolved\n", g_modules[i], n,
           (int)(sizeof(g_target_symbols) / sizeof(g_target_symbols[0]) - 1));
    total_resolved += n;
  }

  if (total_resolved == 0) {
    printf("\nNothing resolved. Check connectivity / symbol names.\n");
    return 1;
  }
  printf("\nDone. Cache is warm - injection can now resolve offline.\n");
  return 0;
}
