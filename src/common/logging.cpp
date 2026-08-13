// logging.cpp - file logger for crash point diagnosis
#include "logging.h"

#include <stdarg.h>
#include <stdio.h>
#include <userenv.h>
#include <windows.h>

namespace glass {

static const wchar_t kLogName[] = L"dp8dwmglass.log";

static wchar_t g_log_path[MAX_PATH] = {0};

// directory of the DLL itself. When injected into dwm.exe the DLL usually sits
// in a folder the user can see and write to, which makes the log findable.
static const wchar_t *ModuleDir() {
  static wchar_t buf[MAX_PATH] = {0};
  if (buf[0] == L'\0') {
    HMODULE mod = GetModuleHandleW(L"dp8-dwmglass.dll");
    DWORD n = mod ? GetModuleFileNameW(mod, buf, ARRAYSIZE(buf)) : 0;
    if (n > 0 && n < ARRAYSIZE(buf)) {
      wchar_t *slash = wcsrchr(buf, L'\\');
      if (slash)
        *slash = L'\0';
      else
        buf[0] = L'\0';
    } else {
      buf[0] = L'\0';
    }
  }
  return buf;
}

static FILE *TryOpenIn(const wchar_t *dir) {
  wchar_t path[MAX_PATH] = {0};
  if (wcslen(dir) + wcslen(kLogName) + 1 > ARRAYSIZE(path))
    return nullptr;
  wcscpy_s(path, ARRAYSIZE(path), dir);
  wcscat_s(path, ARRAYSIZE(path), kLogName);
  FILE *f = _wfopen(path, L"a");
  if (f)
    wcscpy_s(g_log_path, ARRAYSIZE(g_log_path), path);
  return f;
}

static FILE *OpenLog() {
  const wchar_t *mod_dir = ModuleDir();
  if (mod_dir[0]) {
    if (FILE *f = TryOpenIn(mod_dir))
      return f;
  }

  wchar_t dir[MAX_PATH] = {0};
  if (GetTempPathW(MAX_PATH, dir) != 0) {
    if (FILE *f = TryOpenIn(dir))
      return f;
  }

  HANDLE token = nullptr;
  if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    DWORD len = 0;
    GetUserProfileDirectoryW(token, nullptr, &len);
    if (len > 0 && len <= MAX_PATH - 48) {
      if (GetUserProfileDirectoryW(token, dir, &len)) {
        wcscat_s(dir, ARRAYSIZE(dir), L"\\AppData\\Local\\");
        if (FILE *f = TryOpenIn(dir)) {
          CloseHandle(token);
          return f;
        }
      }
    }
    CloseHandle(token);
  }

  return TryOpenIn(L"C:\\Users\\Public\\");
}

const wchar_t *LogFilePath() { return g_log_path[0] ? g_log_path : nullptr; }

void Log(const char *fmt, ...) {
  char line[1024] = {0};

  SYSTEMTIME st;
  GetLocalTime(&st);
  int n = _snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d] ", st.wHour,
                    st.wMinute, st.wSecond, st.wMilliseconds);
  if (n < 0 || (size_t)n >= sizeof(line))
    return;

  va_list args;
  va_start(args, fmt);
  _vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, args);
  va_end(args);

  printf("%s\n", line);

  OutputDebugStringA(line);
  OutputDebugStringA("\n");

  FILE *f = OpenLog();
  if (!f)
    return;
  fprintf(f, "%s\n", line);
  fclose(f);
}

} // namespace glass
