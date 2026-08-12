#pragma once

#include "glass_config.h"
#include <string>
#include <uxtheme.h>
#include <vector>
#include <windows.h>

namespace msstyles {

struct ImageResource {
  WORD id = 0;
  int width = 0;
  int height = 0;
  int bpp = 0;
  std::vector<BYTE> data;
};

struct ThemeInfo {
  std::wstring display_name;
  std::wstring company_name;
  DWORD version = 0;

  std::vector<ImageResource> images;

  glass::ThemeColors colors;
};

class MsstylesParser {
public:
  MsstylesParser();
  ~MsstylesParser();

  bool Open(const std::wstring &filepath);
  void Close();

  bool ParseResources();
  bool ParseImages();

  ThemeInfo GetThemeInfo() const { return m_theme_info; }
  glass::ThemeColors GetThemeColors() const { return m_theme_colors; }
  glass::GlassParams GetGlassParams() const { return m_glass_params; }

  const ImageResource *GetImage(WORD id) const;
  std::vector<WORD> GetImageIds() const;

private:
  bool ParsePE();
  bool LoadFromUxTheme();
  bool BitmapToImageResource(HBITMAP hBitmap, ImageResource &out_image);
  COLORREF GetThemeColorSafe(HTHEME hTheme, int part, int state, int prop,
                             COLORREF fallback);

private:
  HANDLE m_file = INVALID_HANDLE_VALUE;
  HANDLE m_map = nullptr;
  BYTE *m_view = nullptr;
  DWORD m_file_size = 0;

  IMAGE_DOS_HEADER *m_dos = nullptr;
  IMAGE_NT_HEADERS64 *m_nt = nullptr;

  std::wstring m_theme_path;
  HTHEME m_theme = nullptr;

  ThemeInfo m_theme_info;
  glass::ThemeColors m_theme_colors;
  glass::GlassParams m_glass_params;

  std::vector<ImageResource> m_images;
};

} // namespace msstyles
