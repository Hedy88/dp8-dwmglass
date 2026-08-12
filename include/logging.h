#pragma once

namespace glass {

// Append a timestamped line to C:\dp8dwmglass.log (falls back to %TEMP% if the
// root is not writable). printf-style, %ls works for wide strings.
void Log(const char *fmt, ...);

} // namespace glass
