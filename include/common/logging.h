#pragma once

namespace glass {

// Append a timestamped line to dp8dwmglass.log (tried in order: next to the
// DLL, %TEMP%, the session user's %LOCALAPPDATA%, then C:\Users\Public). Each
// line is also printed to stdout and mirrored to OutputDebugString so it
// appears in a debugger / DebugView. printf-style, %ls works for wide strings.
void Log(const char *fmt, ...);

// Full path of the log file Log() writes to. Empty until the first successful
// open; lets DllMain report where diagnostics land.
const wchar_t *LogFilePath();

} // namespace glass
