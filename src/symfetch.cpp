// symfetch.cpp - download PDBs from Microsoft Symbol Server and dump DWM
// symbols usage: symfetch.exe [dwmcore|udwm|all] requires: Windows SDK
// Debugging Tools (symchk.exe) or internet access for dbghelp

#include <windows.h>
#include <dbghelp.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "shlwapi.lib")

#define SYM_STORE L"SRV*%ls*https://msdl.microsoft.com/download/symbols"

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

bool DownloadPDB(const wchar_t *dll_path, wchar_t *pdb_path,
                 DWORD pdb_path_len) {
  wchar_t store_dir[MAX_PATH] = {0};
  wchar_t symbol_store[MAX_PATH * 2] = {0};
  wchar_t temp_dir[MAX_PATH] = {0};

  GetTempPathW(ARRAYSIZE(temp_dir), temp_dir);
  swprintf_s(store_dir, ARRAYSIZE(store_dir), L"%lsdp8-dwmglass-symbols",
             temp_dir);
  CreateDirectoryW(store_dir, nullptr);

  swprintf_s(symbol_store, ARRAYSIZE(symbol_store), SYM_STORE, store_dir);
  printf("Symbol store: %ls\n", symbol_store);
  printf("Downloading symbols for %ls...\n", dll_path);

  if (!SymInitializeW(GetCurrentProcess(), symbol_store, FALSE)) {
    printf("SymInitialize failed: %lu\n", GetLastError());
    return false;
  }

  DWORD64 base = SymLoadModuleExW(GetCurrentProcess(), NULL, dll_path, NULL, 0,
                                  0, NULL, 0);

  if (base == 0) {
    printf("SymLoadModuleEx failed: %lu\n", GetLastError());
    SymCleanup(GetCurrentProcess());
    return false;
  }

  printf("Module loaded at 0x%llx\n", base);

  // Get PDB path
  IMAGEHLP_MODULEW64 mod_info = {0};
  mod_info.SizeOfStruct = sizeof(mod_info);

  if (SymGetModuleInfoW64(GetCurrentProcess(), base, &mod_info)) {
    printf("PDB: %ls\n", mod_info.LoadedPdbName);
    wcscpy_s(pdb_path, pdb_path_len, mod_info.LoadedPdbName);
  }

  // Enumerate all symbols
  printf("\nEnumerating symbols...\n");

  PSYM_ENUMERATESYMBOLS_CALLBACKW callback =
      [](PSYMBOL_INFOW pSymInfo, ULONG SymbolSize, PVOID Context) -> BOOL {
    const wchar_t *name = pSymInfo->Name;
    if (!name)
      return TRUE;

    ULONG *pFound = (ULONG *)Context;
    for (int i = 0; g_target_symbols[i]; i++) {
      if (wcscmp(name, g_target_symbols[i]) == 0) {
        printf("  FOUND: %ls @ 0x%llx (size=%lu)\n", name, pSymInfo->Address,
               SymbolSize);
        (*pFound)++;
        break;
      }
    }

    return TRUE;
  };

  ULONG found = 0;
  SymEnumSymbolsW(GetCurrentProcess(), base, nullptr, callback, &found);
  printf("  %lu target symbol(s) found\n", found);

  SymCleanup(GetCurrentProcess());
  return true;
}

bool FindSystemDll(const wchar_t *dll_name, wchar_t *out_path,
                   DWORD out_path_len) {
  wchar_t system_dir[MAX_PATH] = {0};
  GetSystemDirectoryW(system_dir, MAX_PATH);
  swprintf_s(out_path, out_path_len, L"%ls\\%ls", system_dir, dll_name);

  if (GetFileAttributesW(out_path) != INVALID_FILE_ATTRIBUTES) {
    return true;
  }

  // Try SysWOW64 for 32-bit
  if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_SYSTEMX86, NULL, 0, system_dir))) {
    swprintf_s(out_path, out_path_len, L"%ls\\%ls", system_dir, dll_name);
    if (GetFileAttributesW(out_path) != INVALID_FILE_ATTRIBUTES) {
      return true;
    }
  }

  return false;
}

int wmain(int argc, wchar_t *argv[]) {
  const wchar_t *target = L"all";
  if (argc >= 2) {
    target = argv[1];
  }

  printf("DWM symbol fetcher\n");
  printf("==================\n");
  printf("Symbol server: https://msdl.microsoft.com/download/symbols\n\n");

  // DownloadPDB initializes and cleans up DbgHelp for each module.
  // Set options before that initialization so they apply to every load.
  SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME |
                SYMOPT_ALLOW_ABSOLUTE_SYMBOLS);

  for (int i = 0; g_modules[i]; i++) {
    if (_wcsicmp(target, L"all") != 0 && _wcsicmp(target, g_modules[i]) != 0) {
      continue;
    }

    wchar_t dll_path[MAX_PATH] = {0};
    if (!FindSystemDll(g_modules[i], dll_path, MAX_PATH)) {
      printf("Could not find %ls in system directories\n", g_modules[i]);
      continue;
    }

    printf("\n--- %ls ---\n", g_modules[i]);
    printf("Path: %ls\n", dll_path);

    wchar_t pdb_path[MAX_PATH] = {0};
    if (DownloadPDB(dll_path, pdb_path, MAX_PATH)) {
      printf("PDB downloaded to: %ls\n", pdb_path);
    } else {
      printf("Failed to download PDB\n");
    }
  }

  printf("\nDone.\n");
  return 0;
}
