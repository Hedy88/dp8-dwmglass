// theme_reader.cpp - reads theme settings from registry and msstyles
#include "theme_reader.h"
#include "msstyles_parser.h"
#include <dwmapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#pragma comment(lib, "dwmapi.lib")

using namespace theme;

ThemeReader::ThemeReader() {}

ThemeReader::~ThemeReader() { Unload(); }

bool ThemeReader::LoadCurrentTheme() {
  ReadRegistrySettings();

  // find and parse the current msstyles theme
  wchar_t theme_path[MAX_PATH] = {0};
  DWORD size = sizeof(theme_path);

  HKEY hKey = nullptr;
  if (RegOpenKeyExW(
          HKEY_CURRENT_USER,
          L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
          0, KEY_READ, &hKey) == ERROR_SUCCESS) {

    DWORD type = 0;
    if (RegQueryValueExW(hKey, L"AppliesTo", nullptr, &type, (LPBYTE)theme_path,
                         &size) != ERROR_SUCCESS) {
      wcscpy_s(theme_path,
               L"C:\\Windows\\Resources\\Themes\\Aero\\aero.msstyles");
    }

    RegCloseKey(hKey);
  } else {
    wcscpy_s(theme_path,
             L"C:\\Windows\\Resources\\Themes\\Aero\\aero.msstyles");
  }

  m_theme_path = theme_path;

  // try to parse the theme file
  msstyles::MsstylesParser parser;
  if (parser.Open(m_theme_path)) {
    parser.ParseResources();
    printf("ThemeReader: Parsed msstyles theme: %ls\n", m_theme_path.c_str());
  }

  // read glass opacity from registry
  HKEY hGlassKey = nullptr;
  if (RegOpenKeyExW(
          HKEY_CURRENT_USER,
          L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
          0, KEY_READ, &hGlassKey) == ERROR_SUCCESS) {

    DWORD glass_enabled = 1;
    DWORD type = 0;
    DWORD size_val = sizeof(DWORD);
    RegQueryValueExW(hGlassKey, L"EnableTransparency", nullptr, &type,
                     (LPBYTE)&glass_enabled, &size_val);

    m_glass_enabled = (glass_enabled != 0);
    RegCloseKey(hGlassKey);
  }

  printf("ThemeReader: Loaded theme %ls\n", m_theme_path.c_str());
  printf("ThemeReader: Glass enabled: %s\n", m_glass_enabled ? "yes" : "no");
  return true;
}

void ThemeReader::Unload() { m_theme_path.clear(); }

bool ThemeReader::ReadRegistrySettings() {
  HKEY hKey = nullptr;
  if (RegOpenKeyExW(
          HKEY_CURRENT_USER,
          L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
          0, KEY_READ, &hKey) == ERROR_SUCCESS) {

    DWORD type = 0;
    DWORD value = 0;
    DWORD size = sizeof(DWORD);

    if (RegQueryValueExW(hKey, L"ColorPrevalence", nullptr, &type,
                         (LPBYTE)&value, &size) == ERROR_SUCCESS) {
    }

    RegCloseKey(hKey);
  }

  return true;
}

bool ThemeReader::ReadThemeFile() {
  if (m_theme_path.empty())
    return false;

  msstyles::MsstylesParser parser;
  if (!parser.Open(m_theme_path)) {
    return false;
  }

  parser.ParseResources();

  m_theme_colors = parser.GetThemeColors();
  m_glass_params = parser.GetGlassParams();

  return true;
}

bool ThemeReader::GetColorizationBalance(DWORD *balance) const {
  if (!balance)
    return false;
  *balance = 50; // Default balance
  return true;
}
