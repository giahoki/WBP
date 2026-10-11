// SPDX-License-Identifier: GPL-2.0-or-later
// The icons are 32x32 BC7 textures in PS4 block order (Morton order of the 4x4 blocks). Each is
// drawn with Direct2D at 128x128, scaled down, encoded as BC7 mode 6 and swizzled back; the TPF
// keeps its layout, only the DCX is compressed again (libdeflate, smaller than the original).
#include "key_icons.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include <libdeflate.h>

using Microsoft::WRL::ComPtr;

namespace prep {
namespace {

constexpr int VERSION = 7;  // bump when the output changes: invalidates the cache

// Which action's first binding each icon shows (several: the first one that is bound).
const std::vector<std::pair<const char*, std::vector<const char*>>> ICONS = {
    {"KG_Cancel", {"interact"}},  // the cross button (confirm in this version)
    {"KG_OK", {"dodge_run", "back"}},  // the circle button: dodge and back are one button, prompts mostly mean dodge
    {"KG_R_L", {"use_item"}}, {"KG_R_U", {"blood_vial"}},
    {"KG_L1", {"transform"}}, {"KG_L2", {"gun"}}, {"KG_L3", {"jump"}},
    {"KG_R1", {"attack"}}, {"KG_R2", {"strong_attack"}}, {"KG_R3", {"lock_on"}},
    {"KG_Start", {"menu"}}, {"KG_TP_L", {"gestures"}}, {"KG_TP_R", {"key_items"}},
    {"KG_L_U", {"dpad_up"}}, {"KG_L_D", {"dpad_down"}},
};
const std::vector<std::pair<const char*, std::vector<const char*>>> DPAD = {
    {"KG_L_LR", {"dpad_left", "dpad_right"}},
    {"KG_L_UD", {"dpad_up", "dpad_down"}},
    {"KG_L_UDLR", {"dpad_up", "dpad_down", "dpad_left", "dpad_right"}},
};
// World prompts ("X Inspect") in the HUD atlas MENU_Common_00091 (1024x512 BC7): 26x26 icons,
// each redrawn in a block-aligned rectangle that holds nothing else.
constexpr const char* HUD_ATLAS = "MENU_Common_00091";
struct Hud {
    int x0, y0, x1, y1, ix, iy, size;
    std::vector<const char*> actions;
};
const Hud HUD[] = {{660, 120, 688, 152, 661, 123, 26, {"dodge_run", "back"}},
                   {692, 120, 724, 152, 694, 123, 26, {"interact"}},
                   {724, 120, 756, 152, 726, 123, 26, {"jump"}}};

// ---- archives ------------------------------------------------------------------------------

uint32_t Be32(const Bytes& d, size_t at) {
    if (at + 4 > d.size()) throw Failure("archive too short");
    return uint32_t(d[at]) << 24 | uint32_t(d[at + 1]) << 16 | uint32_t(d[at + 2]) << 8 | d[at + 3];
}
void PutBe32(Bytes& d, size_t at, uint32_t v) {
    d[at] = uint8_t(v >> 24), d[at + 1] = uint8_t(v >> 16), d[at + 2] = uint8_t(v >> 8), d[at + 3] = uint8_t(v);
}

std::pair<Bytes, Bytes> DcxUnpack(const Bytes& data) {
    if (data.size() < 0x2C || std::memcmp(data.data(), "DCX\0", 4) || std::memcmp(data.data() + 0x28, "DFLT", 4))
        throw Failure("expected a DFLT DCX archive");
    static const uint8_t dca[4] = {'D', 'C', 'A', 0};
    auto it = std::search(data.begin(), data.end(), dca, dca + 4);
    if (it == data.end()) throw Failure("DCX without DCA");
    const size_t i = size_t(it - data.begin());
    const size_t header_size = i + Be32(data, i + 4);
    const size_t raw_size = Be32(data, 0x1C);
    if (header_size > data.size()) throw Failure("bad DCX header");
    Bytes header(data.begin(), data.begin() + ptrdiff_t(header_size)), payload(raw_size);
    libdeflate_decompressor* d = libdeflate_alloc_decompressor();
    size_t actual = 0;
    auto result = libdeflate_zlib_decompress(d, data.data() + header_size, data.size() - header_size, payload.data(),
                                             payload.size(), &actual);
    libdeflate_free_decompressor(d);
    if (result != LIBDEFLATE_SUCCESS || actual != raw_size) throw Failure("cannot decompress the DCX");
    return {header, payload};
}

// The DCX with `payload` compressed, or nothing when it does not fit in `limit` bytes: the game
// refuses an archive whose compressed data is larger than the original's.
std::optional<Bytes> DcxPack(Bytes header, const Bytes& payload, size_t limit) {
    libdeflate_compressor* c = libdeflate_alloc_compressor(12);
    Bytes packed(libdeflate_zlib_compress_bound(c, payload.size()));
    size_t n = libdeflate_zlib_compress(c, payload.data(), payload.size(), packed.data(), packed.size());
    libdeflate_free_compressor(c);
    if (!n || n > limit) return std::nullopt;
    packed.resize(n);
    PutBe32(header, 0x1C, uint32_t(payload.size()));
    PutBe32(header, 0x20, uint32_t(n));
    header.insert(header.end(), packed.begin(), packed.end());
    return header;
}

struct Texture {
    uint32_t offset;
    int32_t size;
    int width, height, dxgi;
};

std::map<std::string, Texture> TpfTextures(const Bytes& data) {
    if (data.size() < 16 || std::memcmp(data.data(), "TPF\0", 4) || data[12] != 4) throw Failure("expected a PS4 TPF");
    const uint32_t count = Le32(data, 8);
    const uint8_t encoding = data[14];
    std::map<std::string, Texture> textures;
    size_t off = 16;
    for (uint32_t i = 0; i < count; ++i) {
        Texture t{Le32(data, off), int32_t(Le32(data, off + 4)), Le16(data, off + 12), Le16(data, off + 14), 0};
        const uint32_t name_off = Le32(data, off + 24);
        const int32_t has_float = int32_t(Le32(data, off + 28));
        t.dxgi = int32_t(Le32(data, off + 32));
        off += 36;
        if (has_float == 1) off += 8 + Le32(data, off + 4);
        std::string name;
        if (encoding == 1) {
            for (size_t p = name_off; Le16(data, p); p += 2) name += char(Le16(data, p) & 0x7f);
        } else {
            for (size_t p = name_off; p < data.size() && data[p]; ++p) name += char(data[p]);
        }
        textures[name] = t;
    }
    return textures;
}

// ---- BC7 -----------------------------------------------------------------------------------

using Rgba = std::array<int, 4>;
const int WEIGHTS4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

// One 4x4 block as BC7 mode 6: one subset, RGBA 7.7.7.7 + p-bit endpoints, 4-bit indices.
// Endpoints span the block along its principal axis.
std::array<uint8_t, 16> Bc7Mode6(const std::array<Rgba, 16>& px) {
    double mean[4] = {};
    for (auto& p : px)
        for (int c = 0; c < 4; ++c) mean[c] += p[c] / 16.0;
    double cov[4][4] = {};
    for (int a = 0; a < 4; ++a)
        for (int b = 0; b < 4; ++b)
            for (auto& p : px) cov[a][b] += (p[a] - mean[a]) * (p[b] - mean[b]);
    double axis[4] = {1, 1, 1, 1};
    for (int it = 0; it < 12; ++it) {
        double v[4] = {};
        for (int a = 0; a < 4; ++a)
            for (int b = 0; b < 4; ++b) v[a] += cov[a][b] * axis[b];
        double length = 0;
        for (double x : v) length = std::max(length, std::fabs(x));
        if (length < 1e-9) break;
        for (int a = 0; a < 4; ++a) axis[a] = v[a] / length;
    }
    double lo = 1e30, hi = -1e30, norm = 0;
    for (auto& p : px) {
        double proj = 0;
        for (int c = 0; c < 4; ++c) proj += (p[c] - mean[c]) * axis[c];
        lo = std::min(lo, proj), hi = std::max(hi, proj);
    }
    for (double a : axis) norm += a * a;
    if (norm == 0) norm = 1;
    double e0[4], e1[4];
    for (int c = 0; c < 4; ++c) {
        e0[c] = std::clamp(mean[c] + axis[c] * lo / norm, 0.0, 255.0);
        e1[c] = std::clamp(mean[c] + axis[c] * hi / norm, 0.0, 255.0);
    }
    struct Best {
        long error = -1;
        int q0[4], q1[4], p0, p1;
        int idx[16];
    } best;
    for (int p0 = 0; p0 < 2; ++p0)
        for (int p1 = 0; p1 < 2; ++p1) {
            int q0[4], q1[4], a[4], b[4];
            for (int c = 0; c < 4; ++c) {
                q0[c] = std::clamp(int(std::nearbyint((e0[c] - p0) / 2)), 0, 127);
                q1[c] = std::clamp(int(std::nearbyint((e1[c] - p1) / 2)), 0, 127);
                a[c] = q0[c] * 2 + p0, b[c] = q1[c] * 2 + p1;
            }
            int palette[16][4];
            for (int w = 0; w < 16; ++w)
                for (int c = 0; c < 4; ++c) palette[w][c] = ((64 - WEIGHTS4[w]) * a[c] + WEIGHTS4[w] * b[c] + 32) >> 6;
            long error = 0;
            int idx[16];
            for (int k = 0; k < 16; ++k) {
                long best_err = -1;
                for (int w = 0; w < 16; ++w) {
                    long e = 0;
                    for (int c = 0; c < 4; ++c) e += long(px[k][c] - palette[w][c]) * (px[k][c] - palette[w][c]);
                    if (best_err < 0 || e < best_err) best_err = e, idx[k] = w;
                }
                error += best_err;
            }
            if (best.error < 0 || error < best.error) {
                best.error = error, best.p0 = p0, best.p1 = p1;
                std::memcpy(best.q0, q0, sizeof q0), std::memcpy(best.q1, q1, sizeof q1), std::memcpy(best.idx, idx, sizeof idx);
            }
        }
    if (best.idx[0] >= 8) {  // the anchor index has no top bit: swap the endpoints
        std::swap(best.q0, best.q1), std::swap(best.p0, best.p1);
        for (int& i : best.idx) i = 15 - i;
    }
    uint8_t out[16] = {};
    int pos = 0;
    auto bits = [&](uint32_t value, int width) {
        for (int i = 0; i < width; ++i, ++pos)
            if (value >> i & 1) out[pos >> 3] |= uint8_t(1 << (pos & 7));
    };
    bits(1 << 6, 7);  // mode 6
    for (int c = 0; c < 4; ++c) bits(uint32_t(best.q0[c]), 7), bits(uint32_t(best.q1[c]), 7);
    bits(uint32_t(best.p0), 1), bits(uint32_t(best.p1), 1);
    for (int k = 0; k < 16; ++k) bits(uint32_t(best.idx[k]), k == 0 ? 3 : 4);
    std::array<uint8_t, 16> block;
    std::memcpy(block.data(), out, 16);
    return block;
}

int Morton(int x, int y) {
    int z = 0;
    for (int i = 0; i < 8; ++i) z |= ((x >> i) & 1) << (2 * i) | ((y >> i) & 1) << (2 * i + 1);
    return z;
}

// Byte offset of a 4x4 block: 8x8-block tiles in rows, Morton order inside a tile.
size_t BlockOffset(int bx, int by, int width_blocks) {
    const int tile = (by / 8) * (width_blocks / 8) + bx / 8;
    return size_t(tile * 64 + Morton(bx % 8, by % 8)) * 16;
}

struct Image {
    int w = 0, h = 0;
    std::vector<Rgba> px;  // straight alpha
    Rgba& at(int x, int y) { return px[size_t(y * w + x)]; }
};

void EncodeRegion(Bytes& data, size_t base, int width, Image& image, int x0, int y0) {
    for (int by = 0; by < image.h / 4; ++by)
        for (int bx = 0; bx < image.w / 4; ++bx) {
            std::array<Rgba, 16> block;
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x) block[size_t(y * 4 + x)] = image.at(bx * 4 + x, by * 4 + y);
            auto encoded = Bc7Mode6(block);
            const size_t o = base + BlockOffset(x0 / 4 + bx, y0 / 4 + by, width / 4);
            if (o + 16 > data.size()) throw Failure("texture outside the archive");
            std::memcpy(data.data() + o, encoded.data(), 16);
        }
}

// ---- drawing -------------------------------------------------------------------------------

constexpr int SCALE = 4;  // drawn at 128x128, scaled down for smooth edges
const D2D1_COLOR_F GOLD{220 / 255.f, 188 / 255.f, 124 / 255.f, 1}, GOLD_DIM{150 / 255.f, 126 / 255.f, 84 / 255.f, 1},
    CAP{34 / 255.f, 31 / 255.f, 29 / 255.f, 245 / 255.f}, CAP_RIM{96 / 255.f, 86 / 255.f, 72 / 255.f, 1},
    LIT{190 / 255.f, 44 / 255.f, 44 / 255.f, 1}, BADGE{120 / 255.f, 20 / 255.f, 24 / 255.f, 1},
    BADGE_TEXT{250 / 255.f, 236 / 255.f, 210 / 255.f, 1}, CAP_LINE{70 / 255.f, 64 / 255.f, 56 / 255.f, 1};
const wchar_t* FONT = L"Segoe UI";
const wchar_t* SYMBOL_FONT = L"Segoe UI Symbol";
const std::map<std::string, std::string> SYMBOLS = {
    {"Space", "␣"}, {"Return", "↵"}, {"Keypad Enter", "↵"}, {"Backspace", "⌫"},
    {"Up", "↑"},    {"Down", "↓"},   {"Left", "←"},         {"Right", "→"},
    {"Left Shift", "⇧"}, {"Right Shift", "⇧"}};
const std::map<std::string, std::string> WORDS = {
    {"Escape", "Esc"},  {"Tab", "Tab"},   {"Left Ctrl", "Ctrl"}, {"Right Ctrl", "Ctrl"}, {"Left Alt", "Alt"},
    {"Right Alt", "Alt"}, {"CapsLock", "Caps"}, {"Delete", "Del"}, {"Insert", "Ins"}, {"PageUp", "PgUp"},
    {"PageDown", "PgDn"}, {"Home", "Home"}, {"End", "End"}};

struct Painter {
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmap> bitmap;
    ComPtr<ID2D1RenderTarget> target;

    Painter() {
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf())) ||
            FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown**>(dwrite.GetAddressOf()))) ||
            FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))))
            throw Failure("Direct2D is unavailable");
    }

    void Begin(int size) {
        bitmap.Reset(), target.Reset();
        wic->CreateBitmap(UINT(size), UINT(size), GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap);
        auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                                  D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                                                  96, 96);
        if (!bitmap || FAILED(d2d->CreateWicBitmapRenderTarget(bitmap.Get(), props, &target))) throw Failure("cannot draw icons");
        target->BeginDraw();
        target->Clear(D2D1::ColorF(0, 0, 0, 0));
    }

    ComPtr<ID2D1SolidColorBrush> Brush(D2D1_COLOR_F c) {
        ComPtr<ID2D1SolidColorBrush> b;
        target->CreateSolidColorBrush(c, &b);
        return b;
    }

    // Pillow's rounded_rectangle: the outline lies inside the box.
    void RoundRect(float x0, float y0, float x1, float y1, float r, const D2D1_COLOR_F* fill,
                   const D2D1_COLOR_F* outline = nullptr, float width = 0) {
        auto rect = D2D1::RoundedRect(D2D1::RectF(x0, y0, x1 + 1, y1 + 1), r, r);
        if (fill) target->FillRoundedRectangle(rect, Brush(*fill).Get());
        if (outline) {
            auto inner = D2D1::RoundedRect(D2D1::RectF(x0 + width / 2, y0 + width / 2, x1 + 1 - width / 2, y1 + 1 - width / 2),
                                           std::max(0.f, r - width / 2), std::max(0.f, r - width / 2));
            target->DrawRoundedRectangle(inner, Brush(*outline).Get(), width);
        }
    }

    void Line(float x0, float y0, float x1, float y1, D2D1_COLOR_F c, float width) {
        target->DrawLine({x0, y0}, {x1, y1}, Brush(c).Get(), width);
    }

    void TextCenter(float x0, float y0, float x1, float y1, const std::string& text, const wchar_t* font, float size,
                    D2D1_COLOR_F color) {
        const std::wstring w = Wide(text);
        ComPtr<IDWriteTextLayout> layout;
        DWRITE_OVERHANG_METRICS oh{};
        for (;;) {
            ComPtr<IDWriteTextFormat> format;
            dwrite->CreateTextFormat(font, nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                     DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &format);
            layout.Reset();
            dwrite->CreateTextLayout(w.c_str(), UINT32(w.size()), format.Get(), 1000, 1000, &layout);
            layout->GetOverhangMetrics(&oh);
            const float ink_w = (1000 + oh.right) - (-oh.left);
            if (ink_w <= (x1 - x0) || size <= 20) break;
            size -= 4;
        }
        const float l = -oh.left, t = -oh.top, r = 1000 + oh.right, b = 1000 + oh.bottom;
        const float x = (x0 + x1 - (r - l)) / 2 - l, y = (y0 + y1 - (b - t)) / 2 - t;
        target->DrawTextLayout({x, y}, layout.Get(), Brush(color).Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
    }

    // Scaled down to `pixels` with straight alpha.
    Image End(int pixels) {
        target->EndDraw();
        ComPtr<IWICBitmapScaler> scaler;
        wic->CreateBitmapScaler(&scaler);
        scaler->Initialize(bitmap.Get(), UINT(pixels), UINT(pixels), WICBitmapInterpolationModeFant);
        std::vector<uint8_t> bgra(size_t(pixels * pixels * 4));
        scaler->CopyPixels(nullptr, UINT(pixels * 4), UINT(bgra.size()), bgra.data());
        Image image{pixels, pixels, std::vector<Rgba>(size_t(pixels * pixels))};
        for (int i = 0; i < pixels * pixels; ++i) {
            const uint8_t* p = &bgra[size_t(i) * 4];
            const int a = p[3];
            auto un = [&](int v) { return a ? std::min(255, (v * 255 + a / 2) / a) : 0; };
            image.px[size_t(i)] = {un(p[2]), un(p[1]), un(p[0]), a};
        }
        return image;
    }

    void Keycap() {
        RoundRect(6, 10, 122, 118, 22, &CAP, &CAP_RIM, 5);
        Line(26, 16, 102, 16, CAP_LINE, 3);
    }

    // A mouse seen from above; lit: left, right, middle, wheel or empty.
    void Mouse(const std::string& lit, float x0 = 30, float y0 = 6, float x1 = 98, float y1 = 122) {
        const float mid = std::floor((x0 + x1) / 2), split = y0 + (y1 - y0) * 0.42f;
        RoundRect(x0, y0, x1, y1, 30, &CAP, &GOLD_DIM, 5);
        if (lit == "left") RoundRect(x0 + 5, y0 + 5, mid, split, 22, &LIT);
        if (lit == "right") RoundRect(mid, y0 + 5, x1 - 5, split, 22, &LIT);
        Line(mid, y0 + 4, mid, split, GOLD_DIM, 4);
        Line(x0 + 4, split, x1 - 4, split, GOLD_DIM, 4);
        const D2D1_COLOR_F& wheel = (lit == "middle" || lit == "wheel") ? LIT : GOLD;
        RoundRect(mid - 7, y0 + 18, mid + 7, y0 + 44, 7, &wheel);
    }

    Image Binding(const std::string& binding, int pixels = 32) {
        Begin(32 * SCALE);
        auto parts = Split(binding, '+');
        std::vector<std::string> mods;
        for (size_t i = 0; i + 1 < parts.size(); ++i)
            if (parts[i] == "Shift" || parts[i] == "Ctrl" || parts[i] == "Alt") mods.push_back(parts[i]);
        const std::string name = parts.back();
        if (StartsWith(name, "Mouse") || StartsWith(name, "Wheel")) {
            static const std::map<std::string, std::string> lit = {
                {"Mouse1", "left"}, {"Mouse2", "right"}, {"Mouse3", "middle"}, {"WheelUp", "wheel"}, {"WheelDown", "wheel"}};
            auto it = lit.find(name);
            const std::string which = it == lit.end() ? "" : it->second;
            if (mods.empty()) Mouse(which);
            else Mouse(which, 40, 10, 104, 120);
            if (name == "Mouse4" || name == "Mouse5") TextCenter(36, 52, 92, 112, name.substr(5), FONT, 52, GOLD);
            if (StartsWith(name, "Wheel"))
                TextCenter(36, 60, 92, 118, name == "WheelUp" ? "↑" : "↓", SYMBOL_FONT, 52, GOLD);
        } else {
            Keycap();
            auto sym = SYMBOLS.find(name);
            auto word = WORDS.find(name);
            std::string label = sym != SYMBOLS.end() ? sym->second : word != WORDS.end() ? word->second
                                : name.size() <= 4 ? name : name.substr(0, 4);
            const wchar_t* font = sym != SYMBOLS.end() ? SYMBOL_FONT : FONT;
            const size_t chars = Wide(label).size();
            const float size = chars == 1 ? 84.f : chars == 2 ? 60.f : 44.f;
            if (mods.empty()) TextCenter(12, 18, 116, 112, label, font, size, GOLD);
            else TextCenter(14, 40, 114, 112, label, font, size, GOLD);
        }
        if (!mods.empty()) {
            const bool shift = mods[0] == "Shift";
            const std::string badge = shift ? "⇧" : mods[0];
            RoundRect(0, 0, shift ? 58.f : 74.f, 46, 12, &BADGE, &GOLD, 3);
            TextCenter(4, 2, shift ? 54.f : 70.f, 44, badge, shift ? SYMBOL_FONT : FONT, shift ? 40.f : 26.f, BADGE_TEXT);
        }
        return End(pixels);
    }

    // Several small key labels (WASD, arrow clusters) on one keycap.
    Image TextIcon(const std::vector<std::string>& lines) {
        Begin(32 * SCALE);
        Keycap();
        if (lines.size() == 1) {
            TextCenter(12, 26, 116, 104, lines[0], SYMBOL_FONT, 60, GOLD);
        } else {
            TextCenter(12, 16, 116, 62, lines[0], SYMBOL_FONT, 44, GOLD);
            TextCenter(12, 60, 116, 110, lines[1], SYMBOL_FONT, 44, GOLD);
        }
        return End(32);
    }

    Image MouseIcon() {
        Begin(32 * SCALE);
        Mouse("");
        return End(32);
    }
};

std::string Short(const std::string& binding) {
    const std::string name = Split(binding, '+').back();
    if (auto it = SYMBOLS.find(name); it != SYMBOLS.end()) return it->second;
    if (auto it = WORDS.find(name); it != WORDS.end()) return it->second;
    return name.substr(0, 3);
}

std::map<std::string, Image> IconsFor(Painter& painter, const Bindings& bindings, bool native) {
    std::map<std::string, std::string> first;
    for (auto& [action, values] : bindings)
        if (!values.empty()) first[action] = values[0];
    std::map<std::string, Image> icons;
    for (auto& [texture, actions] : ICONS)
        for (const char* a : actions)
            if (first.count(a)) {
                icons[texture] = painter.Binding(first[a]);
                break;
            }
    for (auto& [texture, actions] : DPAD) {
        std::vector<std::string> labels;
        for (const char* a : actions)
            if (first.count(a)) labels.push_back(Short(first[a]));
        if (labels.size() != actions.size()) continue;
        if (std::string(texture) == "KG_L_UDLR") icons[texture] = painter.TextIcon({labels[0], labels[2] + labels[1] + labels[3]});
        else {
            std::string all;
            for (auto& l : labels) all += l;
            icons[texture] = painter.TextIcon({all});
        }
    }
    std::vector<std::string> move;
    for (const char* a : {"move_forward", "move_left", "move_back", "move_right"})
        if (first.count(a)) move.push_back(Short(first[a]));
    if (move.size() == 4) icons["KG_LS"] = painter.TextIcon({move[0], move[1] + move[2] + move[3]});
    icons["KG_RS"] = painter.MouseIcon();  // the camera is the mouse
    if (DodgeIconSplit(bindings, native)) {
        icons["KG_OK"] = painter.Binding(first["back"]);
        icons[DODGE_ICON] = painter.Binding(first["dodge_run"]);
    }
    return icons;
}

int PatchHud(Painter& painter, Bytes& payload, const std::map<std::string, Texture>& textures, const Bindings& bindings) {
    auto it = textures.find(HUD_ATLAS);
    if (it == textures.end()) return 0;
    const Texture& t = it->second;
    if (t.width != 1024 || t.height != 512 || t.dxgi != 98 || t.size < t.width * t.height) return 0;
    int drawn = 0;
    for (const Hud& h : HUD) {
        std::string binding;
        for (const char* a : h.actions) {
            auto b = bindings.find(a);
            if (b != bindings.end() && !b->second.empty()) {
                binding = b->second[0];
                break;
            }
        }
        if (binding.empty()) continue;
        Image region{h.x1 - h.x0, h.y1 - h.y0, std::vector<Rgba>(size_t((h.x1 - h.x0) * (h.y1 - h.y0)), Rgba{0, 0, 0, 0})};
        Image icon = painter.Binding(binding, h.size);
        for (int y = 0; y < icon.h; ++y)
            for (int x = 0; x < icon.w; ++x) region.at(h.ix - h.x0 + x, h.iy - h.y0 + y) = icon.at(x, y);
        EncodeRegion(payload, t.offset, t.width, region, h.x0, h.y0);
        ++drawn;
    }
    return drawn;
}

}  // namespace

bool DodgeIconSplit(const Bindings& bindings, bool native) {
    auto dodge = bindings.find("dodge_run"), back = bindings.find("back");
    return native && dodge != bindings.end() && back != bindings.end() && !dodge->second.empty() &&
           !back->second.empty() && dodge->second[0] != back->second[0];
}

std::optional<fs::path> BuildKeyIcons(const fs::path& game, const fs::path& out, const Bindings& bindings,
                                      bool native) {
    const fs::path archive = fs::path(L"dvdroot_ps4") / L"menu" / L"common.tpf.dcx";
    const fs::path source = game / archive;
    std::error_code ec;
    if (!fs::is_regular_file(source, ec)) return std::nullopt;
    std::string key = "v" + std::to_string(VERSION) + (native ? "n" : "e");
    for (auto& [action, values] : bindings) {
        key += "|" + action + "=";
        for (auto& v : values) key += v + ",";
    }
    key += "|" + std::to_string((unsigned long long)fs::file_size(source, ec)) + "|" +
           std::to_string((long long)fs::last_write_time(source, ec).time_since_epoch().count());
    const fs::path folder = out / L"key_icons" / Wide(Sha256Hex(key.data(), key.size()).substr(0, 16));
    const fs::path target = folder / archive;
    if (fs::is_regular_file(target, ec)) return folder;

    const Bytes original = ReadFile(source);
    auto [header, payload] = DcxUnpack(original);
    const size_t limit = original.size() - header.size();
    const size_t original_size = payload.size();
    const auto textures = TpfTextures(payload);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int replaced = 0;
    {
        Painter painter;
        for (auto& [name, image] : IconsFor(painter, bindings, native)) {
            auto it = textures.find(name);
            if (it == textures.end()) continue;
            const Texture& t = it->second;
            if (t.width != 32 || t.height != 32 || t.dxgi != 98 || t.size < 1024 || image.w != 32) continue;
            if (size_t(t.offset) + 1024 > payload.size()) continue;
            std::fill(payload.begin() + t.offset, payload.begin() + t.offset + 1024, 0);
            EncodeRegion(payload, t.offset, 32, image, 0, 0);
            ++replaced;
        }
        replaced += PatchHud(painter, payload, textures, bindings);
    }
    if (SUCCEEDED(com)) CoUninitialize();
    // Older caches are dropped: only the current bindings are kept.
    for (auto& old : fs::directory_iterator(out / L"key_icons", ec))
        if (old.path() != folder) fs::remove_all(old.path(), ec);
    if (payload.size() != original_size) throw Failure("the archive layout changed; nothing written");
    auto packed = DcxPack(header, payload, limit);
    if (!packed) {
        SayError("Key icons: the archive does not compress below the original size; the game shows the gamepad buttons");
        return std::nullopt;
    }
    WriteFile(target, *packed);
    SayError("Key icons: " + std::to_string(replaced) + " button prompts drawn for the keyboard and mouse");
    return folder;
}

}  // namespace prep
