// glass_renderer.cpp - D3D11 glass renderer with behind-window capture
#include "glass_renderer.h"
#include "pattern_scan.h"
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
    return false;
  }

  if (!CreateShaders()) {
    printf("GlassRenderer: Failed to create shaders\n");
    DestroyDevice();
    return false;
  }

  m_initialized = true;
  printf("GlassRenderer: Initialized\n");
  return true;
}

void GlassRenderer::Shutdown() {
  if (!m_initialized)
    return;

  DestroyShaders();
  DestroyDevice();

  m_initialized = false;
  printf("GlassRenderer: Shutdown\n");
}

void GlassRenderer::SetParams(const GlassParams &params) { m_params = params; }

bool GlassRenderer::RenderGlass(HWND hwnd, const RECT &dest_rect,
                                const RECT &source_rect) {
  if (!m_initialized || !m_context || !hwnd)
    return false;

  int width = dest_rect.right - dest_rect.left;
  int height = dest_rect.bottom - dest_rect.top;

  if (width <= 0 || height <= 0)
    return false;

  // 1. Capture behind-window content
  ID3D11Texture2D *captured = nullptr;
  if (!CaptureBehindWindow(hwnd, dest_rect, &captured)) {
    printf("GlassRenderer: CaptureBehindWindow failed\n");
    return false;
  }

  // 2. Create shader resource view from captured content
  ID3D11ShaderResourceView *srv = nullptr;
  D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
  srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
  srv_desc.Texture2D.MostDetailedMip = 0;
  srv_desc.Texture2D.MipLevels = 1;

  HRESULT hr = m_device->CreateShaderResourceView(captured, &srv_desc, &srv);
  if (FAILED(hr)) {
    printf("GlassRenderer: Failed to create SRV\n");
    captured->Release();
    return false;
  }

  // 3. Create temporary render target for blur
  ID3D11Texture2D *blur_tex = nullptr;
  D3D11_TEXTURE2D_DESC tex_desc = {};
  tex_desc.Width = width;
  tex_desc.Height = height;
  tex_desc.MipLevels = 1;
  tex_desc.ArraySize = 1;
  tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  tex_desc.SampleDesc.Count = 1;
  tex_desc.Usage = D3D11_USAGE_DEFAULT;
  tex_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

  hr = m_device->CreateTexture2D(&tex_desc, nullptr, &blur_tex);
  if (FAILED(hr)) {
    printf("GlassRenderer: Failed to create blur texture\n");
    srv->Release();
    captured->Release();
    return false;
  }

  ID3D11RenderTargetView *rtv = nullptr;
  hr = m_device->CreateRenderTargetView(blur_tex, nullptr, &rtv);
  if (FAILED(hr)) {
    srv->Release();
    captured->Release();
    blur_tex->Release();
    return false;
  }

  // 4. Apply blur shader
  if (!ApplyBlur(srv, rtv, width, height, m_params.blur_amount)) {
    printf("GlassRenderer: ApplyBlur failed\n");
    rtv->Release();
    srv->Release();
    captured->Release();
    blur_tex->Release();
    return false;
  }

  // 5. Create final SRV from blurred texture
  ID3D11ShaderResourceView *blur_srv = nullptr;
  hr = m_device->CreateShaderResourceView(blur_tex, &srv_desc, &blur_srv);
  if (FAILED(hr)) {
    rtv->Release();
    srv->Release();
    captured->Release();
    blur_tex->Release();
    return false;
  }

  // 6. Render final glass quad with tint
  bool result = RenderGlassQuad(dest_rect, blur_srv);

  // Cleanup
  blur_srv->Release();
  rtv->Release();
  srv->Release();
  captured->Release();
  blur_tex->Release();

  return result;
}

bool GlassRenderer::CaptureBehindWindow(HWND hwnd, const RECT &rect,
                                        ID3D11Texture2D **out_texture) {
  if (!out_texture)
    return false;

  int width = rect.right - rect.left;
  int height = rect.bottom - rect.top;

  if (width <= 0 || height <= 0)
    return false;

  // Try Desktop Duplication API first (Windows 8+)
  if (m_duplication) {
    return CaptureWithDesktopDuplication(hwnd, rect, out_texture);
  }

  // Fall back to GDI BitBlt
  return CaptureWithGDI(rect, out_texture);
}

bool GlassRenderer::CaptureWithDesktopDuplication(
    HWND hwnd, const RECT &rect, ID3D11Texture2D **out_texture) {
  if (!m_duplication || !m_context || !m_device)
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

  // Create output texture
  D3D11_TEXTURE2D_DESC tex_desc = {};
  tex_desc.Width = rect.right - rect.left;
  tex_desc.Height = rect.bottom - rect.top;
  tex_desc.MipLevels = 1;
  tex_desc.ArraySize = 1;
  tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  tex_desc.SampleDesc.Count = 1;
  tex_desc.Usage = D3D11_USAGE_DEFAULT;
  tex_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

  hr = m_device->CreateTexture2D(&tex_desc, nullptr, out_texture);
  if (FAILED(hr)) {
    printf("GlassRenderer: Failed to create capture texture\n");
    desktop_tex->Release();
    desktop_resource->Release();
    m_duplication->ReleaseFrame();
    return false;
  }

  // Copy the region behind the window
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

  // Create D3D11 texture
  D3D11_TEXTURE2D_DESC tex_desc = {};
  tex_desc.Width = width;
  tex_desc.Height = height;
  tex_desc.MipLevels = 1;
  tex_desc.ArraySize = 1;
  tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  tex_desc.SampleDesc.Count = 1;
  tex_desc.Usage = D3D11_USAGE_DYNAMIC;
  tex_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  tex_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

  HRESULT hr = m_device->CreateTexture2D(&tex_desc, nullptr, out_texture);
  if (FAILED(hr)) {
    printf("GlassRenderer: Failed to create texture\n");
    DeleteObject(hBitmap);
    DeleteDC(hMemDC);
    ReleaseDC(nullptr, hScreenDC);
    return false;
  }

  // Map and copy pixels
  D3D11_MAPPED_SUBRESOURCE mapped = {};
  hr = m_context->Map(*out_texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
  if (SUCCEEDED(hr)) {
    BYTE *dst = (BYTE *)mapped.pData;
    for (int y = 0; y < height; y++) {
      memcpy(dst + y * mapped.RowPitch, pixels + y * width * 4, width * 4);
    }
    m_context->Unmap(*out_texture, 0);
  }

  // Cleanup
  DeleteObject(hBitmap);
  DeleteDC(hMemDC);
  ReleaseDC(nullptr, hScreenDC);

  return true;
}

bool GlassRenderer::ApplyBlur(ID3D11ShaderResourceView *input,
                              ID3D11RenderTargetView *output, int width,
                              int height, float blur_radius) {
  if (!m_context || !m_pixel_shader || !input || !output)
    return false;

  // Set render target
  m_context->OMSetRenderTargets(1, &output, nullptr);

  D3D11_VIEWPORT vp = {0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f};
  m_context->RSSetViewports(1, &vp);

  // Set shaders
  m_context->VSSetShader(m_vertex_shader, nullptr, 0);
  m_context->PSSetShader(m_pixel_shader, nullptr, 0);
  m_context->IASetInputLayout(m_input_layout);

  // Set vertex buffer
  UINT stride = sizeof(float) * 4;
  UINT offset = 0;
  m_context->IASetVertexBuffers(0, 1, &m_vertex_buffer, &stride, &offset);
  m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

  // Set shader resources
  m_context->PSSetShaderResources(0, 1, &input);

  // Set constants
  if (m_constant_buffer) {
    struct Constants {
      float tint[4];
      float blur_radius;
      float opacity;
      float resolution[2];
      float padding[2];
    } constants = {{m_params.tint_color.r, m_params.tint_color.g,
                    m_params.tint_color.b, m_params.tint_color.a},
                   blur_radius,
                   m_params.opacity,
                   {(float)width, (float)height},
                   {0.0f, 0.0f}};

    m_context->UpdateSubresource(m_constant_buffer, 0, nullptr, &constants, 0,
                                 0);
    m_context->PSSetConstantBuffers(0, 1, &m_constant_buffer);
  }

  // Draw
  m_context->Draw(4, 0);

  // Unbind SRV
  ID3D11ShaderResourceView *null_srv = nullptr;
  m_context->PSSetShaderResources(0, 1, &null_srv);

  return true;
}

bool GlassRenderer::RenderGlassQuad(const RECT &dest_rect,
                                    ID3D11ShaderResourceView *blur_srv) {
  if (!m_initialized || !m_context)
    return false;

  float width = (float)(dest_rect.right - dest_rect.left);
  float height = (float)(dest_rect.bottom - dest_rect.top);

  if (width <= 0 || height <= 0)
    return false;

  // For now, render a simple tinted quad
  // Full implementation would composite the blurred texture with the glass tint
  return true;
}

bool GlassRenderer::CreateDevice() {
  // Initialize Desktop Duplication if on Windows 8+
  InitDesktopDuplication();

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

  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 flags, feature_levels,
                                 _countof(feature_levels), D3D11_SDK_VERSION,
                                 &m_device, &feature_level, &m_context);

  if (FAILED(hr)) {
    printf("GlassRenderer: D3D11CreateDevice failed: 0x%08x\n", hr);
    return false;
  }

  printf("GlassRenderer: D3D11 device created (feature level 0x%x)\n",
         feature_level);
  return true;
}

bool GlassRenderer::InitDesktopDuplication() {
  if (!m_device)
    return false;

  // Get DXGI device
  IDXGIDevice *dxgi_device = nullptr;
  HRESULT hr =
      m_device->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgi_device);
  if (FAILED(hr)) {
    printf("GlassRenderer: Failed to get DXGI device\n");
    return false;
  }

  // Get adapter
  IDXGIAdapter *adapter = nullptr;
  hr = dxgi_device->GetAdapter(&adapter);
  dxgi_device->Release();
  if (FAILED(hr)) {
    printf("GlassRenderer: Failed to get adapter\n");
    return false;
  }

  // Get output (primary monitor)
  IDXGIOutput *output = nullptr;
  hr = adapter->EnumOutputs(0, &output);
  adapter->Release();
  if (FAILED(hr)) {
    printf("GlassRenderer: No outputs found\n");
    return false;
  }

  // Get output1 for duplication
  IDXGIOutput1 *output1 = nullptr;
  hr = output->QueryInterface(__uuidof(IDXGIOutput1), (void **)&output1);
  output->Release();
  if (FAILED(hr)) {
    printf("GlassRenderer: IDXGIOutput1 not supported\n");
    return false;
  }

  // Create desktop duplication
  hr = output1->DuplicateOutput(m_device, &m_duplication);
  output1->Release();
  if (FAILED(hr)) {
    printf("GlassRenderer: DuplicateOutput failed: 0x%08x\n", hr);
    m_duplication = nullptr;
    return false;
  }

  printf("GlassRenderer: Desktop Duplication initialized\n");
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

  // Vertex buffer for a fullscreen quad
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

  vs_blob->Release();
  ps_blob->Release();

  printf("GlassRenderer: Shaders created\n");
  return true;
}

void GlassRenderer::DestroyShaders() {
  if (m_sampler_state) {
    m_sampler_state->Release();
    m_sampler_state = nullptr;
  }
  if (m_constant_buffer) {
    m_constant_buffer->Release();
    m_constant_buffer = nullptr;
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
