#pragma once

#include "glass_config.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.h>

namespace glass {

class GlassRenderer {
public:
  GlassRenderer();
  ~GlassRenderer();

  bool Initialize();
  void Shutdown();

  void SetParams(const GlassParams &params);
  GlassParams GetParams() const { return m_params; }

  void SetThemeColors(const ThemeColors &colors) { m_theme_colors = colors; }
  ThemeColors GetThemeColors() const { return m_theme_colors; }

  bool RenderGlass(HWND hwnd, const RECT &dest_rect, const RECT &source_rect);
  bool RenderGlassQuad(const RECT &dest_rect,
                       ID3D11ShaderResourceView *blur_srv);

  ID3D11Device *GetDevice() const { return m_device; }
  ID3D11DeviceContext *GetContext() const { return m_context; }

private:
  bool CreateDevice();
  void DestroyDevice();

  bool InitDesktopDuplication();
  bool CreateShaders();
  void DestroyShaders();

  bool CaptureBehindWindow(HWND hwnd, const RECT &rect,
                           ID3D11Texture2D **out_texture);
  bool CaptureWithDesktopDuplication(HWND hwnd, const RECT &rect,
                                     ID3D11Texture2D **out_texture);
  bool CaptureWithGDI(const RECT &rect, ID3D11Texture2D **out_texture);
  bool ApplyBlur(ID3D11ShaderResourceView *input,
                 ID3D11RenderTargetView *output, int width, int height,
                 float blur_radius);

private:
  GlassParams m_params;
  ThemeColors m_theme_colors;

  ID3D11Device *m_device = nullptr;
  ID3D11DeviceContext *m_context = nullptr;

  IDXGIOutputDuplication *m_duplication = nullptr;

  ID3D11VertexShader *m_vertex_shader = nullptr;
  ID3D11PixelShader *m_pixel_shader = nullptr;
  ID3D11InputLayout *m_input_layout = nullptr;
  ID3D11Buffer *m_vertex_buffer = nullptr;
  ID3D11Buffer *m_constant_buffer = nullptr;
  ID3D11SamplerState *m_sampler_state = nullptr;

  int m_width = 0;
  int m_height = 0;
  bool m_initialized = false;
};

} // namespace glass
