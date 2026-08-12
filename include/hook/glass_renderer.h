#pragma once

#include "glass_config.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.h>

namespace glass {

class GlassRenderer {
public:
  // Raw BGRA32 top-down image for a theme frame piece (from aero.msstyles).
  struct FrameImageData {
    const void *pixels = nullptr;
    int width = 0;
    int height = 0;
  };

  // Order matches the 9-slice frame sides: 11=left, 12=right, 13=top,
  // 14=bottom in the msstyles WINDOW class.
  enum FramePart {
    kFrameLeft = 0,
    kFrameRight,
    kFrameTop,
    kFrameBottom,
    kFramePartCount,
  };

  GlassRenderer();
  ~GlassRenderer();

  bool Initialize();
  void Shutdown();

  void SetParams(const GlassParams &params);
  GlassParams GetParams() const { return m_params; }

  void SetThemeColors(const ThemeColors &colors) { m_theme_colors = colors; }
  ThemeColors GetThemeColors() const { return m_theme_colors; }

  // Cached geometry for the glass region. Must be set in desktop coordinates
  // before RenderGlassFrame() is called from a hook (no user32 access there).
  void SetGlassRect(const RECT &rect);
  const RECT &GetGlassRect() const { return m_glass_rect; }

  // Provide the theme frame pieces (parts 11-14). A copy is made and cached
  // as device textures; call again to update.
  void SetFrameImages(const FrameImageData *images, int count);

  bool RenderGlass(HWND hwnd, const RECT &dest_rect, const RECT &source_rect);
  bool RenderGlassFrame();

  ID3D11Device *GetDevice() const { return m_device; }
  ID3D11DeviceContext *GetContext() const { return m_context; }

private:
  bool CreateDevice();
  void DestroyDevice();

  bool InitDesktopDuplication();
  bool CreateShaders();
  void DestroyShaders();
  void ReleaseCachedResources();

  bool CaptureBehindWindow(HWND hwnd, const RECT &rect,
                           ID3D11Texture2D **out_texture,
                           ID3D11ShaderResourceView **out_srv);
  bool CaptureWithDesktopDuplication(HWND hwnd, const RECT &rect,
                                     ID3D11Texture2D **out_texture);
  bool CaptureWithGDI(const RECT &rect, ID3D11Texture2D **out_texture);
  bool ApplyBlur(ID3D11ShaderResourceView *input,
                 ID3D11RenderTargetView *output, int width, int height,
                 float blur_radius);

  bool EnsureBlurResources(int width, int height);
  bool EnsureCaptureTexture(int width, int height);
  void ReleaseBlurResources();
  void ReleaseCaptureResources();
  void ReleaseFramePiece(int index);

  bool DrawQuad(float x, float y, float w, float h, float u0, float v0,
                float u1, float v1, ID3D11ShaderResourceView *srv,
                const float tint[4], float opacity, float blur_radius);
  bool RenderGlassQuad(const RECT &dest_rect);

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
  ID3D11Buffer *m_dynamic_vertex_buffer = nullptr;
  ID3D11Buffer *m_constant_buffer = nullptr;
  ID3D11SamplerState *m_sampler_state = nullptr;
  ID3D11BlendState *m_blend_state = nullptr;

  // Cached per-frame resources (recreated only when the size changes).
  ID3D11Texture2D *m_capture_default = nullptr;
  ID3D11ShaderResourceView *m_capture_srv = nullptr;
  ID3D11Texture2D *m_capture_dynamic = nullptr;
  ID3D11ShaderResourceView *m_capture_dynamic_srv = nullptr;
  ID3D11Texture2D *m_blur_texture = nullptr;
  ID3D11ShaderResourceView *m_blur_srv = nullptr;
  ID3D11RenderTargetView *m_blur_rtv = nullptr;
  int m_blur_width = 0;
  int m_blur_height = 0;

  // Cached theme frame piece textures (aero.msstyles parts 11-14).
  ID3D11Texture2D *m_frame_texture[kFramePartCount] = {};
  ID3D11ShaderResourceView *m_frame_srv[kFramePartCount] = {};
  int m_frame_width[kFramePartCount] = {};
  int m_frame_height[kFramePartCount] = {};

  RECT m_glass_rect = {0};
  bool m_has_glass_rect = false;

  int m_output_width = 0;
  int m_output_height = 0;
  int m_width = 0;
  int m_height = 0;
  bool m_initialized = false;
};

} // namespace glass
