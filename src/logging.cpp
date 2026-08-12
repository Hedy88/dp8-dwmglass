// logging.cpp - file logger for crash point diagnosis
#include "logging.h"

#include <stdarg.h>
#include <stdio.h>
#include <windows.h>

namespace glass {

static const wchar_t kLogPath[] = L"C:\\dp8dwmglass.log";

static FILE *OpenLog() {
  FILE *f = _wfopen(kLogPath, L"a");
  if (f)
    return f;

  // C:\ root is not writable from a medium-integrity process; fall back to
  // the user's temp directory so logging always works.
  wchar_t temp[MAX_PATH] = {0};
  if (GetTempPathW(MAX_PATH, temp) == 0)
    return nullptr;

  wchar_t path[MAX_PATH] = {0};
  swprintf(path, ARRAYSIZE(path), L"%lsdp8dwmglass.log", temp);
  return _wfopen(path, L"a");
}

void Log(const char *fmt, ...) {
  FILE *f = OpenLog();
  if (!f)
    return;

  SYSTEMTIME st;
  GetLocalTime(&st);
  fprintf(f, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond,
          st.wMilliseconds);

  va_list args;
  va_start(args, fmt);
  vfprintf(f, fmt, args);
  va_end(args);
  fprintf(f, "\n");

  fclose(f);
}

} // namespace glass
