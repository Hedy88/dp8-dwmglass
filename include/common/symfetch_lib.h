#pragma once

#include <cstdint>
#include <windows.h>

namespace glass::sym {

// Progress/error reporting hook. dwm sets it to glass::Log (via
// SetLogger(&Log)); symfetch.exe installs a printf-based handler. The default
// writes to stdout, which is harmless in a GUI process.
typedef void(LogFn)(const char *fmt, ...);
void SetLogger(LogFn *fn);

// Resolve the RVA of `symbol` (undecorated, e.g. L"CDrawingContext::DrawVisualTree")
// inside `module` (e.g. L"dwmcore.dll").
//
// - If the module is already loaded in this process the in-memory image is
//   used and the returned RVA is relative to GetModuleHandleW(module).
// - Otherwise the module is located in the system directories.
// - On a cache miss the PDB is fetched from the Microsoft symbol server into a
//   local store under %LOCALAPPDATA%\dp8-dwmglass\symbols and the name is
//   resolved via DbgHelp.
// - Results are cached in %LOCALAPPDATA%\dp8-dwmglass\symbols.cache keyed by
//   SHA-1 of the module file, so a patched dwmcore.dll changes the hash and
//   forces a re-resolve instead of hooking a stale RVA.
//
// Returns true and writes *out_rva on success. Failures are logged.
bool ResolveSymbolRva(const wchar_t *module, const wchar_t *symbol,
                      DWORD64 *out_rva);

// Resolve a list of symbols for `module` and populate the cache. Used by
// symfetch.exe to pre-warm the cache on a machine with network access so hook
// installation inside dwm.exe never touches the network. Returns the number of
// symbols successfully resolved.
int CacheSymbols(const wchar_t *module, const wchar_t *const *symbols);

} // namespace glass::sym
