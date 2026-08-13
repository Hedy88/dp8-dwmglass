// symfetch_lib.cpp - shared DWM symbol resolution library
//
// Resolves undecorated symbol names to module-relative RVAs using DbgHelp plus
// the Microsoft symbol server, with a persistent SHA-1-keyed cache. Both
// symfetch.exe (CLI, offline pre-warm) and dp8-dwmglass.dll (hook install
// time, inside dwm.exe) link against this.

#include "symfetch_lib.h"

#include <shlobj.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <wincrypt.h>
#include <dbghelp.h>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")

#define SYM_SERVER L"https://msdl.microsoft.com/download/symbols"
#define SYM_STORE_DIR L"dp8-dwmglass\\symbols"
#define SYM_CACHE_FILE L"dp8-dwmglass\\symbols.cache"

namespace glass::sym {

static void DefaultLogger(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vprintf(fmt, args);
  va_end(args);
  printf("\n");
}

static LogFn *g_log = &DefaultLogger;

void SetLogger(LogFn *fn) { g_log = fn ? fn : &DefaultLogger; }

static void Log(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  g_log(fmt, args);
  va_end(args);
}

// Local app-data directory via the process token, so this works inside dwm.exe
// (launched by winlogon, may not inherit USERPROFILE/TEMP).
static bool LocalAppDataDir(wchar_t *out, DWORD out_len) {
  if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr,
                                 SHGFP_TYPE_CURRENT, out)) &&
      GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES) {
    return true;
  }
  // Fallback writable by every authenticated user.
  wcscpy_s(out, out_len, L"C:\\Users\\Public");
  return true;
}

// SHA-1 of a file (cache key / build identity guard).
static bool HashFileSha1(const wchar_t *path, BYTE digest[20]) {
  HANDLE hFile =
      CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                  FILE_ATTRIBUTE_NORMAL, nullptr);
  if (hFile == INVALID_HANDLE_VALUE)
    return false;

  HCRYPTPROV prov = 0;
  HCRYPTHASH hash = 0;
  bool ok = false;

  if (CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_FULL,
                           CRYPT_VERIFYCONTEXT)) {
    if (CryptCreateHash(prov, CALG_SHA1, 0, 0, &hash)) {
      BYTE buf[64 * 1024];
      DWORD read = 0;
      ok = true;
      while (ReadFile(hFile, buf, sizeof(buf), &read, nullptr) && read > 0) {
        if (!CryptHashData(hash, buf, read, 0)) {
          ok = false;
          break;
        }
      }
      if (ok) {
        DWORD size = 20;
        ok = CryptGetHashParam(hash, HP_HASHVAL, digest, &size, 0) != FALSE;
      }
      CryptDestroyHash(hash);
    }
    CryptReleaseContext(prov, 0);
  }
  CloseHandle(hFile);
  return ok;
}

// Symbol cache: one line per entry, tab separated:
//   <sha1_hex>  <module>  <symbol>  <rva_hex>
static void Sha1ToHex(const BYTE sha1[20], wchar_t *out) {
  static const wchar_t kHex[] = L"0123456789abcdef";
  for (int i = 0; i < 20; i++) {
    out[i * 2] = kHex[sha1[i] >> 4];
    out[i * 2 + 1] = kHex[sha1[i] & 0xF];
  }
  out[40] = L'\0';
}

static bool FindInCache(const wchar_t *cache_path, const BYTE sha1[20],
                        const wchar_t *module, const wchar_t *symbol,
                        DWORD64 *rva) {
  wchar_t want_sha1[41];
  Sha1ToHex(sha1, want_sha1);

  FILE *f = _wfopen(cache_path, L"r");
  if (!f)
    return false;

  wchar_t line[512];
  bool found = false;
  while (fgetws(line, ARRAYSIZE(line), f)) {
    wchar_t entry_sha1[41];
    wchar_t entry_module[64];
    wchar_t entry_symbol[256];
    unsigned long long entry_rva = 0;
    if (swscanf(line, L"%40[^\t]\t%63[^\t]\t%255[^\t]\t%llx", entry_sha1,
                entry_module, entry_symbol, &entry_rva) == 4 &&
        wcscmp(entry_sha1, want_sha1) == 0 &&
        _wcsicmp(entry_module, module) == 0 &&
        wcscmp(entry_symbol, symbol) == 0) {
      *rva = (DWORD64)entry_rva;
      found = true;
      break;
    }
  }
  fclose(f);
  return found;
}

static void AddToCache(const wchar_t *cache_path, const BYTE sha1[20],
                       const wchar_t *module, const wchar_t *symbol,
                       DWORD64 rva) {
  wchar_t sha1_hex[41];
  Sha1ToHex(sha1, sha1_hex);
  FILE *f = _wfopen(cache_path, L"a");
  if (!f)
    return;
  fwprintf(f, L"%ls\t%ls\t%ls\t0x%llx\n", sha1_hex, module, symbol,
           (unsigned long long)rva);
  fclose(f);
}

// Path of the module file: in-process image if loaded, otherwise the system
// directory.
static bool ModuleFilePath(const wchar_t *module, wchar_t *out,
                           DWORD out_len) {
  HMODULE hMod = GetModuleHandleW(module);
  if (hMod && GetModuleFileNameW(hMod, out, out_len) > 0 &&
      GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES) {
    return true;
  }

  wchar_t sys_dir[MAX_PATH] = {0};
  GetSystemDirectoryW(sys_dir, MAX_PATH);
  if (swprintf_s(out, out_len, L"%ls\\%ls", sys_dir, module) >= 0 &&
      GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES) {
    return true;
  }

  Log("symfetch_lib: could not locate %ls", module);
  return false;
}

// DbgHelp resolution. `base` is the in-process load address of the module (0
// if not loaded); dbghelp is told to load at that base so reported addresses
// map directly, and rva = address - load base either way.
struct ResolveCtx {
  const wchar_t *symbol;
  DWORD64 base;
  DWORD64 rva;
  bool found;
};

static BOOL CALLBACK EnumSymbol(PSYMBOL_INFOW sym, ULONG, PVOID context) {
  ResolveCtx *ctx = (ResolveCtx *)context;
  if (wcscmp(sym->Name, ctx->symbol) == 0) {
    ctx->found = true;
    ctx->rva = (DWORD64)sym->Address - ctx->base;
    return FALSE;
  }
  return TRUE;
}

static bool ResolveViaDbgHelp(const wchar_t *module_path, DWORD64 base,
                              const wchar_t *symbol, DWORD64 *out_rva) {
  wchar_t appdata[MAX_PATH] = {0};
  if (!LocalAppDataDir(appdata, MAX_PATH))
    return false;

  wchar_t store_dir[MAX_PATH] = {0};
  swprintf_s(store_dir, ARRAYSIZE(store_dir), L"%ls\\%ls", appdata,
             SYM_STORE_DIR);
  CreateDirectoryW(store_dir, nullptr);

  wchar_t sym_store[MAX_PATH * 2] = {0};
  swprintf_s(sym_store, ARRAYSIZE(sym_store), L"SRV*%ls*%ls", store_dir,
             SYM_SERVER);

  SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME |
                SYMOPT_ALLOW_ABSOLUTE_SYMBOLS);

  if (!SymInitializeW(GetCurrentProcess(), sym_store, FALSE)) {
    Log("symfetch_lib: SymInitializeW failed: %lu", GetLastError());
    return false;
  }

  DWORD64 loaded_base =
      SymLoadModuleExW(GetCurrentProcess(), nullptr, module_path, nullptr,
                       base, 0, nullptr, 0);
  if (loaded_base == 0) {
    Log("symfetch_lib: SymLoadModuleExW(%ls) failed: %lu", module_path,
        GetLastError());
    SymCleanup(GetCurrentProcess());
    return false;
  }
  if (base == 0)
    base = loaded_base;

  ResolveCtx ctx = {symbol, base, 0, false};
  SymEnumSymbolsW(GetCurrentProcess(), loaded_base, nullptr, EnumSymbol,
                  &ctx);
  SymCleanup(GetCurrentProcess());

  if (ctx.found) {
    Log("symfetch_lib: resolved %ls @ RVA 0x%llx", symbol, ctx.rva);
    *out_rva = ctx.rva;
    return true;
  }

  Log("symfetch_lib: symbol %ls not found in %ls (PDB missing on this "
      "machine? run symfetch.exe %ls first)",
      symbol, module_path, module_path);
  return false;
}

// Public API
bool ResolveSymbolRva(const wchar_t *module, const wchar_t *symbol,
                      DWORD64 *out_rva) {
  if (!module || !symbol || !out_rva)
    return false;

  wchar_t module_path[MAX_PATH] = {0};
  if (!ModuleFilePath(module, module_path, MAX_PATH))
    return false;

  BYTE sha1[20] = {0};
  if (!HashFileSha1(module_path, sha1)) {
    Log("symfetch_lib: failed to hash %ls", module_path);
    return false;
  }

  wchar_t cache_path[MAX_PATH] = {0};
  if (!LocalAppDataDir(cache_path, MAX_PATH))
    return false;
  wcscat_s(cache_path, ARRAYSIZE(cache_path), L"\\");
  wcscat_s(cache_path, ARRAYSIZE(cache_path), SYM_CACHE_FILE);

  if (FindInCache(cache_path, sha1, module, symbol, out_rva)) {
    Log("symfetch_lib: cache hit %ls!%ls @ RVA 0x%llx", module, symbol,
        *out_rva);
    return true;
  }

  wchar_t sha1_hex[41];
  Sha1ToHex(sha1, sha1_hex);
  Log("symfetch_lib: resolving %ls!%ls (module SHA1 %ls)", module, symbol,
      sha1_hex);
  DWORD64 base = (DWORD64)GetModuleHandleW(module);
  DWORD64 rva = 0;
  if (!ResolveViaDbgHelp(module_path, base, symbol, &rva))
    return false;

  AddToCache(cache_path, sha1, module, symbol, rva);
  *out_rva = rva;
  return true;
}

int CacheSymbols(const wchar_t *module, const wchar_t *const *symbols) {
  if (!module || !symbols)
    return 0;

  int resolved = 0;
  for (int i = 0; symbols[i]; i++) {
    DWORD64 rva = 0;
    if (ResolveSymbolRva(module, symbols[i], &rva))
      resolved++;
  }
  return resolved;
}

} // namespace glass::sym
