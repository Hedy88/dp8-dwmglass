// msstyles_parser.cpp - theme data extraction via the UxTheme API
#include "msstyles_parser.h"

#include <dwmapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uxtheme.h>
#include <vsstyle.h>
#include <vssym32.h>
#include <windows.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

using namespace msstyles;

MsstylesParser::MsstylesParser() {}

MsstylesParser::~MsstylesParser() { Close(); }

bool MsstylesParser::Open(const std::wstring &filepath) {
  Close();

  m_file = CreateFileW(filepath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (m_file == INVALID_HANDLE_VALUE) {
    printf("MsstylesParser: Failed to open %ls\n", filepath.c_str());
    return false;
  }

  m_file_size = GetFileSize(m_file, nullptr);
  if (m_file_size == INVALID_FILE_SIZE) {
    printf("MsstylesParser: Failed to get file size\n");
    CloseHandle(m_file);
    m_file = INVALID_HANDLE_VALUE;
    return false;
  }

  m_map = CreateFileMappingW(m_file, nullptr, PAGE_READONLY, 0, 0, nullptr);
  if (!m_map) {
    printf("MsstylesParser: Failed to create file mapping\n");
    CloseHandle(m_file);
    m_file = INVALID_HANDLE_VALUE;
    return false;
  }

  m_view = (BYTE *)MapViewOfFile(m_map, FILE_MAP_READ, 0, 0, 0);
  if (!m_view) {
    printf("MsstylesParser: Failed to map file\n");
    CloseHandle(m_map);
    m_map = nullptr;
    CloseHandle(m_file);
    m_file = INVALID_HANDLE_VALUE;
    return false;
  }

  if (!ParsePE()) {
    printf("MsstylesParser: Invalid PE file\n");
    Close();
    return false;
  }

  m_theme_path = filepath;
  printf("MsstylesParser: Opened %ls (%d bytes)\n", filepath.c_str(),
         m_file_size);
  return true;
}

void MsstylesParser::Close() {
  if (m_theme) {
    CloseThemeData(m_theme);
    m_theme = nullptr;
  }
  if (m_view) {
    UnmapViewOfFile(m_view);
    m_view = nullptr;
  }
  if (m_map) {
    CloseHandle(m_map);
    m_map = nullptr;
  }
  if (m_file != INVALID_HANDLE_VALUE) {
    CloseHandle(m_file);
    m_file = INVALID_HANDLE_VALUE;
  }
  m_images.clear();
}

bool MsstylesParser::ParsePE() {
  if (m_file_size < sizeof(IMAGE_DOS_HEADER))
    return false;

  m_dos = (IMAGE_DOS_HEADER *)m_view;
  if (m_dos->e_magic != IMAGE_DOS_SIGNATURE)
    return false;
  if (m_dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) > (DWORD)m_file_size)
    return false;

  m_nt = (IMAGE_NT_HEADERS64 *)(m_view + m_dos->e_lfanew);
  if (m_nt->Signature != IMAGE_NT_SIGNATURE)
    return false;

  printf("MsstylesParser: PE valid (machine=0x%x, sections=%d)\n",
         m_nt->FileHeader.Machine, m_nt->FileHeader.NumberOfSections);

  return true;
}

COLORREF MsstylesParser::GetThemeColorSafe(HTHEME hTheme, int part, int state,
                                           int prop, COLORREF fallback) {
  COLORREF color = 0;
  if (hTheme && SUCCEEDED(GetThemeColor(hTheme, part, state, prop, &color)))
    return color;
  return fallback;
}

bool MsstylesParser::LoadFromUxTheme() {
  if (m_theme)
    CloseThemeData(m_theme);
  m_theme = OpenThemeData(GetDesktopWindow(), L"WINDOW");
  if (!m_theme) {
    printf("MsstylesParser: OpenThemeData failed (error %d)\n", GetLastError());
    return false;
  }

  // Theme documentation properties (name/company/version) straight from the
  // applied theme file - the UxTheme equivalent of reading VERSIONINFO.
  if (!m_theme_path.empty()) {
    wchar_t buffer[256] = {0};
    GetThemeDocumentationProperty(m_theme_path.c_str(), L"DisplayName",
                                  buffer, ARRAYSIZE(buffer));
    if (buffer[0])
      m_theme_info.display_name = buffer;

    GetThemeDocumentationProperty(m_theme_path.c_str(), L"Company", buffer,
                                  ARRAYSIZE(buffer));
    if (buffer[0])
      m_theme_info.company_name = buffer;

    GetThemeDocumentationProperty(m_theme_path.c_str(), L"Version", buffer,
                                  ARRAYSIZE(buffer));
    if (buffer[0])
      m_theme_info.version = (DWORD)wcstoul(buffer, nullptr, 0);
  }

  // Accent / glass tint from DWM colorization (the Win8 DP accent color)
  COLORREF colorization = RGB(0, 102, 204); // default Aero blue
  DWORD colorization_color = 0;
  BOOL opaque = FALSE;
  if (SUCCEEDED(DwmGetColorizationColor(&colorization_color, &opaque))) {
    colorization = RGB(GetRValue(colorization_color),
                       GetGValue(colorization_color),
                       GetBValue(colorization_color));

    // High byte of colorization encodes opacity (0..255)
    float opacity = (float)((colorization_color >> 24) & 0xFF) / 255.0f;
    if (opacity >= 0.01f && opacity <= 1.0f)
      m_glass_params.opacity = opacity;
  }

  // Caption bar colors from the applied theme
  m_theme_colors.accent = colorization;
  m_theme_colors.active_caption =
      GetThemeColorSafe(m_theme, WP_CAPTION, CS_ACTIVE, TMT_FILLCOLOR,
                        colorization);
  m_theme_colors.active_caption_text =
      GetThemeColorSafe(m_theme, WP_CAPTION, CS_ACTIVE, TMT_TEXTCOLOR,
                        RGB(255, 255, 255));
  m_theme_colors.inactive_caption =
      GetThemeColorSafe(m_theme, WP_CAPTION, CS_INACTIVE, TMT_FILLCOLOR,
                        RGB(96, 96, 96));
  m_theme_colors.inactive_caption_text =
      GetThemeColorSafe(m_theme, WP_CAPTION, CS_INACTIVE, TMT_TEXTCOLOR,
                        RGB(160, 160, 160));

  m_theme_info.colors = m_theme_colors;

  ParseImages();

  printf("MsstylesParser: Loaded theme '%ls' via UxTheme (images=%d)\n",
         m_theme_info.display_name.empty() ? L"?" : m_theme_info.display_name.c_str(),
         (int)m_images.size());
  return true;
}

bool MsstylesParser::ParseResources() {
  if (!m_nt)
    return false;
  return LoadFromUxTheme();
}

bool MsstylesParser::BitmapToImageResource(HBITMAP hBitmap,
                                           ImageResource &out_image) {
  BITMAP bm = {};
  if (!GetObjectW(hBitmap, sizeof(bm), &bm) || bm.bmWidth <= 0 ||
      bm.bmHeight <= 0)
    return false;

  const size_t row_size = (size_t)bm.bmWidth * 4;
  out_image.width = bm.bmWidth;
  out_image.height = bm.bmHeight;
  out_image.bpp = 32;
  out_image.data.resize(row_size * bm.bmHeight);

  HDC hdc = CreateCompatibleDC(nullptr);
  if (!hdc)
    return false;

  BITMAPINFO bi = {};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = bm.bmWidth;
  bi.bmiHeader.biHeight = -bm.bmHeight; // top-down
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;

  const int lines = GetDIBits(hdc, hBitmap, 0, bm.bmHeight,
                              out_image.data.data(), &bi, DIB_RGB_COLORS);
  DeleteDC(hdc);

  return lines == bm.bmHeight;
}

bool MsstylesParser::ParseImages() {
  if (!m_theme)
    return false;

  // aero glass window frame is 9-sliced. Explicit MS SDK part ids (Vista+
  // layout) so this is identical across mingw/MSVC headers. Glass variants
  // first, plain frame parts as fallback.
  static const int kGlassParts[] = {11, 12, 13, 14}; // FRAME*GLASS
  static const int kFrameParts[] = {7, 8, 9, 10};    // FRAMELEFT/RIGHT/TOP/BOTTOM

  for (int part : kGlassParts) {
    HBITMAP hBitmap = nullptr;
    if (FAILED(GetThemeBitmap(m_theme, part, FS_ACTIVE, TMT_IMAGEFILE,
                              GBF_COPY, &hBitmap)))
      continue;

    ImageResource image;
    image.id = (WORD)part;
    if (BitmapToImageResource(hBitmap, image))
      m_images.push_back(image);
    else
      printf("MsstylesParser: Failed to decode bitmap for part %d\n", part);

    DeleteObject(hBitmap);
  }

  for (int part : kFrameParts) {
    if (GetImage((WORD)part))
      continue; // glass variant already provided this side
    HBITMAP hBitmap = nullptr;
    if (FAILED(GetThemeBitmap(m_theme, part, FS_ACTIVE, TMT_IMAGEFILE,
                              GBF_COPY, &hBitmap)))
      continue;

    ImageResource image;
    image.id = (WORD)part;
    if (BitmapToImageResource(hBitmap, image))
      m_images.push_back(image);
    DeleteObject(hBitmap);
  }

  return !m_images.empty();
}

const ImageResource *MsstylesParser::GetImage(WORD id) const {
  for (const auto &img : m_images) {
    if (img.id == id)
      return &img;
  }
  return nullptr;
}

std::vector<WORD> MsstylesParser::GetImageIds() const {
  std::vector<WORD> ids;
  for (const auto &img : m_images) {
    ids.push_back(img.id);
  }
  return ids;
}
