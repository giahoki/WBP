// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "../i18n.h"

using namespace theme;

namespace {

TextStyle Style(float size, Color color, Align align = Align::Left, Font font = Font::Body) {
    TextStyle s;
    s.size = size, s.color = color, s.align = align, s.font = font;
    return s;
}

std::string Utf8Of(char32_t c) {
    std::string out;
    if (c < 0x80) out += char(c);
    else if (c < 0x800) out += char(0xC0 | (c >> 6)), out += char(0x80 | (c & 63));
    else if (c < 0x10000) out += char(0xE0 | (c >> 12)), out += char(0x80 | ((c >> 6) & 63)), out += char(0x80 | (c & 63));
    else out += char(0xF0 | (c >> 18)), out += char(0x80 | ((c >> 12) & 63)), out += char(0x80 | ((c >> 6) & 63)), out += char(0x80 | (c & 63));
    return out;
}

// Byte offsets of UTF-8 character starts.
size_t PrevChar(const std::string& s, size_t at) {
    if (!at) return 0;
    --at;
    while (at && (uint8_t(s[at]) & 0xC0) == 0x80) --at;
    return at;
}
size_t NextChar(const std::string& s, size_t at) {
    if (at >= s.size()) return s.size();
    ++at;
    while (at < s.size() && (uint8_t(s[at]) & 0xC0) == 0x80) ++at;
    return at;
}
size_t Utf16Length(const std::string& s) { return Wide(s).size(); }

}  // namespace

uint64_t Ui::Id(std::string_view text, uint64_t seed) {
    uint64_t h = 1469598103934665603ull ^ seed;
    for (char c : text) h = (h ^ uint8_t(c)) * 1099511628211ull;
    return h | 1;
}

void Ui::BeginFrame() {
    cursor = Cursor::Arrow;
    popup_rect_last_ = popup_ ? popup_rect_ : Rect{};
    clips_.clear();
}

bool Ui::Hovered(const Rect& r) const {
    if (!r.Contains(in.x, in.y)) return false;
    if (!clips_.empty() && !clips_.back().Contains(in.x, in.y)) return false;
    if (popup_ && popup_rect_last_.Contains(in.x, in.y)) return false;
    return true;
}

float Ui::Anim(uint64_t id, float target, float speed) {
    auto [it, inserted] = anim_.try_emplace(id, target);
    float& v = it->second;
    v += (target - v) * std::min(1.f, in.dt * speed);
    if (std::fabs(target - v) < 0.001f) v = target;
    return v;
}

void Ui::PushClip(const Rect& r) {
    Rect c = r;
    if (!clips_.empty()) {
        const Rect& p = clips_.back();
        float x0 = std::max(c.x, p.x), y0 = std::max(c.y, p.y), x1 = std::min(c.Right(), p.Right()),
              y1 = std::min(c.Bottom(), p.Bottom());
        c = {x0, y0, std::max(0.f, x1 - x0), std::max(0.f, y1 - y0)};
    }
    clips_.push_back(c);
    g.PushClip(c);
}

void Ui::PopClip() {
    if (clips_.empty()) return;
    clips_.pop_back();
    g.PopClip();
}

float Ui::ButtonWidth(const std::string& label, bool small) {
    TextStyle s = Style(small ? 14.f : 15.f, TEXT);
    s.spacing = 1.2f, s.wrap = false, s.caps = true;
    return g.Measure(label, s).width + (small ? 32 : 44);
}

bool Ui::Button(std::string_view id_text, const std::string& label, const Rect& r, ButtonStyle style) {
    const uint64_t id = Id(id_text);
    const bool hover = !style.disabled && Hovered(r);
    if (hover) cursor = Cursor::Hand;
    const float h = Anim(id, hover ? 1.f : 0.f);
    const float alpha = style.disabled ? 0.35f : 1.f;
    Rect box = r;
    if (hover && in.down[0]) box = box.Inset(1);
    // a dark plate between two hairlines, lit red from below when hovered
    if (!style.ghost) g.Fill(box, Rgb(style.danger ? 0x2a0a0c : 0x0c0809, 0.85f * alpha));
    if (h > 0.01f) {
        g.VGradient(Rect{box.x, box.y + box.h * 0.35f, box.w, box.h * 0.65f}, BLOOD.Alpha(0), BLOOD.Alpha(0.55f * h));
        g.Glow(Rect{box.x - 14, box.y - 8, box.w + 28, box.h + 16}, BLOOD_GLOW.Alpha(0.25f * h));
    }
    const Color line = GOLD.Alpha((0.45f + 0.55f * h) * alpha);
    g.HGradient(Rect{box.x, box.y, box.w / 2, 1}, line.Alpha(0.2f), line);
    g.HGradient(Rect{box.x + box.w / 2, box.y, box.w / 2, 1}, line, line.Alpha(0.2f));
    g.HGradient(Rect{box.x, box.Bottom() - 1, box.w / 2, 1}, line.Alpha(0.2f), line);
    g.HGradient(Rect{box.x + box.w / 2, box.Bottom() - 1, box.w / 2, 1}, line, line.Alpha(0.2f));
    g.Line(box.x, box.y + 4, box.x, box.Bottom() - 4, line.Alpha(0.5f));
    g.Line(box.Right(), box.y + 4, box.Right(), box.Bottom() - 4, line.Alpha(0.5f));
    TextStyle s = Style(style.small ? 14.f : 15.f, (h > 0.5f ? Rgb(0xfff4e2) : GOLD_HI).Alpha(alpha), Align::Center);
    s.spacing = 1.2f, s.wrap = false, s.caps = true;
    g.Text(label, box, s, true);
    return hover && in.pressed[0];
}

bool Ui::Check(std::string_view id_text, const std::string& label, bool& value, float x, float y, float width,
               float* height) {
    const uint64_t id = Id(id_text);
    TextStyle s = Style(14.5f, TEXT);
    const float text_w = width - 34;
    const auto size = g.Measure(label, s, text_w);
    const float h = std::max(24.f, size.height + 4);
    if (height) *height = h;
    const Rect hit{x, y, std::min(width, 34 + size.width + 6), h};
    const bool hover = Hovered(hit);
    if (hover) cursor = Cursor::Hand;
    bool changed = false;
    if (hover && in.pressed[0]) value = !value, changed = true;
    const float on = Anim(id, value ? 1.f : 0.f, 16);
    const float cx = x + 11, cy = y + h / 2;
    g.Dot(cx, cy, 8.5f, FIELD, hover || value ? GOLD : LINE_STRONG);
    if (on > 0.01f) {
        g.Glow(Rect{cx - 14 * on, cy - 14 * on, 28 * on, 28 * on}, BLOOD_GLOW.Alpha(0.8f * on));
        g.Dot(cx, cy, 5.f * on, BLOOD_HI);
    }
    g.Text(label, Rect{x + 34, y + (h - size.height) / 2, text_w, size.height + 2}, s);
    return changed;
}

bool Ui::Combo(std::string_view id_text, const Rect& r, const Options& options, std::string& value) {
    const uint64_t id = Id(id_text);
    bool changed = false;
    if (popup_changed_ == id) {
        value = popup_choice_;
        popup_changed_ = 0;
        changed = true;
    }
    const bool hover = Hovered(r);
    if (hover) cursor = Cursor::Hand;
    if (hover && in.pressed[0]) {
        if (popup_ == id) {
            popup_ = 0;
        } else {
            popup_ = id;
            popup_state_ = {r, options, nullptr, false, 0};
            popup_choice_ = value;
        }
    }
    const bool open = popup_ == id;
    g.Fill(r, FIELD);
    g.Stroke(r, open ? GOLD : hover ? LINE_STRONG : LINE);
    std::string label = value;
    for (auto& [v, l] : options)
        if (v == value) label = l;
    TextStyle s = Style(14.5f, TEXT);
    s.wrap = false, s.ellipsis = true;
    g.Text(label, Rect{r.x + 12, r.y, r.w - 44, r.h}, s, true);
    // the chevron
    const float cx = r.Right() - 17, cy = r.y + r.h / 2;
    g.Line(cx - 5, cy - 2, cx, cy + 3, GOLD, 1.5f);
    g.Line(cx, cy + 3, cx + 5, cy - 2, GOLD, 1.5f);
    return changed;
}

bool Ui::Slider(std::string_view id_text, const Rect& r, float& value, float lo, float hi, float step) {
    const uint64_t id = Id(id_text);
    const Rect track{r.x, r.y, r.w - 60, r.h};
    const bool hover = Hovered(track);
    if (hover) cursor = Cursor::Hand;
    if (hover && in.pressed[0]) active_ = id;
    bool changed = false;
    if (active_ == id) {
        if (!in.down[0]) {
            active_ = 0;
        } else {
            float t = std::clamp((in.x - track.x) / track.w, 0.f, 1.f);
            float v = lo + t * (hi - lo);
            v = std::round(v / step) * step;
            v = std::clamp(v, lo, hi);
            if (std::fabs(v - value) > step * 0.01f) value = v, changed = true;
        }
    }
    if (hover && in.wheel != 0 && active_ != id) {
        float v = std::clamp(value + (in.wheel > 0 ? step : -step), lo, hi);
        if (v != value) value = v, changed = true;
        in.wheel = 0;
    }
    // The game's gauge: a row of notches, lit up to the value.
    const float t = std::clamp((value - lo) / (hi - lo), 0.f, 1.f);
    const float cy = r.y + r.h / 2;
    const int notches = 24;
    const float gap = track.w / float(notches);
    const float hl = Anim(id, hover || active_ == id ? 1.f : 0.f);
    for (int i = 0; i < notches; ++i) {
        const float f = (float(i) + 0.5f) / float(notches);
        const bool lit = f <= t;
        const float nx = track.x + gap * float(i) + gap * 0.2f, nh = lit ? 12.f : 8.f;
        Color c = lit ? Color{BLOOD_HI.r + (GOLD.r - BLOOD_HI.r) * f, BLOOD_HI.g + (GOLD.g - BLOOD_HI.g) * f,
                              BLOOD_HI.b + (GOLD.b - BLOOD_HI.b) * f, 0.75f + 0.25f * hl}
                      : GOLD.Alpha(0.18f);
        g.Fill(Rect{nx, cy - nh / 2, gap * 0.6f, nh}, c);
    }
    const float px = track.x + t * track.w;
    g.Glow(Rect{px - 16, cy - 16, 32, 32}, GOLD.Alpha(0.2f + 0.3f * hl));
    g.Fill(Rect{px - 2, cy - 10, 4, 20}, Rgb(0xfff4e2));
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f", value);
    TextStyle s = Style(15, GOLD_HI, Align::Right);
    s.wrap = false;
    g.Text(text, Rect{r.Right() - 56, r.y, 56, r.h}, s, true);
    return changed;
}

bool Ui::Entry(std::string_view id_text, const Rect& r, std::string& text, bool secret) {
    const uint64_t id = Id(id_text);
    const bool hover = Hovered(r);
    if (hover) cursor = Cursor::Text;
    bool committed = false;
    if (focus_ == id) {
        auto commit = [&] {
            if (edit_text_ != text) text = edit_text_, committed = true;
            focus_ = 0;
        };
        if (in.pressed[0] && !hover) {
            commit();
        } else {
            for (char32_t c : in.chars)
                if (c >= 32 && c != 127) {
                    std::string u = Utf8Of(c);
                    edit_text_.insert(caret_, u);
                    caret_ += u.size();
                }
            for (const KeyEvent& k : in.keys) {
                const bool ctrl = k.mods & GLFW_MOD_CONTROL;
                if (k.key == GLFW_KEY_BACKSPACE && caret_) {
                    size_t p = PrevChar(edit_text_, caret_);
                    edit_text_.erase(p, caret_ - p), caret_ = p;
                } else if (k.key == GLFW_KEY_DELETE && caret_ < edit_text_.size()) {
                    edit_text_.erase(caret_, NextChar(edit_text_, caret_) - caret_);
                } else if (k.key == GLFW_KEY_LEFT) {
                    caret_ = PrevChar(edit_text_, caret_);
                } else if (k.key == GLFW_KEY_RIGHT) {
                    caret_ = NextChar(edit_text_, caret_);
                } else if (k.key == GLFW_KEY_HOME) {
                    caret_ = 0;
                } else if (k.key == GLFW_KEY_END) {
                    caret_ = edit_text_.size();
                } else if (ctrl && k.key == GLFW_KEY_V && clipboard_get) {
                    std::string paste = clipboard_get();
                    paste.erase(std::remove_if(paste.begin(), paste.end(), [](char c) { return c == '\r' || c == '\n'; }),
                                paste.end());
                    edit_text_.insert(caret_, paste);
                    caret_ += paste.size();
                } else if (ctrl && k.key == GLFW_KEY_C && clipboard_set && !secret) {
                    clipboard_set(edit_text_);
                } else if (ctrl && k.key == GLFW_KEY_A) {
                    caret_ = edit_text_.size();
                } else if (k.key == GLFW_KEY_ENTER || k.key == GLFW_KEY_KP_ENTER || k.key == GLFW_KEY_TAB) {
                    commit();
                    break;
                } else if (k.key == GLFW_KEY_ESCAPE) {
                    focus_ = 0;
                    break;
                }
            }
        }
    } else if (hover && in.pressed[0]) {
        focus_ = id;
        edit_text_ = text;
        caret_ = text.size();
    }
    const bool focused = focus_ == id;
    const std::string& shown_source = focused ? edit_text_ : text;
    std::string shown = shown_source;
    if (secret) {
        shown.clear();
        for (size_t i = 0; i < Utf16Length(shown_source); ++i) shown += "•";
    }
    g.Fill(r, FIELD);
    g.Stroke(r, focused ? GOLD : hover ? LINE_STRONG : LINE);
    if (focused) g.Glow(Rect{r.x - 8, r.y - 8, r.w + 16, r.h + 16}, GOLD.Alpha(0.06f));
    TextStyle s = Style(14.5f, TEXT);
    s.wrap = false;
    const Rect inner{r.x + 12, r.y, r.w - 24, r.h};
    // Scroll the text so the caret stays visible.
    float caret_x = 0;
    if (focused) {
        const std::string before = secret ? std::string() : edit_text_.substr(0, caret_);
        size_t count = secret ? Utf16Length(edit_text_.substr(0, caret_)) : 0;
        std::string measure = before;
        if (secret)
            for (size_t i = 0; i < count; ++i) measure += "•";
        caret_x = measure.empty() ? 0 : g.Measure(measure, s).width;
    }
    const float shift = std::max(0.f, caret_x - inner.w + 4);
    PushClip(inner);
    g.Text(shown, Rect{inner.x - shift, inner.y, 4000, inner.h}, s, true);
    if (focused && std::fmod(in.time, 1.0) < 0.6) {
        const float cx = inner.x - shift + caret_x;
        g.Line(cx, r.y + 8, cx, r.Bottom() - 8, GOLD_HI, 1.2f);
    }
    PopClip();
    return committed;
}

std::string Ui::SdlKeyName(int key) {
    if (key >= GLFW_KEY_A && key <= GLFW_KEY_Z) return std::string(1, char('A' + key - GLFW_KEY_A));
    if (key >= GLFW_KEY_0 && key <= GLFW_KEY_9) return std::string(1, char('0' + key - GLFW_KEY_0));
    if (key >= GLFW_KEY_KP_0 && key <= GLFW_KEY_KP_9) return "Keypad " + std::to_string(key - GLFW_KEY_KP_0);
    if (key >= GLFW_KEY_F1 && key <= GLFW_KEY_F24) return "F" + std::to_string(key - GLFW_KEY_F1 + 1);
    static const std::map<int, const char*> names = {
        {GLFW_KEY_SPACE, "Space"}, {GLFW_KEY_ENTER, "Return"}, {GLFW_KEY_KP_ENTER, "Keypad Enter"},
        {GLFW_KEY_ESCAPE, "Escape"}, {GLFW_KEY_BACKSPACE, "Backspace"}, {GLFW_KEY_TAB, "Tab"},
        {GLFW_KEY_LEFT_SHIFT, "Left Shift"}, {GLFW_KEY_RIGHT_SHIFT, "Right Shift"}, {GLFW_KEY_LEFT_CONTROL, "Left Ctrl"},
        {GLFW_KEY_RIGHT_CONTROL, "Right Ctrl"}, {GLFW_KEY_LEFT_ALT, "Left Alt"}, {GLFW_KEY_RIGHT_ALT, "Right Alt"},
        {GLFW_KEY_UP, "Up"}, {GLFW_KEY_DOWN, "Down"}, {GLFW_KEY_LEFT, "Left"}, {GLFW_KEY_RIGHT, "Right"},
        {GLFW_KEY_CAPS_LOCK, "CapsLock"}, {GLFW_KEY_GRAVE_ACCENT, "`"}, {GLFW_KEY_MINUS, "-"}, {GLFW_KEY_EQUAL, "="},
        {GLFW_KEY_LEFT_BRACKET, "["}, {GLFW_KEY_RIGHT_BRACKET, "]"}, {GLFW_KEY_BACKSLASH, "\\"},
        {GLFW_KEY_SEMICOLON, ";"}, {GLFW_KEY_APOSTROPHE, "'"}, {GLFW_KEY_COMMA, ","}, {GLFW_KEY_PERIOD, "."},
        {GLFW_KEY_SLASH, "/"}, {GLFW_KEY_INSERT, "Insert"}, {GLFW_KEY_DELETE, "Delete"}, {GLFW_KEY_HOME, "Home"},
        {GLFW_KEY_END, "End"}, {GLFW_KEY_PAGE_UP, "PageUp"}, {GLFW_KEY_PAGE_DOWN, "PageDown"},
        {GLFW_KEY_KP_ADD, "Keypad +"}, {GLFW_KEY_KP_SUBTRACT, "Keypad -"}, {GLFW_KEY_KP_MULTIPLY, "Keypad *"},
        {GLFW_KEY_KP_DIVIDE, "Keypad /"}, {GLFW_KEY_KP_DECIMAL, "Keypad ."}};
    auto it = names.find(key);
    return it == names.end() ? std::string() : it->second;
}

std::string Ui::PrettyKey(const std::string& binding) {
    const bool ru = i18n::Language() == "ru";
    static const std::map<std::string, std::string> en = {
        {"Mouse1", "LMB"}, {"Mouse2", "RMB"}, {"Mouse3", "MMB"}, {"Mouse4", "Mouse 4"}, {"Mouse5", "Mouse 5"},
        {"WheelUp", "Wheel ↑"}, {"WheelDown", "Wheel ↓"}, {"Return", "Enter"}, {"Escape", "Esc"},
        {"Up", "↑"}, {"Down", "↓"}, {"Left", "←"}, {"Right", "→"}, {"Left Shift", "Shift"},
        {"Left Ctrl", "Ctrl"}};
    static const std::map<std::string, std::string> rus = {
        {"Mouse1", "ЛКМ"}, {"Mouse2", "ПКМ"}, {"Mouse3", "Колесо"}, {"Mouse4", "Мышь 4"}, {"Mouse5", "Мышь 5"},
        {"WheelUp", "Колесо ↑"}, {"WheelDown", "Колесо ↓"}, {"Space", "Пробел"}, {"Return", "Enter"},
        {"Escape", "Esc"}, {"Up", "↑"}, {"Down", "↓"}, {"Left", "←"}, {"Right", "→"},
        {"Left Shift", "Shift"}, {"Right Shift", "Правый Shift"}, {"Left Ctrl", "Ctrl"}, {"Right Ctrl", "Правый Ctrl"},
        {"Right Alt", "Правый Alt"}};
    const auto& names = ru ? rus : en;
    std::string out;
    for (const std::string& part : Split(binding, '+')) {
        auto it = names.find(part);
        out += (out.empty() ? "" : " + ") + (it == names.end() ? part : it->second);
    }
    return out;
}

bool Ui::BindSlot(std::string_view id_text, const Rect& r, std::string& binding, const std::vector<std::string>& reserved,
                  std::string* refused) {
    const uint64_t id = Id(id_text);
    const bool hover = Hovered(r);
    if (hover) cursor = Cursor::Hand;
    bool changed = false;
    auto take = [&](const std::string& name) {
        std::string bare = name;
        for (const char* m : {"Shift+", "Ctrl+", "Alt+"})
            if (StartsWith(bare, m)) bare = bare.substr(std::strlen(m));
        if (std::find(reserved.begin(), reserved.end(), bare) != reserved.end()) {
            if (refused) *refused = name;
            return;
        }
        binding = name;
        changed = true;
        capture_ = 0;
    };
    if (capture_ == id) {
        auto mods = [&](int m) -> std::string {
            if (m & GLFW_MOD_SHIFT) return "Shift+";
            if (m & GLFW_MOD_CONTROL) return "Ctrl+";
            if (m & GLFW_MOD_ALT) return "Alt+";
            return "";
        };
        for (const KeyEvent& k : in.keys) {
            const std::string name = SdlKeyName(k.key);
            if (name.empty()) continue;
            const bool is_mod = name.find("Shift") != std::string::npos || name.find("Ctrl") != std::string::npos ||
                                name.find("Alt") != std::string::npos;
            take(is_mod ? name : mods(k.mods) + name);
            break;
        }
        if (capture_ == id) {
            if (!capture_armed_ && !in.down[0]) capture_armed_ = true;
            const int key_mods = (GetKeyState(VK_SHIFT) < 0 ? GLFW_MOD_SHIFT : 0) |
                                 (GetKeyState(VK_CONTROL) < 0 ? GLFW_MOD_CONTROL : 0) |
                                 (GetKeyState(VK_MENU) < 0 ? GLFW_MOD_ALT : 0);
            static const char* buttons[5] = {"Mouse1", "Mouse2", "Mouse3", "Mouse4", "Mouse5"};
            for (int b = 0; b < 5 && capture_ == id; ++b) {
                if (!in.pressed[b]) continue;
                if (b == 0 && (!hover || !capture_armed_)) {
                    if (!hover) capture_ = 0;  // a click elsewhere cancels
                    continue;
                }
                take(mods(key_mods) + buttons[b]);
            }
            if (capture_ == id && in.wheel != 0) {
                take(mods(key_mods) + (in.wheel > 0 ? "WheelUp" : "WheelDown"));
                in.wheel = 0;
            }
        }
    } else if (hover && in.pressed[0]) {
        capture_ = id;
        capture_armed_ = false;
    } else if (hover && in.pressed[1]) {
        binding.clear();
        changed = true;
    }
    const bool capturing = capture_ == id;
    const float pulse = capturing ? 0.5f + 0.5f * std::sin(float(in.time) * 6.f) : 0.f;
    g.Fill(r, capturing ? BLOOD.Alpha(0.45f) : FIELD);
    if (capturing) g.Glow(Rect{r.x - 12, r.y - 10, r.w + 24, r.h + 20}, BLOOD_GLOW.Alpha(0.35f + 0.4f * pulse));
    g.Stroke(r, capturing ? BLOOD_HI : hover ? GOLD : LINE);
    TextStyle s = Style(13.5f, capturing ? Color{1, 1, 1} : binding.empty() ? DIM : GOLD_HI, Align::Center);
    s.wrap = false, s.ellipsis = true;
    const std::string label = capturing ? T("Press…", "Нажмите…") : binding.empty() ? "—" : PrettyKey(binding);
    g.Text(label, r.Inset(4), s, true);
    return changed;
}

void Ui::Progress(const Rect& r, float fraction) {
    g.Fill(r, GOLD.Alpha(0.18f));
    Rect fill{r.x, r.y, r.w * std::clamp(fraction, 0.f, 1.f), r.h};
    g.HGradient(fill, BLOOD_HI, GOLD);
}

float Ui::Paragraph(const std::string& text, float x, float y, float w, const TextStyle& style) {
    if (text.empty()) return 0;
    const auto size = g.Measure(text, style, w);
    g.Text(text, Rect{x, y, w, size.height + 2}, style);
    return size.height;
}

void Ui::EndFrame() {
    popup_rect_ = {};
    if (!popup_) return;
    Popup& p = popup_state_;
    const float row = 32, pad = 4;
    const float wanted = p.options.size() * row + pad * 2;
    const float below = g.Height() - p.anchor.Bottom() - 8, above = p.anchor.y - 8;
    float h = std::min(wanted, std::max(below, above));
    const bool down = below >= std::min(wanted, 200.f) || below >= above;
    h = std::min(wanted, down ? below : above);
    Rect box{p.anchor.x, down ? p.anchor.Bottom() + 2 : p.anchor.y - 2 - h, std::max(p.anchor.w, 220.f), h};
    if (box.Right() > g.Width() - 8) box.x = g.Width() - 8 - box.w;
    popup_rect_ = box;
    const bool inside = box.Contains(in.x, in.y);
    if (inside && in.wheel != 0) {
        p.scroll = std::clamp(p.scroll - in.wheel * row * 2, 0.f, std::max(0.f, wanted - h));
        in.wheel = 0;
    }
    g.Fill(Rect{box.x + 4, box.y + 6, box.w, box.h}, Color{0, 0, 0, 0.45f});
    g.Fill(box, PANEL_SOLID.Alpha(0.98f));
    g.Stroke(box, GOLD.Alpha(0.6f));
    g.PushClip(box.Inset(1));
    TextStyle s = Style(14.5f, TEXT);
    s.wrap = false, s.ellipsis = true;
    for (size_t i = 0; i < p.options.size(); ++i) {
        Rect item{box.x + 1, box.y + pad + i * row - p.scroll, box.w - 2, row};
        if (item.Bottom() < box.y || item.y > box.Bottom()) continue;
        const bool selected = p.options[i].first == popup_choice_;
        const bool hover = inside && item.Contains(in.x, in.y);
        if (hover) cursor = Cursor::Hand;
        if (selected) g.HGradient(item, BLOOD.Alpha(0.55f), BLOOD.Alpha(0.1f));
        if (hover) g.Fill(item, GOLD.Alpha(0.10f));
        s.color = selected ? GOLD_HI : hover ? TEXT : MUTED.Alpha(1);
        g.Text(p.options[i].second, Rect{item.x + 24, item.y, item.w - 32, row}, s, true);
        if (hover && in.pressed[0]) {
            popup_choice_ = p.options[i].first;
            popup_changed_ = popup_;
            popup_ = 0;
            in.pressed[0] = false;
        }
    }
    g.PopClip();
    if (popup_ && in.pressed[0] && !inside && !p.anchor.Contains(in.x, in.y)) popup_ = 0;
    if (popup_) {
        for (const KeyEvent& k : in.keys)
            if (k.key == GLFW_KEY_ESCAPE) popup_ = 0;
    }
}

// ---- the game's option rows ---------------------------------------------------------------------

bool Ui::Cycle(std::string_view id_text, const Rect& r, const Options& options, std::string& value, bool row_hovered) {
    const uint64_t id = Id(id_text);
    bool changed = false;
    if (popup_changed_ == id) {
        value = popup_choice_;
        popup_changed_ = 0;
        changed = true;
    }
    size_t index = 0;
    for (size_t i = 0; i < options.size(); ++i)
        if (options[i].first == value) index = i;
    auto step = [&](int d) {
        if (options.empty()) return;
        index = (index + options.size() + size_t(d + int(options.size()))) % options.size();
        value = options[index].first;
        changed = true;
    };
    const float arrow = 26;
    Rect left{r.x, r.y, arrow, r.h}, right{r.Right() - arrow, r.y, arrow, r.h};
    Rect middle{r.x + arrow, r.y, r.w - 2 * arrow, r.h};
    const bool hl = Hovered(left), hr = Hovered(right), hm = Hovered(middle);
    if (hl || hr || hm) cursor = Cursor::Hand;
    if (hl && in.pressed[0]) step(-1);
    else if (hr && in.pressed[0]) step(1);
    else if (hm && in.pressed[0]) {
        if (popup_ == id) popup_ = 0;
        else popup_ = id, popup_state_ = {r, options, nullptr, false, 0}, popup_choice_ = value;
    }
    if (row_hovered && !popup_) {
        if (in.wheel != 0) step(in.wheel > 0 ? -1 : 1), in.wheel = 0;
        else if (in.pressed[1]) step(1);
    }
    const bool live = row_hovered || popup_ == id;
    auto chevron = [&](const Rect& a, bool point_right, bool hover) {
        const float cx = a.x + a.w / 2, cy = a.y + a.h / 2, s = 5;
        Color c = hover ? GOLD_HI : live ? GOLD : GOLD.Alpha(0.45f);
        if (hover) g.Glow(Rect{cx - 12, cy - 12, 24, 24}, GOLD.Alpha(0.25f));
        const float dir = point_right ? 1.f : -1.f;
        g.Line(cx - dir * s * 0.6f, cy - s, cx + dir * s * 0.6f, cy, c, 1.6f);
        g.Line(cx + dir * s * 0.6f, cy, cx - dir * s * 0.6f, cy + s, c, 1.6f);
    };
    chevron(left, false, hl);
    chevron(right, true, hr);
    std::string label = options.empty() ? value : options[index].second;
    TextStyle s;
    s.size = 15, s.color = live ? GOLD_HI : TEXT, s.align = Align::Center, s.wrap = false, s.ellipsis = true;
    g.Text(label, middle, s, true);
    // the position among the options: small dots under the value
    if (options.size() > 1 && options.size() <= 14 && live) {
        const float gap = 7, total = gap * float(options.size() - 1);
        for (size_t i = 0; i < options.size(); ++i) {
            const float dx = middle.x + middle.w / 2 - total / 2 + gap * float(i);
            g.Dot(dx, r.Bottom() - 4, i == index ? 2.f : 1.3f, i == index ? BLOOD_HI : GOLD.Alpha(0.35f));
        }
    }
    return changed;
}

Form::Form(Ui& ui, float x_, float y_, float width) : y(y_), x(x_), w(width), ui_(ui), top_(y_) {
    value_w = std::min(330.f, std::max(240.f, w * 0.46f));
}

Rect Form::Row(std::string_view id, const std::string& label, const std::string& hint, float height, bool* hovered) {
    Rect row{x, y, w, height};
    const bool hover = ui_.Hovered(row);
    if (hovered) *hovered = hover;
    if (hover) ui_.Describe(label, hint);
    const float h = ui_.Anim(Ui::Id(std::string(id) + "#row"), hover ? 1.f : 0.f, 14);
    if (h > 0.01f) {
        ui_.g.HGradient(Rect{row.x, row.y, row.w * 0.75f, row.h}, BLOOD.Alpha(0.38f * h), BLOOD.Alpha(0.0f));
        ui_.g.HGradient(Rect{row.x, row.y, row.w * 0.7f, 1}, GOLD.Alpha(0.55f * h), GOLD.Alpha(0));
        ui_.g.HGradient(Rect{row.x, row.Bottom() - 1, row.w * 0.7f, 1}, GOLD.Alpha(0.55f * h), GOLD.Alpha(0));
        ui_.g.Glow(Rect{row.x + 2, row.y + row.h / 2 - 10, 20, 20}, BLOOD_GLOW.Alpha(h));
    }
    if (h > 0.01f) ui_.g.Fill(Rect{row.x, row.y + 8, 2, row.h - 16}, GOLD.Alpha(h));
    TextStyle s;
    s.size = 15.5f, s.color = h > 0.5f ? Rgb(0xfff4e2) : TEXT, s.wrap = false, s.ellipsis = true;
    ui_.g.Text(label, Rect{row.x + 18, row.y, w - value_w - 54, row.h}, s, true);
    ui_.g.Fill(Rect{row.x + 28, row.Bottom(), w - 28, 1}, GOLD.Alpha(0.06f));
    return row;
}

void Form::Section(const std::string& title) {
    y += y > top_ + 1 ? 26 : 4;
    TextStyle s;
    s.size = 17, s.color = GOLD, s.spacing = 2.6f, s.wrap = false, s.caps = true, s.align = Align::Center,
    s.font = Font::Display;
    const auto size = ui_.g.Measure(title, s);
    const float cx = x + w / 2, tw = size.width + 40;
    ui_.g.Text(title, Rect{cx - tw / 2, y, tw, 26}, s, true);
    const float ly = y + 13;
    const float left_end = cx - tw / 2, right_start = cx + tw / 2;
    ui_.g.HGradient(Rect{x, ly, left_end - x - 10, 1}, GOLD.Alpha(0), GOLD.Alpha(0.6f));
    ui_.g.HGradient(Rect{right_start + 10, ly, x + w - right_start - 10, 1}, GOLD.Alpha(0.6f), GOLD.Alpha(0));
    y += 40;
}

void Form::Note(const std::string& text) {
    TextStyle s;
    s.size = 13, s.color = MUTED;
    y += 8;
    y += ui_.Paragraph(text, x + 28, y, w - 28, s) + 10;
}

void Form::Warning(const std::string& text) {
    if (text.empty()) return;
    TextStyle s;
    s.size = 13.5f, s.color = WARN;
    const auto size = ui_.g.Measure(text, s, w - 56);
    Rect box{x, y, w, size.height + 22};
    ui_.g.HGradient(box, Rgb(0x46280c, 0.65f), Rgb(0x46280c, 0.15f));
    ui_.g.Fill(Rect{x, y, 2, box.h}, WARN.Alpha(0.8f));
    ui_.g.Text(text, Rect{x + 32, y + 11, w - 56, size.height + 2}, s);
    y += box.h + 14;
}

bool Form::Choice(std::string_view id, const std::string& label, std::string& value, const Options& options,
                  const std::string& hint) {
    bool hover = false;
    Rect row = Row(id, label, hint, 46, &hover);
    const bool changed = ui_.Cycle(id, Rect{row.Right() - value_w, row.y + 4, value_w, row.h - 8}, options, value, hover);
    y += row.h;
    return changed;
}

bool Form::CheckRow(std::string_view id, const std::string& label, bool& value, const std::string& hint) {
    bool hover = false;
    Rect row = Row(id, label, hint, 46, &hover);
    Options on_off = {{"0", T("Off", "Выкл.")}, {"1", T("On", "Вкл.")}};
    std::string v = value ? "1" : "0";
    bool changed = ui_.Cycle(id, Rect{row.Right() - value_w, row.y + 4, value_w, row.h - 8}, on_off, v, hover);
    // A click anywhere on the label side toggles too.
    if (!changed && hover && ui_.in.pressed[0] && ui_.in.x < row.Right() - value_w) v = value ? "0" : "1", changed = true;
    if (changed) value = v == "1";
    // the state lamp
    const float on = ui_.Anim(Ui::Id(std::string(id) + "#on"), value ? 1.f : 0.f, 14);
    const float lx = row.Right() - value_w - 18, ly = row.y + row.h / 2;
    if (on > 0.01f) ui_.g.Glow(Rect{lx - 12, ly - 12, 24, 24}, BLOOD_GLOW.Alpha(on));
    ui_.g.Dot(lx, ly, 4, BLOOD_HI.Alpha(on), GOLD.Alpha(0.4f + 0.6f * on));
    y += row.h;
    return changed;
}

bool Form::SliderRow(std::string_view id, const std::string& label, float& value, float lo, float hi, float step,
                     const std::string& hint) {
    bool hover = false;
    Rect row = Row(id, label, hint, 46, &hover);
    const bool changed = ui_.Slider(id, Rect{row.Right() - value_w, row.y + 8, value_w, row.h - 16}, value, lo, hi, step);
    y += row.h;
    return changed;
}

bool Form::EntryRow(std::string_view id, const std::string& label, std::string& value, const std::string& hint,
                    bool secret) {
    bool hover = false;
    Rect row = Row(id, label, hint, 46, &hover);
    const bool changed = ui_.Entry(id, Rect{row.Right() - value_w, row.y + 6, value_w, row.h - 12}, value, secret);
    y += row.h;
    return changed;
}

int Form::FolderRow(std::string_view id, const std::string& label, std::string& value, const std::string& hint) {
    bool hover = false;
    const std::string idt(id);
    Rect row = Row(id, label, hint + (hint.empty() ? "" : "\n\n") + value, 46, &hover);
    const float b = 34;
    int result = 0;
    Rect field{row.Right() - value_w, row.y + 6, value_w - 2 * (b + 6), row.h - 12};
    if (ui_.Entry(idt, field, value)) result = 1;
    if (ui_.Button(idt + ".browse", "…", Rect{field.Right() + 6, field.y, b, field.h}, {true})) {
        fs::path start = value.empty() ? PortDir() : PathFrom(value);
        if (auto chosen = browse_folder ? browse_folder(label, start) : std::nullopt) value = PathText(*chosen), result = 1;
    }
    if (ui_.Hovered(Rect{field.Right() + 6, field.y, b, field.h})) ui_.Describe(label, T("Browse…", "Обзор…"));
    if (ui_.Button(idt + ".open", "↗", Rect{field.Right() + 12 + b, field.y, b, field.h}, {true})) result = 2;
    if (ui_.Hovered(Rect{field.Right() + 12 + b, field.y, b, field.h})) ui_.Describe(label, T("Open", "Открыть"));
    y += row.h;
    return result;
}

int Form::Buttons(std::string_view id, const std::vector<std::string>& labels, bool small) {
    y += 10;
    float bx = x + 28;
    const float h = small ? 34 : 40;
    int pressed = -1;
    for (size_t i = 0; i < labels.size(); ++i) {
        const float bw = ui_.ButtonWidth(labels[i], small);
        if (bx + bw > x + w && bx > x + 28) bx = x + 28, y += h + 10;
        if (ui_.Button(std::string(id) + "." + std::to_string(i), labels[i], Rect{bx, y, bw, h}, {small})) pressed = int(i);
        bx += bw + 10;
    }
    y += h + 12;
    return pressed;
}
