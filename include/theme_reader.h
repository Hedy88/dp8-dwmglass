#pragma once

#include "glass_config.h"
#include <string>
#include <windows.h>

namespace theme {

class ThemeReader {
public:
  ThemeReader();
  ~ThemeReader();

  bool LoadCurrentTheme();
  void Unload();

  std::wstring GetCurrentThemePath() const { return m_theme_path; }
  glass::ThemeColors GetThemeColors() const { return m_theme_colors; }
  glass::GlassParams GetGlassParams() const { return m_glass_params; }

  bool IsGlassEnabled() const { return m_glass_enabled; }
  void SetGlassEnabled(bool enabled) { m_glass_enabled = enabled; }

  COLORREF GetColorizationColor() const { return m_colorization_color; }
  bool GetColorizationBalance(DWORD *balance) const;

private:
  bool ReadRegistrySettings();
  bool ReadThemeFile();
  bool ParseThemeColors(const std::wstring &theme_path);

private:
  std::wstring m_theme_path;
  glass::ThemeColors m_theme_colors;
  glass::GlassParams m_glass_params;
  bool m_glass_enabled = true;
  COLORREF m_colorization_color = RGB(0, 102, 204);
};

} // namespace theme
