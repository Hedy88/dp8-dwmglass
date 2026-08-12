#pragma once

#include <cstdint>
#include <d3d11.h>
#include <string>
#include <vector>
#include <windows.h>

namespace glass {

struct GlassParams {
  float blur_amount = 15.0f;
  float opacity = 0.7f;
  float corner_radius = 8.0f;
  bool enabled = true;

  struct Color {
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 0.5f;
  } tint_color;

  struct Margins {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
  } glass_margins;

  Margins caption_margins;
};

struct ThemeColors {
  COLORREF active_caption = RGB(0, 102, 204);
  COLORREF inactive_caption = RGB(128, 128, 128);
  COLORREF active_caption_text = RGB(255, 255, 255);
  COLORREF inactive_caption_text = RGB(128, 128, 128);
  COLORREF accent = RGB(0, 102, 204);
};

struct WindowInfo {
  HWND hwnd = nullptr;
  RECT window_rect = {0};
  RECT client_rect = {0};
  bool is_active = false;
  bool has_glass = true;
  GlassParams::Margins glass_margins;
};

} // namespace glass
