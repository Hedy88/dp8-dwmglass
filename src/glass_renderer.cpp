// glass_renderer.cpp - D3D11 glass renderer with behind-window capture
#include "glass_renderer.h"
#include "logging.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

using namespace glass;

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

GlassRenderer::GlassRenderer() {}

GlassRenderer::~GlassRenderer() { Shutdown(); }

bool GlassRenderer::Initialize() {
  if (m_initialized)
    return true;

  if (!CreateDevice()) {
    printf("GlassRenderer: Failed to create D3D11 device\n");
    Log("GlassRenderer: CreateDevice FAILED");
    return false;
  }
  Log("GlassRenderer: CreateDevice done");

  if (!CreateShaders()) {
    printf("GlassRenderer: Failed to create shaders\n");
    Log("GlassRenderer: CreateShaders FAILED");
    DestroyDevice();
    return false;
  }
  Log("GlassRenderer: CreateShaders done");

  if (m_output_width <= 0 || m_output_height <= 0) {
    m_output_width = GetSystemMetrics(SM_CXSCREEN);
    m_output_height = GetSystemMetrics(SM_CYSCREEN);
  }

  m_initialized = true;
  printf("GlassRenderer: Initialized (output %dx%d)\n", m_output_width,
         m_output_height);
  return true;
}

void GlassRenderer::Shutdown() {
  if (!m_initialized)
    return;

  ReleaseCachedResources();
  DestroyShaders();
  DestroyDevice();

  m_initialized = false;
  printf("GlassRenderer: Shutdown\n");
}

void GlassRenderer::SetParams(const GlassParams &params) { m_params = params; }

void GlassRenderer::SetGlassRect(const RECT &rect) {
  m_glass_rect = rect;
  m_has_glass_rect = true;
}

void GlassRenderer::SetFrameImages(const FrameImageData *images, int count) {
  if (!m_device || !images || count <= 0)
    return;

  int n = count < kFramePartCount ? count : kFramePartCount;
  for (int i = 0; i < n; i++) {
    const FrameImageData &img = images[i];
    if (!img.pixels || img.width <= 0 || img.height <= 0)
      continue;

    ReleaseFramePiece(i);

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)img.width;
    td.Height = (UINT)img.height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem = img.pixels;
    init.SysMemPitch = (UINT)img.width * 4;

    if (FAILED(m_device->CreateTexture2D(&td, &init, &m_frame_texture[i])))
      continue;
    if (FAILED(m_device->CreateShaderResourceView(m_frame_texture[i], nullptr,
                                                  &m_frame_srv[i]))) {
      m_frame_texture[i]->Release();
      m_frame_texture[i] = nullptr;
      continue;
    }
    m_frame_width[i] = img.width;
    m_frame_height[i] = img.height;
    Log("GlassRenderer: cached frame piece %d (%dx%d)", i, img.width,
        img.height);
  }
}

bool GlassRenderer::RenderGlass(HWND hwnd, const RECT &dest_rect,
                                const RECT &source_rect) {
  SetGlassRect(dest_rect);
  return RenderGlassFrame();
}

bool GlassRenderer::RenderGlassFrame() {
  if (!m_initialized || !m_context || !m_device)
    return false;

  RECT rect = m_glass_rect;
  if (!m_has_glass_rect || rect.right - rect.left <= 0 ||
      rect.bottom - rect.top <= 0) {
    rect = {0, 0, m_output_width, m_output_height};
  }

  int width = rect.right - rect.left;
  int height = rect.bottom - rect.top;
  if (width <= 0 || height <= 0)
    return false;

  m_width = width;
  m_height = height;

  if (!EnsureBlurResources(width, height))
    return false;
  if (!EnsureCaptureTexture(width, height))
    return false;

  ID3D11Texture2D *captured = nullptr;
  ID3D11ShaderResourceView *capture_srv = nullptr;
  if (!CaptureBehindWindow(nullptr, rect, &captured, &capture_srv))
    return false;

  if (!ApplyBlur(capture_srv, m_blur_rtv, width, height,
                 m_params.blur_amount)) {
    printf("GlassRenderer: ApplyBlur failed\n");
    return false;
  }

  return RenderGlassQuad(rect);
}

// ---------------------------------------------------------------------------
// Capture
// ---------------------------------------------------------------------------

bool GlassRenderer::CaptureBehindWindow(HWND hwnd, const RECT &rect,
                                        ID3D11Texture2D **out_texture,
                                        ID3D11ShaderResourceView **out_srv) {
  if (!out_texture || !out_srv)
    return false;

  // Try Desktop Duplication API first (Windows 8+)
  if (m_duplication) {
    if (CaptureWithDesktopDuplication(hwnd, rect, &m_capture_default)) {
      *out_texture = m_capture_default;
      *out_srv = m_capture_srv;
      return true;
    }
  }

  // Fall back to GDI BitBlt
  if (CaptureWithGDI(rect, &m_capture_dynamic)) {
    *out_texture = m_capture_dynamic;
    *out_srv = m_capture_dynamic_srv;
    return true;
  }

  return false;
}

bool GlassRenderer::CaptureWithDesktopDuplication(HWND hwnd, const RECT &rect,
                                                  ID3D11Texture2D **out_texture) {
  if (!m_duplication || !m_context || !m_device || !out_texture)
    return false;

  // Acquire next desktop frame
  DXGI_OUTDUPL_FRAME_INFO frame_info = {};
  IDXGIResource *desktop_resource = nullptr;
  HRESULT hr =
      m_duplication->AcquireNextFrame(1000, &frame_info, &desktop_resource);
  if (FAILED(hr)) {
    printf("GlassRenderer: AcquireNextFrame failed: 0x%08x\n", hr);
    return false;
  }

  ID3D11Texture2D *desktop_tex = nullptr;
  hr = desktop_resource->QueryInterface(__uuidof(ID3D11Texture2D),
                                        (void **)&desktop_tex);
  if (FAILED(hr)) {
    m_duplication->ReleaseFrame();
    desktop_resource->Release();
    return false;
  }

  // Copy the region into the cached capture texture
  D3D11_BOX src_box = {(UINT)rect.left,  (UINT)rect.top,    0,
                       (UINT)rect.right, (UINT)rect.bottom, 1};
  m_context->CopySubresourceRegion(*out_texture, 0, 0, 0, 0, desktop_tex, 0,
                                   &src_box);

  // Cleanup
  desktop_tex->Release();
  desktop_resource->Release();
  m_duplication->ReleaseFrame();

  return true;
}

bool GlassRenderer::CaptureWithGDI(const RECT &rect,
                                   ID3D11Texture2D **out_texture) {
  if (!out_texture)
    return false;

  int width = rect.right - rect.left;
  int height = rect.bottom - rect.top;

  if (width <= 0 || height <= 0)
    return false;

  // Get screen DC
  HDC hScreenDC = GetDC(nullptr);
  if (!hScreenDC)
    return false;

  HDC hMemDC = CreateCompatibleDC(hScreenDC);
  if (!hMemDC) {
    ReleaseDC(nullptr, hScreenDC);
    return false;
  }

  // Create bitmap
  BITMAPINFO bmi = {0};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = width;
  bmi.bmiHeader.biHeight = -height; // top-down
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  BYTE *pixels = nullptr;
  HBITMAP hBitmap = CreateDIBSection(hMemDC, &bmi, DIB_RGB_COLORS,
                                     (void **)&pixels, nullptr, 0);
  if (!hBitmap || !pixels) {
    DeleteDC(hMemDC);
    ReleaseDC(nullptr, hScreenDC);
    return false;
  }

  SelectObject(hMemDC, hBitmap);

  // Capture screen area
  if (!BitBlt(hMemDC, 0, 0, width, height, hScreenDC, rect.left, rect.top,
              SRCCOPY | CAPTUREBLT)) {
    printf("GlassRenderer: BitBlt failed\n");
    DeleteObject(hBitmap);
    DeleteDC(hMemDC);
    ReleaseDC(nullptr, hScreenDC);
    return false;
  }

  // Map and copy pixels into the cached dynamic texture
  D3D11_MAPPED_SUBRESOURCE mapped = {};
  HRESULT hr = m_context->Map(*out_texture, 0, D3D11_MAP_WRITE_DISCARD, 0,
                              &mapped);
  if (SUCCEEDED(hr)) {
    BYTE *dst = (BYTE *)mapped.pData;
    for (int y = 0; y < height; y++) {
      memcpy(dst + y * mapped.RowPitch, pixels + y * width * 4, width * 4);
    }
    m_context->Unmap(*out_texture, 0);
  } else {
    printf("GlassRenderer: Map failed in CaptureWithGDI: 0x%08x\n", hr);
    hr = S_OK;
  }

  // Cleanup
  DeleteObject(hBitmap);
  DeleteDC(hMemDC);
  ReleaseDC(nullptr, hScreenDC);

  return SUCCEEDED(hr);
}

// ---------------------------------------------------------------------------
// Draw helpers
// ---------------------------------------------------------------------------

bool GlassRenderer::ApplyBlur(ID3D11ShaderResourceView *input,
                              ID3D11RenderTargetView *output, int width,
                              int height, float blur_radius) {
  if (!m_context || !input || !output)
    return false;

  m_context->OMSetRenderTargets(1, &output, nullptr);

  D3D11_VIEWPORT vp = {0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f};
  m_context->RSSetViewports(1, &vp);

  float tint[4] = {m_params.tint_color.r, m_params.tint_color.g,
                   m_params.tint_color.b, m_params.tint_color.a};
  return DrawQuad(0.0f, 0.0f, (float)width, (float)height, 0.0f, 0.0f, 1.0f,
                  1.0f, input, tint, m_params.opacity, blur_radius);
}

bool GlassRenderer::DrawQuad(float x, float y, float w, float h, float u0,
                             float v0, float u1, float v1,
                             ID3D11ShaderResourceView *srv,
                             const float tint[4], float opacity,
                             float blur_radius) {
  if (!m_context || !m_device || !srv || !m_dynamic_vertex_buffer)
    return false;

  float cx0 = x * 2.0f / m_width - 1.0f;
  float cy0 = 1.0f - y * 2.0f / m_height;
  float cx1 = (x + w) * 2.0f / m_width - 1.0f;
  float cy1 = 1.0f - (y + h) * 2.0f / m_height;

  // pos (xyzw), tex (uv) - triangle strip
  float vertices[24] = {
      cx0, cy0, 0.0f, 1.0f, u0, v0,
      cx1, cy0, 0.0f, 1.0f, u1, v0,
      cx0, cy1, 0.0f, 1.0f, u0, v1,
      cx1, cy1, 0.0f, 1.0f, u1, v1,
  };

  D3D11_MAPPED_SUBRESOURCE mapped = {};
  if (FAILED(m_context->Map(m_dynamic_vertex_buffer, 0, D3D11_MAP_WRITE_DISCARD,
                            0, &mapped)))
    return false;
  memcpy(mapped.pData, vertices, sizeof(vertices));
  m_context->Unmap(m_dynamic_vertex_buffer, 0);

  UINT stride = sizeof(float) * 4;
  UINT offset = 0;
  m_context->IASetVertexBuffers(0, 1, &m_dynamic_vertex_buffer, &stride,
                                &offset);
  m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  m_context->IASetInputLayout(m_input_layout);
  m_context->VSSetShader(m_vertex_shader, nullptr, 0);
  m_context->PSSetShader(m_pixel_shader, nullptr, 0);
  m_context->PSSetSamplers(0, 1, &m_sampler_state);
  m_context->OMSetBlendState(m_blend_state, nullptr, 0xffffffff);

  m_context->PSSetShaderResources(0, 1, &srv);

  // Resolution drives the blur sample offset; the blur pass always samples a
  // texture sized to the current output, so m_width/m_height is correct here.
  struct Constants {
    float tint[4];
    float blur_radius;
    float opacity;
    float resolution[2];
    float padding[2];
  } constants = {{tint[0], tint[1], tint[2], tint[3]}, blur_radius, opacity,
                 {(float)m_width, (float)m_height}, {0.0f, 0.0f}};

  m_context->UpdateSubresource(m_constant_buffer, 0, nullptr, &constants, 0, 0);
  m_context->PSSetConstantBuffers(0, 1, &m_constant_buffer);

  m_context->Draw(4, 0);

  ID3D11ShaderResourceView *null_srv = nullptr;
  m_context->PSSetShaderResources(0, 1, &null_srv);

  return true;
}

bool GlassRenderer::RenderGlassQuad(const RECT &dest_rect) {
  if (!m_initialized || !m_context)
    return false;

  float w = (float)(dest_rect.right - dest_rect.left);
  float h = (float)(dest_rect.bottom - dest_rect.top);

  if (w <= 0 || h <= 0)
    return false;

  COLORREF accent = m_theme_colors.accent;
  float frame_tint[4] = {GetRValue(accent) / 255.0f,
                         GetGValue(accent) / 255.0f,
                         GetBValue(accent) / 255.0f, 0.6f};

  float lt = m_frame_srv[kFrameLeft] ? (float)m_frame_width[kFrameLeft] : 0.0f;
  float rt = m_frame_srv[kFrameRight] ? (float)m_frame_width[kFrameRight] : 0.0f;
  float tt = m_frame_srv[kFrameTop] ? (float)m_frame_height[kFrameTop] : 0.0f;
  float bt = m_frame_srv[kFrameBottom] ? (float)m_frame_height[kFrameBottom]
                                       : 0.0f;

  // Compose the theme frame pieces over the blurred glass quad.
  if (m_frame_srv[kFrameTop])
    DrawQuad(0.0f, 0.0f, w, tt, 0.0f, 0.0f, 1.0f, 1.0f, m_frame_srv[kFrameTop],
             frame_tint, 1.0f, 0.0f);
  if (m_frame_srv[kFrameBottom])
    DrawQuad(0.0f, h - bt, w, bt, 0.0f, 0.0f, 1.0f, 1.0f,
             m_frame_srv[kFrameBottom], frame_tint, 1.0f, 0.0f);
  if (m_frame_srv[kFrameLeft])
    DrawQuad(0.0f, 0.0f, lt, h, 0.0f, 0.0f, 1.0f, 1.0f,
             m_frame_srv[kFrameLeft], frame_tint, 1.0f, 0.0f);
  if (m_frame_srv[kFrameRight])
    DrawQuad(w - rt, 0.0f, rt, h, 0.0f, 0.0f, 1.0f, 1.0f,
             m_frame_srv[kFrameRight], frame_tint, 1.0f, 0.0f);

  return true;
}

// ---------------------------------------------------------------------------
// Resource management
// ---------------------------------------------------------------------------

bool GlassRenderer::EnsureBlurResources(int width, int height) {
  if (m_blur_width == width && m_blur_height == height && m_blur_texture)
    return true;

  ReleaseBlurResources();

  D3D11_TEXTURE2D_DESC td = {};
  td.Width = (UINT)width;
  td.Height = (UINT)height;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

  if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_blur_texture)))
    return false;
  if (FAILED(m_device->CreateShaderResourceView(m_blur_texture, nullptr,
                                                &m_blur_srv)))
    return false;
  if (FAILED(m_device->CreateRenderTargetView(m_blur_texture, nullptr,
                                              &m_blur_rtv)))
    return false;

  m_blur_width = width;
  m_blur_height = height;
  return true;
}

bool GlassRenderer::EnsureCaptureTexture(int width, int height) {
  if (m_capture_default && m_capture_dynamic && m_capture_srv &&
      m_capture_dynamic_srv && m_blur_width == width &&
      m_blur_height == height)
    return true;

  ReleaseCaptureResources();

  D3D11_TEXTURE2D_DESC td = {};
  td.Width = (UINT)width;
  td.Height = (UINT)height;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;

  // Default usage texture for the desktop duplication path.
  D3D11_TEXTURE2D_DESC default_desc = td;
  default_desc.Usage = D3D11_USAGE_DEFAULT;
  default_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  if (FAILED(m_device->CreateTexture2D(&default_desc, nullptr,
                                       &m_capture_default)))
    return false;
  if (FAILED(m_device->CreateShaderResourceView(m_capture_default, nullptr,
                                                &m_capture_srv)))
    return false;

  // Dynamic usage texture for the GDI fallback path.
  D3D11_TEXTURE2D_DESC dynamic_desc = td;
  dynamic_desc.Usage = D3D11_USAGE_DYNAMIC;
  dynamic_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  dynamic_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  if (FAILED(m_device->CreateTexture2D(&dynamic_desc, nullptr,
                                       &m_capture_dynamic)))
    return false;
  if (FAILED(m_device->CreateShaderResourceView(m_capture_dynamic, nullptr,
                                                &m_capture_dynamic_srv)))
    return false;

  return true;
}

void GlassRenderer::ReleaseBlurResources() {
  if (m_blur_rtv) {
    m_blur_rtv->Release();
    m_blur_rtv = nullptr;
  }
  if (m_blur_srv) {
    m_blur_srv->Release();
    m_blur_srv = nullptr;
  }
  if (m_blur_texture) {
    m_blur_texture->Release();
    m_blur_texture = nullptr;
  }
  m_blur_width = 0;
  m_blur_height = 0;
}

void GlassRenderer::ReleaseCaptureResources() {
  if (m_capture_dynamic_srv) {
    m_capture_dynamic_srv->Release();
    m_capture_dynamic_srv = nullptr;
  }
  if (m_capture_dynamic) {
    m_capture_dynamic->Release();
    m_capture_dynamic = nullptr;
  }
  if (m_capture_srv) {
    m_capture_srv->Release();
    m_capture_srv = nullptr;
  }
  if (m_capture_default) {
    m_capture_default->Release();
    m_capture_default = nullptr;
  }
}

void GlassRenderer::ReleaseFramePiece(int index) {
  if (index < 0 || index >= kFramePartCount)
    return;
  if (m_frame_srv[index]) {
    m_frame_srv[index]->Release();
    m_frame_srv[index] = nullptr;
  }
  if (m_frame_texture[index]) {
    m_frame_texture[index]->Release();
    m_frame_texture[index] = nullptr;
  }
  m_frame_width[index] = 0;
  m_frame_height[index] = 0;
}

void GlassRenderer::ReleaseCachedResources() {
  for (int i = 0; i < kFramePartCount; i++)
    ReleaseFramePiece(i);
  ReleaseCaptureResources();
  ReleaseBlurResources();
}

bool GlassRenderer::CreateDevice() {
  Log("GlassRenderer: CreateDevice: initializing desktop duplication...");
  // Initialize Desktop Duplication if on Windows 8+
  InitDesktopDuplication();
  Log("GlassRenderer: CreateDevice: InitDesktopDuplication returned");

  D3D_FEATURE_LEVEL feature_levels[] = {
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_1,
      D3D_FEATURE_LEVEL_10_0,
  };

  D3D_FEATURE_LEVEL feature_level;

  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
  flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

  Log("GlassRenderer: CreateDevice: D3D11CreateDevice...");
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 flags, feature_levels,
                                 _countof(feature_levels), D3D11_SDK_VERSION,
                                 &m_device, &feature_level, &m_context);

  if (FAILED(hr)) {
    printf("GlassRenderer: D3D11CreateDevice failed: 0x%08x\n", hr);
    Log("GlassRenderer: D3D11CreateDevice failed: 0x%08x", (unsigned)hr);
    return false;
  }

  printf("GlassRenderer: D3D11 device created (feature level 0x%x)\n",
         feature_level);
  Log("GlassRenderer: D3D11 device created (feature level 0x%x)",
      (unsigned)feature_level);
  return true;
}

bool GlassRenderer::InitDesktopDuplication() {
  if (!m_device)
    return false;

  Log("GlassRenderer: InitDesktopDuplication: getting DXGI device...");
  // Get DXGI device
  IDXGIDevice *dxgi_device = nullptr;
  HRESULT hr =
      m_device->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgi_device);
  if (FAILED(hr)) {
    printf("GlassRenderer: Failed to get DXGI device\n");
    Log("GlassRenderer: InitDesktopDuplication: IDXGIDevice failed 0x%08x",
        (unsigned)hr);
    return false;
  }

  // Get adapter
  IDXGIAdapter *adapter = nullptr;
  hr = dxgi_device->GetAdapter(&adapter);
  dxgi_device->Release();
  if (FAILED(hr)) {
    printf("GlassRenderer: Failed to get adapter\n");
    Log("GlassRenderer: InitDesktopDuplication: GetAdapter failed 0x%08x",
        (unsigned)hr);
    return false;
  }

  // Get output (primary monitor)
  IDXGIOutput *output = nullptr;
  hr = adapter->EnumOutputs(0, &output);
  adapter->Release();
  if (FAILED(hr)) {
    printf("GlassRenderer: No outputs found\n");
    Log("GlassRenderer: InitDesktopDuplication: EnumOutputs failed 0x%08x",
        (unsigned)hr);
    return false;
  }

  DXGI_OUTPUT_DESC output_desc = {};
  output->GetDesc(&output_desc);
  m_output_width = output_desc.DesktopCoordinates.right -
                   output_desc.DesktopCoordinates.left;
  m_output_height = output_desc.DesktopCoordinates.bottom -
                    output_desc.DesktopCoordinates.top;

  // Get output1 for duplication
  IDXGIOutput1 *output1 = nullptr;
  hr = output->QueryInterface(__uuidof(IDXGIOutput1), (void **)&output1);
  output->Release();
  if (FAILED(hr)) {
    printf("GlassRenderer: IDXGIOutput1 not supported\n");
    Log("GlassRenderer: InitDesktopDuplication: IDXGIOutput1 failed 0x%08x",
        (unsigned)hr);
    return false;
  }

  // Create desktop duplication
  hr = output1->DuplicateOutput(m_device, &m_duplication);
  output1->Release();
  if (FAILED(hr)) {
    printf("GlassRenderer: DuplicateOutput failed: 0x%08x\n", hr);
    Log("GlassRenderer: InitDesktopDuplication: DuplicateOutput failed 0x%08x",
        (unsigned)hr);
    m_duplication = nullptr;
    return false;
  }

  printf("GlassRenderer: Desktop Duplication initialized (%dx%d)\n",
         m_output_width, m_output_height);
  Log("GlassRenderer: Desktop Duplication initialized");
  return true;
}

void GlassRenderer::DestroyDevice() {
  if (m_duplication) {
    m_duplication->Release();
    m_duplication = nullptr;
  }
  if (m_context) {
    m_context->Release();
    m_context = nullptr;
  }
  if (m_device) {
    m_device->Release();
    m_device = nullptr;
  }
}

bool GlassRenderer::CreateShaders() {
  Log("GlassRenderer: CreateShaders: compiling shaders...");
  // Simple vertex shader
  const char *vs_source = R"(
        struct VS_INPUT {
            float4 pos : POSITION;
            float2 tex : TEXCOORD;
        };
        struct VS_OUTPUT {
            float4 pos : SV_POSITION;
            float2 tex : TEXCOORD;
        };
        VS_OUTPUT main(VS_INPUT input) {
            VS_OUTPUT output;
            output.pos = input.pos;
            output.tex = input.tex;
            return output;
        }
    )";

  // Pixel shader with blur
  const char *ps_source = R"(
        Texture2D g_Texture : register(t0);
        SamplerState g_Sampler : register(s0);

        cbuffer GlassConstants : register(b0) {
            float4 g_TintColor;
            float g_BlurRadius;
            float g_Opacity;
            float2 g_Resolution;
            float2 g_Padding;
        };

        struct PS_INPUT {
            float4 pos : SV_POSITION;
            float2 tex : TEXCOORD;
        };

        float4 main(PS_INPUT input) : SV_TARGET {
            float2 texel_size = 1.0f / g_Resolution;

            float4 color = float4(0.0f, 0.0f, 0.0f, 0.0f);

            // 9-tap Gaussian blur
            float weights[5] = { 0.227027f, 0.1945946f, 0.1216216f, 0.054054f, 0.016216f };

            color += g_Texture.Sample(g_Sampler, input.tex) * weights[0];

            for (int i = 1; i < 5; i++) {
                float2 offset = texel_size * g_BlurRadius * i;
                color += g_Texture.Sample(g_Sampler, input.tex + float2(offset.x, 0.0f)) * weights[i];
                color += g_Texture.Sample(g_Sampler, input.tex - float2(offset.x, 0.0f)) * weights[i];
                color += g_Texture.Sample(g_Sampler, input.tex + float2(0.0f, offset.y)) * weights[i];
                color += g_Texture.Sample(g_Sampler, input.tex - float2(0.0f, offset.y)) * weights[i];
            }

            // Apply tint
            color.rgb = lerp(color.rgb, g_TintColor.rgb, g_TintColor.a);

            // Apply overall opacity
            color.a *= g_Opacity;

            return color;
        }
    )";

  ID3DBlob *vs_blob = nullptr;
  ID3DBlob *ps_blob = nullptr;
  ID3DBlob *error_blob = nullptr;

  HRESULT hr =
      D3DCompile(vs_source, strlen(vs_source), nullptr, nullptr, nullptr,
                 "main", "vs_4_0", 0, 0, &vs_blob, &error_blob);
  if (FAILED(hr)) {
    printf("GlassRenderer: Vertex shader compile failed\n");
    if (error_blob)
      error_blob->Release();
    return false;
  }

  hr = m_device->CreateVertexShader(vs_blob->GetBufferPointer(),
                                    vs_blob->GetBufferSize(), nullptr,
                                    &m_vertex_shader);
  if (FAILED(hr)) {
    vs_blob->Release();
    return false;
  }

  hr = D3DCompile(ps_source, strlen(ps_source), nullptr, nullptr, nullptr,
                  "main", "ps_4_0", 0, 0, &ps_blob, &error_blob);
  if (FAILED(hr)) {
    printf("GlassRenderer: Pixel shader compile failed\n");
    vs_blob->Release();
    if (error_blob)
      error_blob->Release();
    return false;
  }

  hr = m_device->CreatePixelShader(ps_blob->GetBufferPointer(),
                                   ps_blob->GetBufferSize(), nullptr,
                                   &m_pixel_shader);
  if (FAILED(hr)) {
    vs_blob->Release();
    ps_blob->Release();
    return false;
  }

  // Input layout
  D3D11_INPUT_ELEMENT_DESC layout[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,
       D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16,
       D3D11_INPUT_PER_VERTEX_DATA, 0},
  };

  hr = m_device->CreateInputLayout(layout, 2, vs_blob->GetBufferPointer(),
                                   vs_blob->GetBufferSize(), &m_input_layout);
  if (FAILED(hr)) {
    vs_blob->Release();
    ps_blob->Release();
    return false;
  }

  // Vertex buffer for a fullscreen quad (unused now, kept for the blur pass)
  float vertices[] = {
      // pos (xy), tex (uv)
      -1.0f, 1.0f,  0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f,  0.0f, 1.0f, 1.0f, 0.0f,
      -1.0f, -1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 1.0f,
  };

  D3D11_BUFFER_DESC vb_desc = {};
  vb_desc.Usage = D3D11_USAGE_DEFAULT;
  vb_desc.ByteWidth = sizeof(vertices);
  vb_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

  D3D11_SUBRESOURCE_DATA init_data = {};
  init_data.pSysMem = vertices;

  hr = m_device->CreateBuffer(&vb_desc, &init_data, &m_vertex_buffer);
  if (FAILED(hr)) {
    vs_blob->Release();
    ps_blob->Release();
    return false;
  }

  // Dynamic vertex buffer for per-draw quads
  D3D11_BUFFER_DESC dyn_desc = {};
  dyn_desc.Usage = D3D11_USAGE_DYNAMIC;
  dyn_desc.ByteWidth = sizeof(float) * 4 * 4; // 4 verts * (pos4 + uv2)
  dyn_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  dyn_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

  hr = m_device->CreateBuffer(&dyn_desc, nullptr, &m_dynamic_vertex_buffer);
  if (FAILED(hr)) {
    vs_blob->Release();
    ps_blob->Release();
    return false;
  }

  // Constant buffer
  D3D11_BUFFER_DESC cb_desc = {};
  cb_desc.ByteWidth =
      sizeof(float) *
      8; // 4 for tint, 1 for blur, 1 for opacity, 2 for resolution
  cb_desc.Usage = D3D11_USAGE_DEFAULT;
  cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

  hr = m_device->CreateBuffer(&cb_desc, nullptr, &m_constant_buffer);
  if (FAILED(hr)) {
    vs_blob->Release();
    ps_blob->Release();
    return false;
  }

  // Sampler state
  D3D11_SAMPLER_DESC samp_desc = {};
  samp_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  samp_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
  samp_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
  samp_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  samp_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
  samp_desc.MinLOD = 0;
  samp_desc.MaxLOD = D3D11_FLOAT32_MAX;

  hr = m_device->CreateSamplerState(&samp_desc, &m_sampler_state);
  if (FAILED(hr)) {
    vs_blob->Release();
    ps_blob->Release();
    return false;
  }

  // Alpha blend state for frame-piece compositing
  D3D11_BLEND_DESC blend_desc = {};
  blend_desc.RenderTarget[0].BlendEnable = TRUE;
  blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
  blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_SRC_ALPHA;
  blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
  blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

  hr = m_device->CreateBlendState(&blend_desc, &m_blend_state);
  if (FAILED(hr)) {
    vs_blob->Release();
    ps_blob->Release();
    return false;
  }

  vs_blob->Release();
  ps_blob->Release();

  printf("GlassRenderer: Shaders created\n");
  return true;
}

void GlassRenderer::DestroyShaders() {
  if (m_blend_state) {
    m_blend_state->Release();
    m_blend_state = nullptr;
  }
  if (m_sampler_state) {
    m_sampler_state->Release();
    m_sampler_state = nullptr;
  }
  if (m_constant_buffer) {
    m_constant_buffer->Release();
    m_constant_buffer = nullptr;
  }
  if (m_dynamic_vertex_buffer) {
    m_dynamic_vertex_buffer->Release();
    m_dynamic_vertex_buffer = nullptr;
  }
  if (m_vertex_buffer) {
    m_vertex_buffer->Release();
    m_vertex_buffer = nullptr;
  }
  if (m_input_layout) {
    m_input_layout->Release();
    m_input_layout = nullptr;
  }
  if (m_pixel_shader) {
    m_pixel_shader->Release();
    m_pixel_shader = nullptr;
  }
  if (m_vertex_shader) {
    m_vertex_shader->Release();
    m_vertex_shader = nullptr;
  }
}
