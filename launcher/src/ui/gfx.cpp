// SPDX-License-Identifier: GPL-2.0-or-later
#include "gfx.h"

#include <d3dcompiler.h>
#include <dwrite_1.h>

#include <algorithm>
#include <cstring>

namespace {

// The background: a Blood Moon low over the window (a lit sphere), self-warped smoke drifting
// across it, low mist, a lantern glow at the cursor and film grain. Over the cover art only smoke, a blood
// tint and a vignette.
const char* SKY_HLSL = R"(
cbuffer Frame : register(b0) {
    float2 uRes; float uTime; float uArt; float2 uMouse; float2 uLamp; float uLampA; float uReveal; float2 pad;
};
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 p = float2(id == 2 ? 3.0 : -1.0, id == 1 ? 3.0 : -1.0);  // clockwise: not culled
    return float4(p, 0.0, 1.0);
}
float hash(float2 p) { p = frac(p * float2(123.34, 456.21)); p += dot(p, p + 45.32); return frac(p.x * p.y); }
float noise(float2 p) {
    float2 i = floor(p), f = frac(p);
    float2 u = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(hash(i), hash(i + float2(1, 0)), u.x), lerp(hash(i + float2(0, 1)), hash(i + float2(1, 1)), u.x), u.y);
}
float fbm(float2 p) {
    float v = 0.0, a = 0.5;
    [unroll] for (int i = 0; i < 5; i++) { v += a * noise(p); p = float2(1.6 * p.x - 1.2 * p.y, 1.2 * p.x + 1.6 * p.y); a *= 0.5; }
    return v;
}
float ss(float e0, float e1, float x) { float t = saturate((x - e0) / (e1 - e0)); return t * t * (3.0 - 2.0 * t); }
// Smoke: fbm warped by itself, drifting slowly to the left and up.
float smoke(float2 p, float t) {
    float2 q = float2(fbm(p + float2(0.0, t * 0.05)), fbm(p + float2(5.2, 1.3) - t * 0.03));
    return fbm(p + 3.0 * q + float2(t * 0.02, -t * 0.015));
}
float4 ps(float4 pos : SV_Position) : SV_Target {
    float2 uv = float2(pos.x / uRes.x, 1.0 - pos.y / uRes.y);
    float asp = uRes.x / uRes.y;
    float2 p = float2(uv.x * asp, uv.y);
    float t = uTime;
    // Parallax by depth, against the cursor: the far moon barely shifts, the smoke and the
    // mist in front of it shift more. uMouse is already eased on the CPU.
    float2 o = uMouse - 0.5;
    float reveal = ss(0.0, 1.0, uReveal);
    // The Blood Moon as in the game: huge, glowing from within and seen through a red haze, not
    // an astronomical body. A soft sphere (wrapped light, a glowing core, a dark bleeding limb),
    // blurred stains drifting slowly over it, a soft edge and a wide bloom.
    float2 mc = float2(asp * 0.5, 1.02) - o * float2(0.010, 0.005) + float2(0.0, (1.0 - reveal) * 0.22);
    float d = length(p - mc);
    float r = 0.27;
    float disc = ss(r + 0.0015, r - 0.0045, d);
    float2 L2 = normalize(float2(-0.5, 0.35));
    float3 moon = 0;
    if (d < r + 0.004) {
        float2 mp = (p - mc) / r;
        float3 n = float3(mp, sqrt(saturate(1.0 - dot(mp, mp))));
        // the stains sit on the sphere: squeezed towards the limb, turning very slowly
        float2 q = float2(atan2(n.x, n.z) + t * 0.006, asin(clamp(n.y, -0.999, 0.999)));
        float2 w = float2(fbm(q * 1.3 + t * 0.01), fbm(q * 1.3 + 4.7 - t * 0.008));
        float stain = ss(0.38, 0.72, fbm(q * 1.6 + w * 1.2 + 2.0));
        float veil = fbm(q * 4.0 + w * 2.0 - 1.3);
        // wrapped light from the upper left: no hard terminator, just a rounder, darker side
        float lit = saturate(dot(n, normalize(float3(L2 * 0.55, 0.85))) * 0.65 + 0.35);
        float core = pow(n.z, 1.6);
        float3 hot = float3(1.0, 0.40, 0.25), deep = float3(0.30, 0.02, 0.025);
        moon = lerp(hot, deep, saturate(stain * 0.75 + (1.0 - lit) * 0.7));
        moon *= (0.30 + 0.70 * pow(core, 0.7)) * lerp(0.78, 1.12, veil) * (0.5 + 0.7 * lit);
        moon += float3(0.35, 0.10, 0.07) * pow(core, 5.0) * lit * 0.45;  // the inner glow
        moon += float3(0.50, 0.04, 0.03) * pow(1.0 - n.z, 3.0) * lit * 0.45;  // blood bleeding at the lit limb
    }
    float dd = max(d - r, 0.0);
    float side = 0.8 + 0.25 * dot((p - mc) / max(d, 1e-4), L2);
    float corona = (exp(-dd * 3.2) * 0.6 + exp(-dd * 11.0) * 0.75 + exp(-dd * 40.0) * 0.35) * side;
    float3 col = lerp(float3(0.010, 0.006, 0.010), float3(0.045, 0.012, 0.016), ss(1.0, 0.0, uv.y));
    col += float3(0.50, 0.06, 0.05) * corona * 0.55;
    col = lerp(col, moon, disc);
    // Smoke in front of the moon, lit red where it crosses the corona.
    float2 ps1 = p - o * float2(0.05, 0.025);
    float s1 = smoke(ps1 * 1.6 + float2(t * 0.01, 0.0), t);
    float dense = ss(0.42, 0.85, s1);
    float3 smokeCol = float3(0.035, 0.022, 0.026) + float3(0.55, 0.08, 0.05) * corona * 0.6 + float3(0.25, 0.04, 0.03) * disc * 0.5;
    col = lerp(col, smokeCol, dense * 0.85);
    // Low mist and a darker band at the bottom, the nearest layer.
    float2 ps2 = p - o * float2(0.10, 0.04);
    float s2 = smoke(ps2 * float2(2.4, 5.0) - float2(t * 0.03, 0.0), t * 1.3);
    col = lerp(col, float3(0.09, 0.05, 0.055), ss(0.45, 0.9, s2) * (1.0 - ss(0.0, 0.42, uv.y)) * 0.7);
    // A lantern: warm light around the cursor, fading out when the cursor leaves.
    float2 m = float2(uLamp.x * asp, uLamp.y);
    float lantern = exp(-length(p - m) * 3.2) * uLampA;
    col += float3(0.20, 0.09, 0.03) * lantern * 0.35 * (0.9 + 0.1 * sin(t * 7.0 + sin(t * 3.1)));
    // Vignette and grain.
    float vig = ss(1.35, 0.2, length((uv - float2(0.5, 0.55)) * float2(asp * 0.7, 1.0)));
    float grain = hash(pos.xy + frac(t * 13.0) * 100.0) - 0.5;
    if (uArt > 0.5) {
        // Over the cover art: smoke, a blood tint and a heavy vignette.
        float a = dense * 0.55 + ss(0.45, 0.9, s2) * (1.0 - ss(0.0, 0.5, uv.y)) * 0.5;
        float shade = (1.0 - vig) * 0.9 + (1.0 - ss(0.0, 0.4, uv.y)) * 0.4 + 0.18;
        float alpha = lerp(1.0, min(saturate(a + shade), 0.94), reveal);
        float3 c = lerp(float3(0.0, 0.0, 0.0), float3(0.22, 0.03, 0.035), a / max(alpha, 1e-3)) * reveal;
        return float4(c * alpha + grain * 0.012, alpha);
    }
    col *= lerp(0.2, 1.0, vig) * reveal;
    col += grain * 0.012;
    return float4(col, 1.0);
}
)";

struct Constants {
    float res[2], time, art, mouse[2], lamp[2], lamp_alpha, reveal, pad[2];
};

std::string Key(const TextStyle& s) {
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "%d|%.2f|%d|%d|%.2f|%d|%d|%d", int(s.font), s.size, int(s.align), s.bold, s.spacing,
                  s.wrap, s.ellipsis, s.caps);
    return buffer;
}

}  // namespace

bool Gfx::Init(HWND window, int width, int height, float dpi_scale) {
    window_ = window, width_ = std::max(1, width), height_ = std::max(1, height), scale_ = dpi_scale;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 3, D3D11_SDK_VERSION,
                                   &device_, nullptr, &context_);
    if (FAILED(hr))  // no usable GPU driver: WARP still draws the launcher
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, 3, D3D11_SDK_VERSION, &device_,
                               nullptr, &context_);
    if (FAILED(hr)) return false;
    ComPtr<IDXGIDevice> dxgi_device;
    device_.As(&dxgi_device);
    ComPtr<IDXGIAdapter> adapter;
    dxgi_device->GetAdapter(&adapter);
    ComPtr<IDXGIFactory2> factory;
    adapter->GetParent(IID_PPV_ARGS(&factory));
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = UINT(width_), desc.Height = UINT(height_);
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    // The window is layered (it fades in and out as a whole): only the bit-block transfer model
    // draws into the window's own surface, which the layered window's opacity applies to.
    desc.BufferCount = 1;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    if (FAILED(factory->CreateSwapChainForHwnd(device_.Get(), window, &desc, nullptr, nullptr, &swapchain_))) {
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD, desc.BufferCount = 2;
        if (FAILED(factory->CreateSwapChainForHwnd(device_.Get(), window, &desc, nullptr, nullptr, &swapchain_))) return false;
    }
    factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);

    D2D1_FACTORY_OPTIONS options{};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options,
                                 reinterpret_cast<void**>(d2d_.GetAddressOf()))))
        return false;
    if (FAILED(d2d_->CreateDevice(dxgi_device.Get(), &d2d_device_)) ||
        FAILED(d2d_device_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc_)))
        return false;
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()));
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_));
    CreateSky();
    CreateTargets();
    return dwrite_ != nullptr;
}

void Gfx::CreateSky() {
    ComPtr<ID3DBlob> vs, ps, errors;
    const size_t length = std::strlen(SKY_HLSL);
    if (FAILED(D3DCompile(SKY_HLSL, length, "sky", nullptr, nullptr, "vs", "vs_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs, &errors)) ||
        FAILED(D3DCompile(SKY_HLSL, length, "sky", nullptr, nullptr, "ps", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps, &errors))) {
        if (errors) OutputDebugStringA(static_cast<const char*>(errors->GetBufferPointer()));
        if (errors && !Env("BB_LAUNCHER_SHOT").empty())
            WriteFile(PathFrom(Env("BB_LAUNCHER_SHOT") + ".shader.txt"), errors->GetBufferPointer(), errors->GetBufferSize());
        return;
    }
    device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vs_);
    device_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &ps_);
    D3D11_BUFFER_DESC desc{sizeof(Constants), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    device_->CreateBuffer(&desc, nullptr, &constants_);
    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device_->CreateBlendState(&blend, &blend_);
    sky_ok_ = vs_ && ps_ && constants_;
}

void Gfx::CreateTargets() {
    ComPtr<ID3D11Texture2D> back;
    swapchain_->GetBuffer(0, IID_PPV_ARGS(&back));
    device_->CreateRenderTargetView(back.Get(), nullptr, &rtv_);
    ComPtr<IDXGISurface> surface;
    back.As(&surface);
    const float dpi = 96.f * scale_;
    auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                         D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), dpi, dpi);
    dc_->CreateBitmapFromDxgiSurface(surface.Get(), &props, &target_);
    dc_->SetTarget(target_.Get());
    dc_->SetDpi(dpi, dpi);
    dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    if (!brush_) dc_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &brush_);
}

void Gfx::Resize(int width, int height, float dpi_scale) {
    width = std::max(1, width), height = std::max(1, height);
    if (width == width_ && height == height_ && dpi_scale == scale_) return;
    width_ = width, height_ = height, scale_ = dpi_scale;
    dc_->SetTarget(nullptr);
    target_.Reset(), rtv_.Reset();
    context_->ClearState();
    context_->Flush();
    swapchain_->ResizeBuffers(0, UINT(width), UINT(height), DXGI_FORMAT_UNKNOWN, 0);
    CreateTargets();
}

void Gfx::BeginFrame(float time, const Sky& sky, const std::function<void()>& under) {
    const float clear[4] = {0.027f * sky.reveal, 0.02f * sky.reveal, 0.04f * sky.reveal, 1};
    context_->ClearRenderTargetView(rtv_.Get(), clear);
    if (under) {
        dc_->BeginDraw();
        dc_->SetTransform(D2D1::Matrix3x2F::Identity());
        under();
        dc_->EndDraw();
    }
    if (sky_ok_) {
        Constants c{{float(width_), float(height_)}, time, under ? 1.f : 0.f, {sky.mouse_x, sky.mouse_y},
                    {sky.lamp_x, sky.lamp_y}, sky.lamp, sky.reveal, {0, 0}};
        context_->UpdateSubresource(constants_.Get(), 0, nullptr, &c, 0, 0);
        D3D11_VIEWPORT vp{0, 0, float(width_), float(height_), 0, 1};
        context_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
        // Premultiplied fog over the art; the plain sky is opaque.
        const float factor[4] = {0, 0, 0, 0};
        context_->OMSetBlendState(under ? blend_.Get() : nullptr, factor, 0xffffffff);
        context_->RSSetViewports(1, &vp);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->IASetInputLayout(nullptr);
        context_->VSSetShader(vs_.Get(), nullptr, 0);
        context_->PSSetShader(ps_.Get(), nullptr, 0);
        context_->PSSetConstantBuffers(0, 1, constants_.GetAddressOf());
        context_->Draw(3, 0);
        context_->OMSetRenderTargets(0, nullptr, nullptr);
    }
    dc_->BeginDraw();
    // Opening and closing: the interface fades in a layer and grows from the centre.
    const float w = Width(), h = Height();
    dc_->SetTransform(D2D1::Matrix3x2F::Scale(sky.ui_scale, sky.ui_scale, D2D1::Point2F(w / 2, h / 2)) *
                      D2D1::Matrix3x2F::Translation(0, sky.ui_shift));
    faded_ = sky.ui_opacity < 0.999f;
    if (faded_)
        dc_->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                              D2D1::IdentityMatrix(), std::max(0.f, sky.ui_opacity)),
                       nullptr);
}

void Gfx::EndFrame() {
    while (clips_ > 0) PopClip();
    if (faded_) dc_->PopLayer(), faded_ = false;
    dc_->EndDraw();
    if (!shot.empty()) {
        ComPtr<ID3D11Texture2D> back, staging;
        swapchain_->GetBuffer(0, IID_PPV_ARGS(&back));
        D3D11_TEXTURE2D_DESC desc;
        back->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING, desc.BindFlags = 0, desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ, desc.MiscFlags = 0;
        device_->CreateTexture2D(&desc, nullptr, &staging);
        context_->CopyResource(staging.Get(), back.Get());
        D3D11_MAPPED_SUBRESOURCE map;
        if (SUCCEEDED(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) {
            ComPtr<IWICBitmap> bitmap;
            wic_->CreateBitmapFromMemory(desc.Width, desc.Height, GUID_WICPixelFormat32bppBGRA, map.RowPitch,
                                         map.RowPitch * desc.Height, static_cast<BYTE*>(map.pData), &bitmap);
            context_->Unmap(staging.Get(), 0);
            ComPtr<IWICStream> stream;
            ComPtr<IWICBitmapEncoder> encoder;
            ComPtr<IWICBitmapFrameEncode> frame;
            wic_->CreateStream(&stream);
            stream->InitializeFromFilename(shot.c_str(), GENERIC_WRITE);
            wic_->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
            encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
            encoder->CreateNewFrame(&frame, nullptr);
            frame->Initialize(nullptr);
            WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
            frame->SetPixelFormat(&format);
            frame->WriteSource(bitmap.Get(), nullptr);
            frame->Commit();
            encoder->Commit();
        }
        shot.clear();
    }
    swapchain_->Present(1, 0);
    if (layouts_.size() > 3000) layouts_.clear();
}

ID2D1SolidColorBrush* Gfx::Brush(Color c) {
    brush_->SetColor(c.D2());
    brush_->SetOpacity(1);
    return brush_.Get();
}

void Gfx::Fill(const Rect& r, Color c) { dc_->FillRectangle(r.D2(), Brush(c)); }
void Gfx::FillRound(const Rect& r, float radius, Color c) {
    dc_->FillRoundedRectangle(D2D1::RoundedRect(r.D2(), radius, radius), Brush(c));
}
void Gfx::Stroke(const Rect& r, Color c, float width) {
    const float h = width / 2;
    dc_->DrawRectangle(D2D1::RectF(r.x + h, r.y + h, r.Right() - h, r.Bottom() - h), Brush(c), width);
}
void Gfx::Line(float x0, float y0, float x1, float y1, Color c, float width) {
    dc_->DrawLine({x0, y0}, {x1, y1}, Brush(c), width);
}
void Gfx::Ring(float cx, float cy, float radius, Color c, float width) {
    dc_->DrawEllipse(D2D1::Ellipse({cx, cy}, radius, radius), Brush(c), width);
}

void Gfx::Dot(float cx, float cy, float radius, Color fill, Color ring) {
    if (fill.a > 0) Ellipse(cx, cy, radius, radius, fill);
    if (ring.a > 0) Ring(cx, cy, radius, ring, 1);
}

void Gfx::Crescent(float cx, float cy, float radius, float ox, float oy, Color c) {
    ComPtr<ID2D1EllipseGeometry> a, b;
    d2d_->CreateEllipseGeometry(D2D1::Ellipse({cx, cy}, radius, radius), &a);
    d2d_->CreateEllipseGeometry(D2D1::Ellipse({cx + ox, cy + oy}, radius * 0.92f, radius * 0.92f), &b);
    ComPtr<ID2D1PathGeometry> path;
    d2d_->CreatePathGeometry(&path);
    ComPtr<ID2D1GeometrySink> sink;
    path->Open(&sink);
    a->CombineWithGeometry(b.Get(), D2D1_COMBINE_MODE_EXCLUDE, nullptr, sink.Get());
    sink->Close();
    dc_->FillGeometry(path.Get(), Brush(c));
}

void Gfx::Polygon(const std::vector<D2D1_POINT_2F>& points, Color fill, Color border, float width) {
    if (points.size() < 3) return;
    ComPtr<ID2D1PathGeometry> path;
    d2d_->CreatePathGeometry(&path);
    ComPtr<ID2D1GeometrySink> sink;
    path->Open(&sink);
    sink->BeginFigure(points[0], D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLines(points.data() + 1, UINT32(points.size() - 1));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    if (fill.a > 0) dc_->FillGeometry(path.Get(), Brush(fill));
    if (border.a > 0) dc_->DrawGeometry(path.Get(), Brush(border), width);
}

void Gfx::Ellipse(float cx, float cy, float rx, float ry, Color c) {
    dc_->FillEllipse(D2D1::Ellipse({cx, cy}, rx, ry), Brush(c));
}

void Gfx::Diamond(float cx, float cy, float half, Color fill, Color border, float border_width) {
    ComPtr<ID2D1PathGeometry> path;
    d2d_->CreatePathGeometry(&path);
    ComPtr<ID2D1GeometrySink> sink;
    path->Open(&sink);
    sink->BeginFigure({cx, cy - half}, D2D1_FIGURE_BEGIN_FILLED);
    D2D1_POINT_2F points[3] = {{cx + half, cy}, {cx, cy + half}, {cx - half, cy}};
    sink->AddLines(points, 3);
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    if (fill.a > 0) dc_->FillGeometry(path.Get(), Brush(fill));
    if (border.a > 0 && border_width > 0) dc_->DrawGeometry(path.Get(), Brush(border), border_width);
}

void Gfx::VGradient(const Rect& r, Color top, Color bottom) {
    D2D1_GRADIENT_STOP stops[2] = {{0, top.D2()}, {1, bottom.D2()}};
    ComPtr<ID2D1GradientStopCollection> collection;
    dc_->CreateGradientStopCollection(stops, 2, &collection);
    ComPtr<ID2D1LinearGradientBrush> brush;
    dc_->CreateLinearGradientBrush({{r.x, r.y}, {r.x, r.Bottom()}}, collection.Get(), &brush);
    dc_->FillRectangle(r.D2(), brush.Get());
}

void Gfx::HGradient(const Rect& r, Color left, Color right) {
    D2D1_GRADIENT_STOP stops[2] = {{0, left.D2()}, {1, right.D2()}};
    ComPtr<ID2D1GradientStopCollection> collection;
    dc_->CreateGradientStopCollection(stops, 2, &collection);
    ComPtr<ID2D1LinearGradientBrush> brush;
    dc_->CreateLinearGradientBrush({{r.x, r.y}, {r.Right(), r.y}}, collection.Get(), &brush);
    dc_->FillRectangle(r.D2(), brush.Get());
}

void Gfx::Glow(const Rect& r, Color c) {
    D2D1_GRADIENT_STOP stops[2] = {{0, c.D2()}, {1, c.Alpha(0).D2()}};
    ComPtr<ID2D1GradientStopCollection> collection;
    dc_->CreateGradientStopCollection(stops, 2, &collection);
    ComPtr<ID2D1RadialGradientBrush> brush;
    dc_->CreateRadialGradientBrush({{r.x + r.w / 2, r.y + r.h / 2}, {0, 0}, r.w / 2, r.h / 2}, collection.Get(), &brush);
    dc_->FillEllipse(D2D1::Ellipse({r.x + r.w / 2, r.y + r.h / 2}, r.w / 2, r.h / 2), brush.Get());
}

void Gfx::Panel(const Rect& r, Color fill, Color line, Color corner) {
    Fill(r, fill);
    Stroke(r, line);
    const float s = 14, o = 4;
    Line(r.x - o, r.y - o, r.x - o + s, r.y - o, corner);
    Line(r.x - o, r.y - o, r.x - o, r.y - o + s, corner);
    Line(r.Right() + o, r.Bottom() + o, r.Right() + o - s, r.Bottom() + o, corner);
    Line(r.Right() + o, r.Bottom() + o, r.Right() + o, r.Bottom() + o - s, corner);
}

IDWriteTextFormat* Gfx::Format(const TextStyle& s) {
    const std::string key = Key(s);
    auto it = formats_.find(key);
    if (it != formats_.end()) return it->second.Get();
    const wchar_t* family = s.font == Font::Mono      ? L"Consolas"
                            : s.font == Font::Symbol  ? L"Segoe UI Symbol"
                                                      : L"Palatino Linotype";
    ComPtr<IDWriteTextFormat> format;
    dwrite_->CreateTextFormat(family, nullptr, s.bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                              DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, s.size, L"", &format);
    format->SetTextAlignment(s.align == Align::Center  ? DWRITE_TEXT_ALIGNMENT_CENTER
                             : s.align == Align::Right ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                                       : DWRITE_TEXT_ALIGNMENT_LEADING);
    format->SetWordWrapping(s.wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    if (s.ellipsis) {
        DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> sign;
        dwrite_->CreateEllipsisTrimmingSign(format.Get(), &sign);
        format->SetTrimming(&trimming, sign.Get());
    }
    formats_[key] = format;
    return format.Get();
}

IDWriteTextLayout* Gfx::Layout(const std::string& text, const TextStyle& style, float max_width, float max_height) {
    char size[64];
    std::snprintf(size, sizeof(size), "|%.1f|%.1f|", max_width, max_height);
    const std::string key = Key(style) + size + text;
    auto it = layouts_.find(key);
    if (it != layouts_.end()) return it->second.Get();
    const std::wstring w = Wide(text);
    ComPtr<IDWriteTextLayout> layout;
    dwrite_->CreateTextLayout(w.c_str(), UINT32(w.size()), Format(style), std::max(1.f, max_width),
                              std::max(1.f, max_height), &layout);
    if (style.spacing != 0) {
        ComPtr<IDWriteTextLayout1> spaced;
        if (SUCCEEDED(layout.As(&spaced))) spaced->SetCharacterSpacing(0, style.spacing, 0, {0, UINT32(w.size())});
    }
    if (style.caps) {
        ComPtr<IDWriteTypography> typography;
        if (SUCCEEDED(dwrite_->CreateTypography(&typography))) {
            typography->AddFontFeature({DWRITE_FONT_FEATURE_TAG_SMALL_CAPITALS, 1});
            layout->SetTypography(typography.Get(), {0, UINT32(w.size())});
        }
    }
    layouts_[key] = layout;
    return layout.Get();
}

D2D1_SIZE_F Gfx::Measure(const std::string& text, const TextStyle& style, float max_width) {
    DWRITE_TEXT_METRICS m{};
    Layout(text, style, max_width)->GetMetrics(&m);
    // Trailing spacing of a letter-spaced text is not part of its look.
    return {m.widthIncludingTrailingWhitespace - (style.spacing > 0 ? style.spacing : 0), m.height};
}

void Gfx::Text(const std::string& text, const Rect& box, const TextStyle& style, bool center_vertically) {
    if (text.empty()) return;
    IDWriteTextLayout* layout = Layout(text, style, box.w, std::max(1.f, box.h));
    float y = box.y;
    if (center_vertically) {
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        y = box.y + (box.h - m.height) / 2;
    }
    float x = box.x;
    // Letter spacing adds space after the last letter too: centred text moves back by half of it.
    if (style.spacing > 0 && style.align == Align::Center) x += style.spacing / 2;
    dc_->DrawTextLayout({x, y}, layout, Brush(style.color), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
}

ID2D1Bitmap* Gfx::Image(const fs::path& path) {
    auto it = images_.find(path.native());
    if (it != images_.end()) return it->second.Get();
    ComPtr<ID2D1Bitmap> bitmap;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (wic_ && SUCCEEDED(wic_->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(wic_->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                        WICBitmapPaletteTypeMedianCut)))
        dc_->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &bitmap);
    images_[path.native()] = bitmap;
    return bitmap.Get();
}

void Gfx::PushOpacity(float opacity) {
    dc_->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                          D2D1::IdentityMatrix(), opacity, nullptr, D2D1_LAYER_OPTIONS1_NONE),
                   nullptr);
}

void Gfx::PopOpacity() { dc_->PopLayer(); }

void Gfx::PushClip(const Rect& r) {
    dc_->PushAxisAlignedClip(r.D2(), D2D1_ANTIALIAS_MODE_ALIASED);
    ++clips_;
}

void Gfx::PopClip() {
    if (clips_ <= 0) return;
    dc_->PopAxisAlignedClip();
    --clips_;
}
