// SPDX-License-Identifier: GPL-2.0-or-later
// The launcher's renderer: Direct3D 11 draws the animated background (a pixel shader of a
// moonlit gothic city with fog), Direct2D and DirectWrite draw the interface over it on the
// same back buffer. Coordinates are in DIPs (96 per inch); the DPI scale is applied here.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "../util.h"

using Microsoft::WRL::ComPtr;

struct Color {
    float r, g, b, a = 1;
    Color Alpha(float alpha) const { return {r, g, b, a * alpha}; }
    D2D1_COLOR_F D2() const { return {r, g, b, a}; }
};
constexpr Color Rgb(uint32_t rgb, float a = 1) {
    return {((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a};
}

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    float Right() const { return x + w; }
    float Bottom() const { return y + h; }
    bool Contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    Rect Inset(float d) const { return {x + d, y + d, w - 2 * d, h - 2 * d}; }
    D2D1_RECT_F D2() const { return {x, y, x + w, y + h}; }
};

enum class Font { Body, Display, Mono, Symbol };
enum class Align { Left, Center, Right };

struct TextStyle {
    Font font = Font::Body;
    float size = 15;
    Color color{0.92f, 0.87f, 0.81f};
    Align align = Align::Left;
    bool bold = false;
    float spacing = 0;   // extra letter spacing in DIPs
    bool wrap = true;
    bool ellipsis = false;
    bool caps = false;   // small capitals (Palatino's smcp), the game's menu lettering
};

class Gfx {
public:
    bool Init(HWND window, int width, int height, float dpi_scale);
    void Resize(int width, int height, float dpi_scale);
    float Scale() const { return scale_; }
    float Width() const { return width_ / scale_; }   // DIPs
    float Height() const { return height_ / scale_; }

    // Background: the sky shader. With `under` (the cover art, drawn with Direct2D first) the
    // shader only adds fog, a crimson tint and a vignette over it.
    void BeginFrame(float time, float mouse_x, float mouse_y, const std::function<void()>& under = {});
    void EndFrame();
    fs::path shot;  // development: the next frame is also saved as this PNG

    ID2D1DeviceContext* Dc() const { return dc_.Get(); }
    ID2D1SolidColorBrush* Brush(Color c);

    void Fill(const Rect& r, Color c);
    void FillRound(const Rect& r, float radius, Color c);
    void Stroke(const Rect& r, Color c, float width = 1);
    void Line(float x0, float y0, float x1, float y1, Color c, float width = 1);
    void Diamond(float cx, float cy, float half, Color fill, Color border, float border_width = 1);
    void Ellipse(float cx, float cy, float rx, float ry, Color c);
    void Ring(float cx, float cy, float radius, Color c, float width = 1);
    void Dot(float cx, float cy, float radius, Color fill, Color ring = {0, 0, 0, 0});
    // A crescent: the disc at (cx, cy) minus the same disc moved by (ox, oy).
    void Crescent(float cx, float cy, float radius, float ox, float oy, Color c);
    // A closed polygon.
    void Polygon(const std::vector<D2D1_POINT_2F>& points, Color fill, Color border = {0, 0, 0, 0}, float width = 1);
    void VGradient(const Rect& r, Color top, Color bottom);
    void HGradient(const Rect& r, Color left, Color right);
    // A soft glow (radial gradient) centred on the rectangle.
    void Glow(const Rect& r, Color c);
    // Corner ornaments of a framed panel.
    void Panel(const Rect& r, Color fill, Color line, Color corner);

    // Text: layouts are cached by their content.
    IDWriteTextLayout* Layout(const std::string& text, const TextStyle& style, float max_width, float max_height = 10000);
    D2D1_SIZE_F Measure(const std::string& text, const TextStyle& style, float max_width = 10000);
    void Text(const std::string& text, const Rect& box, const TextStyle& style, bool center_vertically = false);

    // Images (PNG/JPEG through WIC), cached by path; nullptr when they cannot be read.
    ID2D1Bitmap* Image(const fs::path& path);

    // Everything drawn until PopOpacity is blended with this opacity (tab transitions).
    void PushOpacity(float opacity);
    void PopOpacity();
    void PushClip(const Rect& r);
    void PopClip();

private:
    void CreateTargets();
    void CreateSky();
    IDWriteTextFormat* Format(const TextStyle& style);

    HWND window_ = nullptr;
    int width_ = 1, height_ = 1;
    float scale_ = 1;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain1> swapchain_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11Buffer> constants_;
    ComPtr<ID3D11BlendState> blend_;
    bool sky_ok_ = false;

    ComPtr<ID2D1Factory1> d2d_;
    ComPtr<ID2D1Device> d2d_device_;
    ComPtr<ID2D1DeviceContext> dc_;
    ComPtr<ID2D1Bitmap1> target_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<IWICImagingFactory> wic_;
    std::map<std::string, ComPtr<IDWriteTextFormat>> formats_;
    std::map<std::string, ComPtr<IDWriteTextLayout>> layouts_;
    std::map<std::wstring, ComPtr<ID2D1Bitmap>> images_;
    int clips_ = 0;
};
