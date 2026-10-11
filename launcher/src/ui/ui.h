// SPDX-License-Identifier: GPL-2.0-or-later
// An immediate-mode widget set in the launcher's Bloodborne look, drawn with Gfx: buttons,
// diamond check boxes, drop-down lists, sliders, text boxes, key binding slots and a form
// layout (label column, control column, hints) for the settings pages.
#pragma once
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "gfx.h"

namespace theme {
constexpr Color INK = Rgb(0x07050a), PANEL = Rgb(0x0a0608, 0.84f), PANEL_SOLID = Rgb(0x0d090b),
                CARD = Rgb(0x140d0f, 0.78f), LINE = Rgb(0xc4a068, 0.22f), LINE_STRONG = Rgb(0xd6b274, 0.55f),
                TEXT = Rgb(0xeadfce), MUTED = Rgb(0xa39482), DIM = Rgb(0x6f6357), GOLD = Rgb(0xcfae6e),
                GOLD_HI = Rgb(0xf0d699), BLOOD = Rgb(0x7a0f14), BLOOD_HI = Rgb(0xa3161d),
                BLOOD_GLOW = Rgb(0xbe1820, 0.55f), OK = Rgb(0xb8c48e), WARN = Rgb(0xe0a64b), BAD = Rgb(0xd9614f),
                FIELD = Rgb(0x060405, 0.75f);
}

struct KeyEvent {
    int key, scancode, mods;
};

struct Input {
    float x = -1, y = -1;
    bool down[5] = {}, pressed[5] = {}, released[5] = {};
    float wheel = 0;
    std::u32string chars;
    std::vector<KeyEvent> keys;
    double time = 0;
    float dt = 0;
};

enum class Cursor { Arrow, Hand, Text };
struct ButtonStyle {
    bool small = false, ghost = false, disabled = false, danger = false;
};
using Options = std::vector<std::pair<std::string, std::string>>;  // value, label

class Ui {
public:
    explicit Ui(Gfx& gfx) : g(gfx) {}
    Gfx& g;
    Input in;
    Cursor cursor = Cursor::Arrow;
    void* window = nullptr;  // HWND, for dialogs
    std::function<std::string()> clipboard_get;
    std::function<void(const std::string&)> clipboard_set;

    void BeginFrame();
    void EndFrame();  // popups and tooltips, drawn last

    static uint64_t Id(std::string_view text, uint64_t seed = 0);
    bool Hovered(const Rect& r) const;  // honours the clip and an open popup
    bool Clicked(const Rect& r, int button = 0) const { return Hovered(r) && in.pressed[button]; }
    float Anim(uint64_t id, float target, float speed = 12);  // eases toward target
    bool Busy() const { return focus_ || capture_ || popup_; }

    void PushClip(const Rect& r);
    void PopClip();

    // Widgets. They return true when the value changed or the button was pressed.
    bool Button(std::string_view id, const std::string& label, const Rect& r, ButtonStyle style = {});
    float ButtonWidth(const std::string& label, bool small = false);
    bool Check(std::string_view id, const std::string& label, bool& value, float x, float y, float width, float* height = nullptr);
    bool Combo(std::string_view id, const Rect& r, const Options& options, std::string& value);
    // The game's option value: "<  value  >"; the arrows, the wheel and right click cycle,
    // a click on the value opens the list.
    bool Cycle(std::string_view id, const Rect& r, const Options& options, std::string& value, bool row_hovered);
    // The text for the description panel (the hovered or focused setting).
    std::string desc_title, desc_text;
    void Describe(const std::string& title, const std::string& text) { desc_title = title, desc_text = text; }
    bool Slider(std::string_view id, const Rect& r, float& value, float lo, float hi, float step);
    bool Entry(std::string_view id, const Rect& r, std::string& text, bool secret = false);
    // A key binding slot: click, then press a key or mouse button (Shift/Ctrl/Alt combine);
    // right click clears. `reserved` names cannot be taken (reported through `refused`).
    bool BindSlot(std::string_view id, const Rect& r, std::string& binding, const std::vector<std::string>& reserved,
                  std::string* refused);
    void Progress(const Rect& r, float fraction);

    // Text helpers that return their height.
    float Paragraph(const std::string& text, float x, float y, float w, const TextStyle& style);

    static std::string PrettyKey(const std::string& binding);
    static std::string SdlKeyName(int glfw_key);

private:
    uint64_t active_ = 0, focus_ = 0, capture_ = 0, popup_ = 0;
    bool capture_armed_ = false;
    std::string edit_text_;
    size_t caret_ = 0;
    std::map<uint64_t, float> anim_;
    std::vector<Rect> clips_;
    // The open drop-down: drawn by EndFrame.
    struct Popup {
        Rect anchor;
        Options options;
        std::string* value = nullptr;
        bool changed = false;
        float scroll = 0;
    } popup_state_;
    Rect popup_rect_, popup_rect_last_;
    uint64_t popup_changed_ = 0;
    std::string popup_choice_;
};

// The settings form in the game's option-menu look: full-width rows (label left, value right)
// that light up under the mouse; a row's hint goes to the description panel.
class Form {
public:
    Form(Ui& ui, float x, float y, float width);
    float y;
    void Section(const std::string& title);
    void Note(const std::string& text);
    void Warning(const std::string& text);
    bool Choice(std::string_view id, const std::string& label, std::string& value, const Options& options,
                const std::string& hint = {});
    bool CheckRow(std::string_view id, const std::string& label, bool& value, const std::string& hint = {});
    bool SliderRow(std::string_view id, const std::string& label, float& value, float lo, float hi, float step,
                   const std::string& hint = {});
    bool EntryRow(std::string_view id, const std::string& label, std::string& value, const std::string& hint = {},
                  bool secret = false);
    // A folder: text box, Browse… and Open. Returns 1 changed, 2 open pressed.
    int FolderRow(std::string_view id, const std::string& label, std::string& value, const std::string& hint = {});
    // A row of buttons; the index of the pressed one, or -1.
    int Buttons(std::string_view id, const std::vector<std::string>& labels, bool small = true);
    void Space(float h) { y += h; }
    float Height() const { return y - top_; }
    float x, w, value_w;
    // The folder dialog (actions::BrowseFolder with the window).
    inline static std::function<std::optional<fs::path>(const std::string&, const fs::path&)> browse_folder;

    // The row's background and label; returns the row rectangle and whether it is hovered.
    Rect Row(std::string_view id, const std::string& label, const std::string& hint, float height, bool* hovered);

private:
    Ui& ui_;
    float top_;
};
