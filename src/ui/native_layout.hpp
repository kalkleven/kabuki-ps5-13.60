// SPDX-License-Identifier: GPL-3.0-or-later
// The native control centre's layout and motion, independent of PUI so the PC preview
// (ui/host/native_preview.cpp) runs exactly this code. A backend B makes and changes the
// widgets: on the console PUI Label/ImageBox/Panel (probe/bridge/native-worker.inc), on the
// PC a recorded widget tree that the browser draws (ui/tools/native_preview.py).
//
// Every number is a 1080p unit (the backend scales to the screen). Card measurements are the
// ones theme.hpp took from the console's own Now Playing card (4K screenshot, firmware 13.20).
#pragma once
#include "native_model.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

namespace overlay::native {

// Screen and card.
// The rail sits centred in the space right of the card, its middle tab level with the card's middle.
constexpr float kRailX = (1716 + 1920) / 2.0f;       // the rail icons' centre line
constexpr int kTabs = 7; // sidebar slots (kSlots): the tiles Settings puts there, in its order
// No console icon is a chip: the System tab's is a PNG the probe puts beside its logs.
constexpr const char* kChipIcon = "/user/data/custom-overlays/chip.png";
constexpr float kRailStep = 120; // between the tiles' centres
inline float rail_y0(int count) { return 135 + 810 / 2.0f - kRailStep * (std::max(count, 1) - 1) / 2; } // the first's centre
constexpr float kRailDisc = 72, kRailIcon = 40;
constexpr float kRailLabelW = 200, kRailLabelH = 34, kRailLabelBelow = 8;
constexpr float kCardX = 1196, kCardY = 135, kCardW = 520, kCardH = 810;
constexpr float kPad = 24, kContentW = kCardW - 2 * kPad;
constexpr float kHeaderTile = 48, kHeaderTextX = 90.5f, kHeaderBaseline = 56.5f, kHeaderSize = 26;
constexpr float kArtTop = 136, kArtSize = 286;
constexpr float kTitleBaseline = 501, kTitleSize = 30, kArtistBaseline = 547, kArtistSize = 24;
constexpr float kBarTop = 581, kBarH = 4, kScrubBarH = 6, kThumb = 18, kThumbRing = 30;
constexpr float kTimeBaseline = 614.5f, kTimeSize = 17;
constexpr float kControlsY = 685, kSlot = kContentW / 5, kControlIcon = 56;
constexpr float kDisc = 72, kRing = 84; // focused button: white disc, gradient ring (r 40.25, 2.5 wide)
constexpr float kBadgeDx = 20, kBadgeDy = -25, kBadgeIcon = 34;
constexpr float kHintBelow = 38, kHintInset = 13, kHintIcon = 44, kHintDisc = 15, kHintGap = 7.5f, kHintSize = 23.5f;
// Volume/options panel (card coordinates) and its parts (panel coordinates).
constexpr float kPopupX = 12, kPopupY = 560, kPopupW = 496, kPopupH = 230;
constexpr float kSliderX = 92, kSliderW = 380, kSliderY = 118;
constexpr float kOptionTop = 22, kOptionH = 80, kSwitchW = 64, kSwitchH = 32;
// System: rows of readings, then the console's details.
constexpr float kStatH = 74, kInfoH = 44, kSysConsole = 104 + 18 + 4 * 74 + 40;
// Settings rows, and the hold-time list (card coordinates) that opens under the second row.
constexpr float kSideH = 80; // the sidebar page's slot rows
constexpr float kSettingH = 84, kPickRowH = 56, kPickW = 300, kPickX = kCardW - 12 - kPickW;
constexpr float kPickY = 104 + 2 * kSettingH - 6, kPickH = 16 + 6 * kPickRowH;
constexpr float kListTop = 104, kRowH = 88, kRowCover = 60, kRowTitle = 36, kRowArtist = 67; // row baselines
constexpr int kRows = 8; // row widgets: the most rows in view, and one more while scrolling
constexpr float kPopMs = 260, kPopFrom = 0.94f, kFocusMs = 120, kHintFadeMs = 180;
constexpr float kCardOutMs = 180, kCardInMs = 280, kSweepMs = 1100;
// A label's text, measured on the console: PUI centres it in its box, and SST's capital
// letters then sit centred kLabelLift * size above the box's middle; the baseline is
// 0.35 * size below that. So a box for a given baseline is (baseline - kBoxTop * size, kBoxH * size).
constexpr float kLabelLift = 0.12f, kBoxTop = 1.03f, kBoxH = 1.6f;

struct Rgb { float r, g, b; };
constexpr Rgb kWhite{1, 1, 1}, kHeaderInk{200 / 255.f, 201 / 255.f, 205 / 255.f};
constexpr Rgb kSecondary{206 / 255.f, 207 / 255.f, 211 / 255.f}, kDarkInk{21 / 255.f, 22 / 255.f, 26 / 255.f};
constexpr Rgb kBarFill{183 / 255.f, 184 / 255.f, 188 / 255.f};
constexpr Rgb kBehindControls{33 / 255.f, 37 / 255.f, 40 / 255.f};
constexpr Rgb kRailInk{28 / 255.f, 30 / 255.f, 36 / 255.f};
constexpr float kDisabled = 0.33f;
constexpr Rgb kPopupFace{44 / 255.f, 48 / 255.f, 53 / 255.f}; // the volume/options panel

// PUI's enums, as the console's Sce.PlayStation.Core and PUI assemblies number them.
enum Weight { normal = 0, bold = 1, light = 2, medium = 3, ui3_light = 4, ui3_bold = 5, ui3_medium = 6 };
enum Align { left = 0, centre = 1, right = 2 };
enum Trim { ellipsis = 3, marquee = 6 };
// The pictures drawn once at startup, white unless noted: tinted with FilterColor.
enum Skin { card_face, rounded, disc, veil, ring, sweep, glow, corners };

// What holds the focus, on the screen (unscaled card): the console draws its own glass focus
// there when it can (FocusRenderWidget); otherwise the layout's rings stay. `id` names the target
// (0-4 buttons, 10-12 rail tabs, 20+ list slots, 30 the scrub thumb, -1 nothing).
struct Focus { int id = -1; float x = 0, y = 0, w = 0, h = 0, radius = 0; };

// A label's box for a text baseline, in its parent's units.
struct Box { float x, y, w, h; };
inline Box text_box(float x, float baseline, float w, float size) {
    return {x, baseline - kBoxTop * size, w, kBoxH * size};
}

// The veil over the game (theme.hpp's measured profile down the screen), darkening towards the
// right behind the rail the way the console's darkens behind its row along the bottom.
inline float veil_profile(float y) {
    struct Stop { float y, a; };
    static constexpr Stop s[] = {{0, 0.94f},   {90, 0.94f},  {135, 0.87f}, {180, 0.81f}, {225, 0.72f}, {270, 0.61f},
                                 {315, 0.54f}, {360, 0.53f}, {630, 0.53f}, {675, 0.57f}, {720, 0.64f}, {765, 0.71f},
                                 {810, 0.77f}, {855, 0.82f}, {900, 0.84f}, {1080, 0.80f}};
    constexpr int n = sizeof s / sizeof s[0];
    if (y <= s[0].y) return s[0].a;
    for (int i = 1; i < n; ++i)
        if (y <= s[i].y) return s[i - 1].a + (s[i].a - s[i - 1].a) * (y - s[i - 1].y) / (s[i].y - s[i - 1].y);
    return s[n - 1].a;
}
// The console's focus ring colour, degrees clockwise from the right (theme.hpp's samples).
inline Rgb ring_colour(float degrees) {
    struct Stop { float d; unsigned char r, g, b; };
    static constexpr Stop s[] = {{0, 132, 134, 149},   {22, 110, 114, 123},  {44, 81, 89, 92},     {66, 71, 75, 78},
                                 {88, 73, 81, 84},     {110, 96, 101, 107},  {132, 116, 120, 129}, {154, 133, 135, 148},
                                 {176, 131, 134, 153}, {198, 146, 142, 159}, {220, 158, 148, 159}, {242, 161, 147, 162},
                                 {264, 131, 134, 153}, {286, 131, 134, 149}, {330, 126, 129, 148}, {352, 128, 134, 150}};
    constexpr int n = sizeof s / sizeof s[0];
    for (int i = 0; i < n; ++i) {
        const auto& a = s[i];
        const auto& b = s[(i + 1) % n];
        const float end = i + 1 < n ? b.d : b.d + 360;
        const float at = degrees < a.d ? degrees + 360 : degrees;
        if (at >= a.d && at <= end) {
            const float t = (at - a.d) / (end - a.d);
            return {(a.r + (b.r - a.r) * t) / 255, (a.g + (b.g - a.g) * t) / 255, (a.b + (b.b - a.b) * t) / 255};
        }
    }
    return {s[0].r / 255.f, s[0].g / 255.f, s[0].b / 255.f};
}
// A skin's picture size in pixels for a w x h unit box shown `scale` pixels per unit.
inline void skin_size(int kind, float w, float h, float scale, int& pw, int& ph) {
    if (kind == veil) { pw = 64; ph = 270; return; }          // smooth: stretched
    if (kind == card_face) scale = std::min(scale, 1.0f);      // large and smooth
    if (kind == sweep) { pw = 64; ph = 1; return; }
    if (kind == glow) { pw = 64; ph = 32; return; } // soft on every side: stretched
    pw = std::max(1, int(std::lround(w * scale)));
    ph = std::max(1, int(std::lround(h * scale)));
}
// Straight-alpha RGBA for a skin; `radius` is in units (rounded rectangles).
inline std::vector<unsigned char> skin_pixels(int kind, float w, float h, float radius, int pw, int ph) {
    std::vector<unsigned char> pixels(std::size_t(pw) * ph * 4);
    const float sx = pw / w, sy = ph / h; // pixels per unit
    for (int py = 0; py < ph; ++py)
        for (int px = 0; px < pw; ++px) {
            float a = 1, r = 1, g = 1, b = 1;
            const float ux = (px + 0.5f) / sx, uy = (py + 0.5f) / sy; // the pixel's centre, in units
            if (kind == veil) {
                const float y = (py + 0.5f) * 1080 / ph, from_right = 1920 - (px + 0.5f) * 1920 / pw;
                const float t = std::clamp(1 - from_right / 760, 0.0f, 1.0f);
                const float side = 0.96f * t * t * (3 - 2 * t);
                a = std::max(veil_profile(y), side);
                r = 2 / 255.f; g = 3 / 255.f; b = 10 / 255.f;
            } else if (kind == glow) {
                const float tx = (px + 0.5f) / pw, ty = (py + 0.5f) / ph;
                const float sx = std::sin(tx * 3.14159265f), sy = std::sin(ty * 3.14159265f);
                a = 0.55f * sx * sx * sy * sy;
            } else if (kind == sweep) {
                const float t = (px + 0.5f) / pw; // a soft light, brightest in the middle
                a = 0.55f * std::sin(t * 3.14159265f) * std::sin(t * 3.14159265f);
            } else if (kind == ring) {
                const float cx = w / 2, cy = h / 2, dx = ux - cx, dy = uy - cy;
                const float d = std::sqrt(dx * dx + dy * dy);
                const float rr = radius > 0 ? radius : 40.25f; // the stroke's centre; 2.5 wide
                a = std::clamp((1.25f + 0.5f / sx - std::fabs(d - rr)) * sx, 0.0f, 1.0f);
                float deg = std::atan2(dy, dx) * 57.2957795f;
                if (deg < 0) deg += 360;
                const auto c = ring_colour(deg);
                r = c.r; g = c.g; b = c.b;
            } else {
                const float rad = kind == disc ? std::min(w, h) / 2 : radius;
                const float dx = std::max(std::fabs(ux - w / 2) - (w / 2 - rad), 0.0f);
                const float dy = std::max(std::fabs(uy - h / 2) - (h / 2 - rad), 0.0f);
                a = std::clamp((rad - std::sqrt(dx * dx + dy * dy)) * sx + 0.5f, 0.0f, 1.0f);
                if (kind == corners) { a = 1 - a; r = 22 / 255.f; g = 25 / 255.f; b = 30 / 255.f; } // the face, far from its glow
                if (kind == card_face) {
                    // Radial glow from the bottom-left corner: (42,46,49) fading to (22,25,30) at 700.
                    const float t = std::clamp(std::sqrt(ux * ux + (h - uy) * (h - uy)) / 700, 0.0f, 1.0f);
                    r = (42 + (22 - 42) * t) / 255; g = (46 + (25 - 46) * t) / 255; b = (49 + (30 - 49) * t) / 255;
                }
            }
            auto* p = &pixels[(std::size_t(py) * pw + px) * 4];
            p[0] = (unsigned char)std::lround(r * 255); p[1] = (unsigned char)std::lround(g * 255);
            p[2] = (unsigned char)std::lround(b * 255); p[3] = (unsigned char)std::lround(std::clamp(a, 0.0f, 1.0f) * 255);
        }
    return pixels;
}

inline float ease_out(float t) { t = std::clamp(t, 0.0f, 1.0f); return 1 - (1 - t) * (1 - t) * (1 - t); }
inline float smooth(float t) { t = std::clamp(t, 0.0f, 1.0f); return t * t * (3 - 2 * t); }
// Moves `value` towards `target` with time constant tau seconds; snaps when close.
inline float approach(float value, float target, float dt, float tau, float snap = 0.002f) {
    if (value < -9000) return target;
    const float next = value + (target - value) * (1 - std::exp(-dt / tau));
    return std::fabs(next - target) < snap ? target : next;
}
inline std::string clock(int ms) {
    const int s = std::max(0, ms) / 1000;
    char text[32];
    if (s >= 3600) std::snprintf(text, sizeof text, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    else std::snprintf(text, sizeof text, "%d:%02d", s / 60, s % 60);
    return text;
}

// B provides (W = typename B::Widget; `parent` is a widget, `b.screen()` the screen):
//   bool panel(W&, W& parent, Box, bool clip)
//   bool skin(W&, W& parent, Box, int kind, float radius)
//   bool image(W&, W& parent, Box, const std::string& uri)          (aspect-fit picture)
//   bool label(W&, W& parent, Box, float size, int weight, Rgb, int align, int trim, text)
//   void geometry(W&, Box); show(W&, bool); alpha(W&, float); words(W&, text);
//   void tint(W&, Rgb, float a); picture(W&, uri); scale(W&, float s) (about the centre)
//   float text_width(W& label, text)                                (units)
//   std::string icon(name)                                          (a console icon id)
//   bool focus(const Focus&, int presses)  true when the backend draws the focus itself
//   std::string fallback_art()              the console's "no cover" picture
template <class B> class Layout {
    using W = typename B::Widget;
    B& b;
    W veil_, card_, face_, header_tile_, header_icon_, header_, header_b_;
    std::string head_words_[2]; int head_now_ = 0; std::uint64_t head_at_ = 0;
    W header_art_, header_corners_; // an open playlist's or album's cover in place of the header's icon
    float header_art_a_ = 0;
    std::string header_art_uri_;
    W cover_, title_, artist_, bar_, bar_base_, bar_fill_, sweep_, thumb_ring_, thumb_, clock_, duration_;
    W controls_[5], focus_ring_, focus_, keys_[2], square_; // keys_: the console's own L1 and R1 marks
    W hint_[2], hint_square_;
    // Volume and options: a panel over the bar and buttons, in the console's own controls (a
    // slider with the UI3 handle, a switch with the UI3 knob).
    W popup_, popup_face_, popup_title_, popup_value_, popup_hint_, vol_icon_, vol_base_, vol_fill_, vol_knob_;
    W opt_focus_, opt_icon_[2], opt_label_[2], opt_value_, sw_track_, sw_check_, sw_knob_;
    // System: readings in two sections.
    static constexpr int kStatRows = 8;
    W sys_heading_[2], sys_label_[kStatRows], sys_value_[kStatRows], sys_base_[2], sys_fill_[2], sys_sub_;
    // Settings: three rows (shortcut, hold time, sounds), the hold-time list, the version.
    static constexpr int kSettingRows = 5, kChoices = 6;
    W set_focus_, set_label_[kSettingRows], set_value_[kSettingRows], set_note_, set_track_, set_check_, set_knob_, set_chevron_;
    W side_icon_[kSlots], side_label_[kSlots], side_more_, libre_more_; // submenu arrows and the sidebar page's slots
    W pick_, pick_face_, pick_focus_, pick_label_[kChoices], pick_tick_[kChoices], version_, version_low_, link_;
    W side_star_; // the first slot's tile opens with the panel
    W message_, message2_, list_heading_, list_, row_focus_, row_sweep_;
    W row_tile_[kRows], row_cover_[kRows], row_title_[kRows], row_artist_[kRows];
    // While a list loads: placeholder rows (a cover tile, title and artist bars) with a light that
    // sweeps across them row after row. No "Loading" text; the real rows fade in over them.
    W skel_, skel_tile_[kRows], skel_title_[kRows], skel_artist_[kRows], skel_sweep_[kRows];
    W rail_disc_, rail_[kTabs], rail_label_[kTabs];

    // Motion state.
    float pop_ = 0, pop_from_ = 0;
    bool open_ = false;
    std::uint64_t pop_at_ = 0, last_ = 0, card_at_ = 0, opening_ = 0, focus_at_ = 0, hint_at_ = 0;
    NativeView display_{};
    bool known_ = false, card_out_ = false;
    float card_fade_ = 1, card_from_ = 1;
    float focus_x_ = -9999, rail_y_ = -9999, scroll_ = -9999, sel_ = 0, scrub_ = 0;
    std::string list_key_ = "-";
    float rows_a_ = 0, skel_a_ = 0, msg_a_ = 0;
    bool rows_out_ = false; // the last list's rows fading out before the new list's are shown
    float rail_disc_a_ = 0, rail_icon_a_[kTabs]{}, rail_ink_[kTabs]{}, rail_label_a_[kTabs]{};
    // Wrapping round the rail: the disc slides on past the end, fading, and in from beyond the other end.
    int rail_at_ = -1, wrap_dir_ = 0;
    float wrap_t_ = -1, wrap_from_ = 0, wrap_a_ = 1, wrap_a0_ = 1;
    float popup_t_ = 0, opt_y_ = -9999, vol_ = -1, sw_ = -1;
    float set_y_ = -9999, set_sw_ = -1, music_sw_ = -1, pick_t_ = 0, pick_y_ = -9999;
    std::map<std::string, float> rail_tile_y_, rail_tile_a_; // per tile: its centre on the rail, fading in when put on
    std::map<std::string, float> side_tile_y_, side_tile_a_; // the Sidebar page's rows, per tile
    float side_empty_y_[kSlots];
    void unplace() {
        rail_tile_y_.clear(); rail_tile_a_.clear(); side_tile_y_.clear(); side_tile_a_.clear();
        for (auto& y : side_empty_y_) y = -9999;
    }
    int popup_kind_ = 1;
    int focus_key_ = -1;
    float focus_a_ = 0, ink_[5]{}; // the button disc's presence and each icon's darkness (they fade)
    Focus want_{};
    bool system_focus_ = false;
    std::string hint_words_[2];
    int hint_now_ = 0;
    float hint_left_ = -9999;

    float time(std::uint64_t now, std::uint64_t at, float ms) const { return float(now - at) / (ms * 1000); }

public:
    std::function<Focus(W&,const NativeView&,std::uint64_t,float)> plugin;
    explicit Layout(B& backend) : b(backend) {}

    bool create() {
        W& screen = b.screen();
        if (!b.skin(veil_, screen, {0, 0, 1920, 1080}, veil, 0)) return false;
        if (!b.panel(card_, screen, {kCardX, kCardY, kCardW, kCardH}, false)) return false;
        W& c = card_;
        bool ok = b.skin(face_, c, {0, 0, kCardW, kCardH}, card_face, 13) &&
            b.skin(header_tile_, c, {kPad, kPad, kHeaderTile, kHeaderTile}, rounded, 6) &&
            b.image(header_icon_, c, {kPad, kPad, kHeaderTile, kHeaderTile}, b.icon("iconid_music")) &&
            b.image(header_art_, c, {kPad, kPad, kHeaderTile, kHeaderTile}, "") &&
            b.skin(header_corners_, c, {kPad, kPad, kHeaderTile, kHeaderTile}, corners, 6) &&
            b.label(header_, c, text_box(kHeaderTextX, kHeaderBaseline, kCardW - kHeaderTextX - kPad, kHeaderSize),
                    kHeaderSize, light, kHeaderInk, left, ellipsis, "Now Playing") &&
            b.label(header_b_, c, text_box(kHeaderTextX, kHeaderBaseline, kCardW - kHeaderTextX - kPad, kHeaderSize),
                    kHeaderSize, light, kHeaderInk, left, ellipsis, "") &&
            b.image(cover_, c, {(kCardW - kArtSize) / 2, kArtTop, kArtSize, kArtSize}, b.icon("iconid_music")) &&
            b.label(title_, c, text_box(kPad, kTitleBaseline, kContentW, kTitleSize), kTitleSize, light, kWhite, left, marquee, "") &&
            b.label(artist_, c, text_box(kPad, kArtistBaseline, kContentW, kArtistSize), kArtistSize, light, kSecondary, left, marquee, "") &&
            b.panel(bar_, c, {kPad, kBarTop - 1, kContentW, kScrubBarH}, true) &&
            b.skin(bar_base_, bar_, {0, 1, kContentW, kBarH}, rounded, 0) &&
            b.skin(bar_fill_, bar_, {0, 1, 0.1f, kBarH}, rounded, 0) &&
            b.skin(sweep_, bar_, {0, 0, kContentW * 0.3f, kScrubBarH}, sweep, 0) &&
            b.skin(thumb_ring_, c, {0, 0, kThumbRing, kThumbRing}, ring, 13.25f) &&
            b.skin(thumb_, c, {0, 0, kThumb, kThumb}, disc, 0) &&
            b.label(clock_, c, text_box(kPad, kTimeBaseline, kContentW / 2, kTimeSize), kTimeSize, bold, kWhite, left, ellipsis, "") &&
            b.label(duration_, c, text_box(kPad + kContentW / 2, kTimeBaseline, kContentW / 2, kTimeSize), kTimeSize, bold, kWhite, right, ellipsis, "") &&
            b.skin(focus_ring_, c, {0, 0, kRing, kRing}, ring, 40.25f) &&
            b.skin(focus_, c, {0, 0, kDisc, kDisc}, disc, 0);
        if (!ok) return false;
        b.tint(header_tile_, kWhite, 1); b.tint(header_icon_, kDarkInk, 1);
        b.tint(bar_base_, kWhite, 0.15f); b.tint(bar_fill_, kBarFill, 1);
        const char* icons[] = {"iconid_sound_speaking", "iconid_control_contents_reverse", "iconid_control_play",
                               "iconid_control_contents_forward", "iconid_option"};
        for (int i = 0; i < 5; ++i)
            if (!b.image(controls_[i], c, {control_x(i) - kControlIcon / 2, kControlsY - kControlIcon / 2, kControlIcon, kControlIcon},
                         b.icon(icons[i]))) return false;
        // L1 and R1: the console's key marks, in the same box as the Square mark.
        const char* keys[] = {"iconid_controller_key_l1", "iconid_controller_key_r1"};
        for (int i = 0; i < 2; ++i) {
            const float x = control_x(i ? 3 : 1) + kBadgeDx, y = kControlsY + kBadgeDy;
            if (!b.image(keys_[i], c, {x - kBadgeIcon / 2, y - kBadgeIcon / 2, kBadgeIcon, kBadgeIcon}, b.icon(keys[i])))
                return false;
        }
        {
            const float x = control_x(2) + kBadgeDx, y = kControlsY + kBadgeDy;
            if (!b.image(square_, c, {x - kBadgeIcon / 2, y - kBadgeIcon / 2, kBadgeIcon, kBadgeIcon},
                         b.icon("iconid_controller_key_face_square"))) return false;
        }
        // The hint under the card, right-aligned to it: two words to crossfade, the Square before.
        const float hint_y = kCardH + kHintBelow;
        for (auto& h : hint_)
            if (!b.label(h, c, text_box(0, hint_y + 9, kCardW - kHintInset, kHintSize), kHintSize, ui3_bold, kWhite, right, ellipsis, ""))
                return false;
        if (!b.image(hint_square_, c, {0, hint_y - kHintIcon / 2, kHintIcon, kHintIcon}, b.icon("iconid_controller_key_face_square")))
            return false;
        // Lists.
        ok = b.label(message_, c, text_box(kPad, kListTop + 48, kContentW, kTitleSize), kTitleSize, light, kWhite, left, ellipsis, "") &&
             b.label(message2_, c, text_box(kPad, kListTop + 88, kContentW, kArtistSize), kArtistSize, light, kSecondary, left, ellipsis, "") &&
             b.label(list_heading_, c, text_box(kPad, kListTop + 18, kContentW, kTimeSize), kTimeSize, bold, kSecondary, left, ellipsis, "") &&
             b.panel(list_, c, {0, kListTop, kCardW, 7 * kRowH}, true) &&
             b.skin(row_focus_, list_, {12, 3, kCardW - 24, kRowH - 6}, rounded, 10) &&
             b.skin(row_sweep_, list_, {0, 0, 140, 3}, sweep, 0);
        if (!ok) return false;
        b.tint(row_focus_, kWhite, 34 / 255.f);
        for (int i = 0; i < kRows; ++i) {
            if (!b.skin(row_tile_[i], list_, {kPad, 0, kRowCover, kRowCover}, rounded, 4) ||
                !b.image(row_cover_[i], list_, {kPad, 0, kRowCover, kRowCover}, "") ||
                !b.label(row_title_[i], list_, {0, 0, 10, 34}, 24, normal, kWhite, left, ellipsis, "") ||
                !b.label(row_artist_[i], list_, {0, 0, 10, 30}, 20, light, kSecondary, left, ellipsis, ""))
                return false;
            b.tint(row_tile_[i], kBehindControls, 1);
        }
        if (!b.panel(skel_, c, {0, kListTop, kCardW, 7 * kRowH}, true)) return false;
        for (int i = 0; i < kRows; ++i) {
            if (!b.skin(skel_tile_[i], skel_, {kPad, 0, kRowCover, kRowCover}, rounded, 4) ||
                !b.skin(skel_title_[i], skel_, {0, 0, 240, 14}, rounded, 7) ||
                !b.skin(skel_artist_[i], skel_, {0, 0, 160, 11}, rounded, 5.5f) ||
                !b.skin(skel_sweep_[i], skel_, {0, 0, 220, kRowH}, glow, 0))
                return false;
            b.tint(skel_tile_[i], kWhite, 0.07f); b.tint(skel_title_[i], kWhite, 0.10f); b.tint(skel_artist_[i], kWhite, 0.07f);
            b.tint(skel_sweep_[i], kWhite, 0.22f);
        }
        // Volume and options, over the bar and the buttons.
        ok = b.panel(popup_, c, {kPopupX, kPopupY, kPopupW, kPopupH}, false) &&
             b.skin(popup_face_, popup_, {0, 0, kPopupW, kPopupH}, rounded, 13) &&
             b.label(popup_title_, popup_, text_box(kPad, 50, 300, kHeaderSize), kHeaderSize, light, kWhite, left, ellipsis, "") &&
             b.label(popup_value_, popup_, text_box(kPopupW - kPad - 150, 50, 150, kHeaderSize), kHeaderSize, light, kWhite, right, ellipsis, "") &&
             b.label(popup_hint_, popup_, text_box(kPad, kPopupH - 22, kPopupW - 2 * kPad, kTimeSize), kTimeSize, bold, kSecondary, left, ellipsis, "") &&
             b.image(vol_icon_, popup_, {kPad, kSliderY - 22, 44, 44}, b.icon("iconid_sound_speaking")) &&
             b.skin(vol_base_, popup_, {kSliderX, kSliderY - 3, kSliderW, 6}, rounded, 3) &&
             b.skin(vol_fill_, popup_, {kSliderX, kSliderY - 3, kSliderW, 6}, rounded, 3) &&
             b.image(vol_knob_, popup_, {kSliderX - 24, kSliderY - 24, 48, 48}, b.icon("image_slider_handle_highlight")) &&
             b.skin(opt_focus_, popup_, {12, 0, kPopupW - 24, kOptionH - 6}, rounded, 10) &&
             b.label(opt_value_, popup_, text_box(kPopupW - kPad - 160, 0, 160, 24), 24, light, kSecondary, right, ellipsis, "") &&
             b.skin(sw_track_, popup_, {kPopupW - kPad - kSwitchW, 0, kSwitchW, kSwitchH}, rounded, kSwitchH / 2) &&
             b.image(sw_check_, popup_, {0, 0, 28, 28}, b.icon("image_switch_on_label")) &&
             b.image(sw_knob_, popup_, {0, 0, 48, 48}, b.icon("image_switch_knob_highlight"));
        if (!ok) return false;
        b.tint(popup_face_, kPopupFace, 1); b.tint(vol_base_, kWhite, 0.18f); b.tint(opt_focus_, kWhite, 34 / 255.f);
        const char* options[] = {"iconid_control_shuffle", "iconid_control_repeat"};
        const char* option_names[] = {"Shuffle", "Repeat"};
        for (int i = 0; i < 2; ++i) {
            const float y = kOptionTop + i * kOptionH;
            if (!b.image(opt_icon_[i], popup_, {kPad, y + (kOptionH - 6) / 2 - 20, 40, 40}, b.icon(options[i])) ||
                !b.label(opt_label_[i], popup_, text_box(kPad + 58, y + (kOptionH - 6) / 2 + 9, 240, 24), 24, normal, kWhite, left, ellipsis, option_names[i]))
                return false;
        }
        // System.
        const char* stat_names[] = {"CPU", "Memory", "Frame rate", "Temperature", "Firmware", "Uptime", "IP address", "librespot-ps5"};
        for (int i = 0; i < 2; ++i)
            if (!b.label(sys_heading_[i], c, text_box(kPad, i ? kSysConsole : kListTop + 18, kContentW, kTimeSize), kTimeSize, bold, kSecondary,
                         left, ellipsis, i ? "Console" : "Performance"))
                return false;
        for (int i = 0; i < kStatRows; ++i) {
            const float base = stat_baseline(i), size = i < 4 ? 22 : 20;
            if (!b.label(sys_label_[i], c, text_box(kPad, base, 240, size), size, normal, i < 4 ? kWhite : kSecondary, left, ellipsis, stat_names[i]) ||
                !b.label(sys_value_[i], c, text_box(kCardW - kPad - 260, base, 260, size), size, light, kWhite, right, ellipsis, ""))
                return false;
        }
        for (int i = 0; i < 2; ++i) {
            const float y = stat_baseline(i) + 16;
            if (!b.skin(sys_base_[i], c, {kPad, y, kContentW, 4}, rounded, 2) || !b.skin(sys_fill_[i], c, {kPad, y, 0.1f, 4}, rounded, 2))
                return false;
            b.tint(sys_base_[i], kWhite, 0.15f);
        }
        if (!b.label(sys_sub_, c, text_box(kPad, stat_baseline(2) + 28, kContentW, kTimeSize), kTimeSize, light, kSecondary, left, ellipsis, ""))
            return false;
        // Settings.
        const char* setting_names[] = {"Open shortcut", "Hold to open", "Interface sounds", "Sidebar", "Librespot"};
        if (!b.skin(set_focus_, c, {12, kListTop, kCardW - 24, kSettingH - 6}, rounded, 10)) return false;
        b.tint(set_focus_, kWhite, 34 / 255.f);
        for (int i = 0; i < kSettingRows; ++i) {
            const float base = kListTop + i * kSettingH + (kSettingH - 6) / 2 + 9;
            if (!b.label(set_label_[i], c, text_box(kPad, base, 260, 24), 24, normal, kWhite, left, ellipsis, setting_names[i]) ||
                !b.label(set_value_[i], c, text_box(kCardW - kPad - 230, base, i == 1 || i == 3 || i == 4 ? 196 : 230, 24), 24, light, kSecondary, right, ellipsis, ""))
                return false;
        }
        const float sw_y = kListTop + 2 * kSettingH + (kSettingH - 6) / 2;
        ok = b.label(set_note_, c, text_box(kPad, kListTop + 4 * kSettingH + 26, kContentW, kTimeSize), kTimeSize, light, kSecondary, left, ellipsis, "") &&
             b.skin(set_track_, c, {kCardW - kPad - kSwitchW, sw_y - kSwitchH / 2, kSwitchW, kSwitchH}, rounded, kSwitchH / 2) &&
             b.image(set_check_, c, {kCardW - kPad - kSwitchW + 6, sw_y - 14, 28, 28}, b.icon("image_switch_on_label")) &&
             b.image(set_knob_, c, {0, sw_y - 24, 48, 48}, b.icon("image_switch_knob_highlight")) &&
             b.image(set_chevron_, c, {kCardW - kPad - 28, kListTop + kSettingH + (kSettingH - 6) / 2 - 14, 28, 28}, b.icon("image_pulldown_arrow")) &&
             // Kabuki's version: the bottom left of the screen while the panel is open.
             // With Settings showing, bottom left of the screen: Kabuki's version and where it comes from.
             b.image(side_more_, c, {kCardW - kPad - 28, kListTop + 3 * kSettingH + (kSettingH - 6) / 2 - 14, 28, 28}, b.icon("iconid_arrow_right")) &&
             b.image(libre_more_, c, {kCardW - kPad - 28, kListTop + 4 * kSettingH + (kSettingH - 6) / 2 - 14, 28, 28}, b.icon("iconid_arrow_right")) &&
             b.label(version_, b.screen(), text_box(48, 1080 - 66, 500, 20), 20, normal, kSecondary, left, ellipsis, "Kabuki v0.2.0-rc.1") &&
             b.label(version_low_, b.screen(), text_box(48, 1080 - 38, 500, 20), 20, normal, kSecondary, left, ellipsis, "Kabuki v0.2.0-rc.1") &&
             b.image(side_star_, c, {0, 0, 22, 22}, b.icon("iconid_rating")) &&
             b.label(link_, b.screen(), text_box(48, 1080 - 38, 500, 20), 20, normal, kSecondary, left, ellipsis,
                     "github.com/stupiditytries") &&
             // The hold-time list opens under its row, over the rest of the card.
             b.panel(pick_, c, {kPickX, kPickY, kPickW, kPickH}, false) &&
             b.skin(pick_face_, pick_, {0, 0, kPickW, kPickH}, rounded, 13) &&
             b.skin(pick_focus_, pick_, {8, 8, kPickW - 16, kPickRowH - 4}, rounded, 10);
        if (!ok) return false;
        b.tint(pick_face_, kPopupFace, 1); b.tint(pick_focus_, kWhite, 34 / 255.f); b.tint(set_chevron_, kSecondary, 1);
        b.tint(side_more_, kSecondary, 1); b.tint(libre_more_, kSecondary, 1);
        for (int i = 0; i < kSlots; ++i)
            if (!b.image(side_icon_[i], c, {kPad, kListTop + i * kSideH + (kSideH - 6) / 2 - 16, 32, 32}, "") ||
                !b.label(side_label_[i], c, text_box(kPad + 52, kListTop + i * kSideH + (kSideH - 6) / 2 + 9, 360, 24), 24, normal, kWhite,
                         left, ellipsis, ""))
                return false;
        for (int i = 0; i < kChoices; ++i) {
            const float y = 8 + i * kPickRowH, base = y + (kPickRowH - 4) / 2 + 8;
            char text[16]; std::snprintf(text, sizeof text, "%.2g s", kHoldChoices[i] / 1000.0);
            if (!b.label(pick_label_[i], pick_, text_box(24, base, kPickW - 90, 22), 22, normal, kWhite, left, ellipsis, text) ||
                !b.image(pick_tick_[i], pick_, {kPickW - 52, y + (kPickRowH - 4) / 2 - 14, 28, 28}, b.icon("image_checkbox_checked")))
                return false;
        }
        // The rail: disc, icons, the focused tab's name under its disc.
        if (!b.skin(rail_disc_, screen, {kRailX - kRailDisc / 2, rail_y0(5) - kRailDisc / 2, kRailDisc, kRailDisc}, disc, 0)) return false;
        b.tint(rail_disc_, {242 / 255.f, 243 / 255.f, 245 / 255.f}, 1);
        for (int i = 0; i < kTabs; ++i) {
            const float y = rail_y0(kTabs) + i * kRailStep;
            if (!b.image(rail_[i], screen, {kRailX - kRailIcon / 2, y - kRailIcon / 2, kRailIcon, kRailIcon}, "") ||
                !b.label(rail_label_[i], screen, {kRailX - kRailLabelW / 2, y + kRailDisc / 2 + kRailLabelBelow, kRailLabelW, kRailLabelH},
                         22, normal, kWhite, centre, ellipsis, ""))
                return false;
            b.alpha(rail_label_[i], 0);
        }
        b.alpha(rail_disc_, 0);
        b.show(screen, false);
        return true;
    }

    static float control_x(int i) { return kPad + kSlot * i + kSlot / 2; }

    // Every widget this layout made (not the screen), for the backend to let go of.
    template <class F> void each(F f) {
        for (W* w : {&veil_, &card_, &face_, &header_tile_, &header_icon_, &header_art_, &header_corners_, &header_, &header_b_, &cover_, &title_, &artist_, &bar_,
                     &bar_base_, &bar_fill_, &sweep_, &thumb_ring_, &thumb_, &clock_, &duration_, &focus_ring_, &focus_,
                     &square_, &hint_square_, &popup_, &popup_face_, &popup_title_, &popup_value_, &popup_hint_, &vol_icon_,
                     &vol_base_, &vol_fill_, &vol_knob_, &opt_focus_, &opt_value_, &sw_track_, &sw_check_, &sw_knob_, &sys_sub_,
                     &message_, &message2_, &list_heading_, &list_, &row_focus_, &row_sweep_, &rail_disc_})
            f(*w);
        for (auto& w : opt_icon_) f(w);
        for (auto& w : opt_label_) f(w);
        for (auto& w : sys_heading_) f(w);
        for (auto& w : sys_label_) f(w);
        for (auto& w : sys_value_) f(w);
        for (auto& w : sys_base_) f(w);
        for (auto& w : sys_fill_) f(w);
        for (W* w : {&set_focus_, &set_note_, &set_track_, &set_check_, &set_knob_, &set_chevron_, &pick_, &pick_face_, &pick_focus_, &version_,
                     &link_, &side_more_, &libre_more_, &version_low_, &side_star_})
            f(*w);
        for (auto& w : set_label_) f(w);
        for (auto& w : set_value_) f(w);
        for (auto& w : pick_label_) f(w);
        for (auto& w : pick_tick_) f(w);
        for (auto& w : side_icon_) f(w);
        for (auto& w : side_label_) f(w);
        for (auto& w : controls_) f(w);
        for (auto& w : keys_) f(w);
        for (auto& w : hint_) f(w);
        for (auto& w : rail_) f(w);
        for (auto& w : rail_label_) f(w);
        for (int i = 0; i < kRows; ++i) { f(row_tile_[i]); f(row_cover_[i]); f(row_title_[i]); f(row_artist_[i]); }
        f(skel_);
        for (int i = 0; i < kRows; ++i) { f(skel_tile_[i]); f(skel_title_[i]); f(skel_artist_[i]); f(skel_sweep_[i]); }
    }

    // One ShellUI frame. `active` the panel is open; `opening` changes with each opening.
    void frame(const NativeView& view, bool active, std::uint64_t now_us, std::uint64_t opening) {
        const float dt = last_ ? std::min(0.1f, float(now_us - last_) / 1e6f) : 1 / 60.0f;
        last_ = now_us;
        // The console's pop: grows from kPopFrom about the card's centre as it and the veil fade in.
        // A new opening waits (a frame or two) for a view of it, not the last opening's state.
        if (active && !open_ && view.opening != opening) active = false;
        if (active != open_) { open_ = active; pop_at_ = now_us; pop_from_ = pop_; }
        const float e = ease_out(time(now_us, pop_at_, kPopMs));
        pop_ = open_ ? pop_from_ + (1 - pop_from_) * e : pop_from_ * (1 - e);
        if (open_ ? pop_ > 0.998f : pop_ < 0.002f) pop_ = open_ ? 1 : 0;
        W& screen = b.screen();
        b.show(screen, open_ || pop_ > 0);
        b.alpha(screen, pop_);
        b.scale(card_, kPopFrom + (1 - kPopFrom) * pop_);
        want_ = {};
        if (!open_) system_focus_ = b.focus(want_, view.presses); // the console's focus fades out first
        if (!open_ && pop_ <= 0) { known_ = false; return; }

        if (!known_ || opening != opening_) {
            opening_ = opening; known_ = true; display_ = view;
            card_fade_ = card_from_ = 1; card_out_ = false; card_at_ = now_us;
            focus_x_ = rail_y_ = scroll_ = -9999; focus_key_ = -1; rows_a_ = skel_a_ = msg_a_ = -1; focus_a_ = 0; for (auto& k : ink_) k = 0;
            hint_words_[0] = hint_words_[1] = ""; hint_left_ = -9999;
            head_words_[0] = head_words_[1] = "";
            rail_disc_a_ = 0; for (int i = 0; i < kTabs; ++i) { rail_icon_a_[i] = rail_label_a_[i] = 0; rail_ink_[i] = 1; }
            rail_at_ = -1; wrap_t_ = -1; wrap_a_ = 1;
            popup_t_ = 0; opt_y_ = -9999; vol_ = sw_ = -1; set_y_ = pick_y_ = -9999; set_sw_ = -1; pick_t_ = 0;
            unplace();
        }
        // Between tabs: the old card fades out, the new one fades in.
        if (view.tile != display_.tile && !card_out_) { card_out_ = true; card_at_ = now_us; card_from_ = card_fade_; }
        else if (view.tile == display_.tile && card_out_) { card_out_ = false; card_at_ = now_us; card_from_ = card_fade_; }
        const float t = time(now_us, card_at_, card_out_ ? kCardOutMs : kCardInMs);
        card_fade_ = card_out_ ? card_from_ * (1 - smooth(t)) : card_from_ + (1 - card_from_) * smooth(t);
        if (card_out_ && t >= 1) { display_ = view; card_out_ = false; card_at_ = now_us; card_from_ = 0; scroll_ = -9999; }
        else if (view.tile == display_.tile) display_ = view;
        b.alpha(card_, card_fade_);
        // Kabuki's version, always at the bottom left; with Settings, the GitHub line fades in under it
        // (with the card, between tabs) and the version moves up a line to make room.
        const float link = display_.tab == kSettings ? card_fade_ : 0;
        for (W* w : {&link_, &version_}) { b.alpha(*w, link); b.show(*w, link > 0.001f); } // fades only: no movement
        b.alpha(version_low_, 1 - link); b.show(version_low_, link < 0.999f);
        render(display_, now_us, dt);
        rail(view, dt);
        if (open_) {
            if (card_out_) want_ = {}; // between tabs: the focus waits for the new card
            system_focus_ = b.focus(want_, view.presses);
        }
    }

private:
    // A tile's icon: one of the console's ("icon:<id>"), Kabuki's chip, or a provider's picture.
    std::string tile_icon(const std::string& spec) {
        if (spec.compare(0, 5, "icon:") == 0) return b.icon(spec.c_str() + 5);
        if (spec == "chip") return kChipIcon;
        if (spec.compare(0, 5, "file:") == 0) return spec.substr(5);
        return "";
    }
    void rail(const NativeView& v, float dt) {
        const int n = std::min(int(v.rail.size()), kTabs), at = std::min(v.rail_at, n - 1);
        const float y0 = rail_y0(n);
        const bool on_rail = !v.focused && at >= 0;
        if (on_rail) want_ = {10 + at, kRailX - kRailDisc / 2, y0 + at * kRailStep - kRailDisc / 2, kRailDisc, kRailDisc, kRailDisc / 2};
        const bool moved = at >= 0 && rail_at_ >= 0 && at != rail_at_;
        if (moved && on_rail && n > 2 && rail_y_ > -9000 && ((rail_at_ == 0 && at == n - 1) || (rail_at_ == n - 1 && at == 0))) {
            wrap_t_ = 0; wrap_dir_ = at == 0 ? 1 : -1; wrap_from_ = rail_y_; wrap_a0_ = wrap_a_;
        } else if (moved) wrap_t_ = -1; // an ordinary step: glide on from here
        if (at >= 0) rail_at_ = at;
        if (at >= 0) {
            const float target = y0 + at * kRailStep - kRailDisc / 2;
            if (wrap_t_ >= 0) {
                constexpr float out = 0.07f, in = 0.13f, reach = 0.3f * kRailStep;
                wrap_t_ += dt;
                if (wrap_t_ < out) {
                    const float p = smooth(wrap_t_ / out);
                    rail_y_ = wrap_from_ + wrap_dir_ * reach * p; wrap_a_ = wrap_a0_ * (1 - p);
                } else {
                    const float p = ease_out((wrap_t_ - out) / in);
                    rail_y_ = target - wrap_dir_ * reach * (1 - p); wrap_a_ = p;
                    if (p >= 1) wrap_t_ = -1;
                }
            } else {
                rail_y_ = approach(rail_y_, target, dt, 0.05f, 0.05f);
                wrap_a_ = approach(wrap_a_, 1, dt, 0.03f);
            }
        }
        const float wrap_a = wrap_a_;
        rail_disc_a_ = approach(rail_disc_a_, on_rail ? 1 : 0, dt, 0.09f); // ~0.3 s, crossfading with the card's focus
        b.geometry(rail_disc_, {kRailX - kRailDisc / 2, rail_y_, kRailDisc, kRailDisc});
        b.alpha(rail_disc_, rail_disc_a_ * wrap_a); b.show(rail_disc_, rail_disc_a_ * wrap_a > 0);
        for (auto it = rail_tile_y_.begin(); it != rail_tile_y_.end();) { // tiles gone from the rail fade in again when back
            const bool there = std::any_of(v.rail.begin(), v.rail.end(), [&](const TileView& t) { return t.id == it->first; });
            if (!there) { rail_tile_a_.erase(it->first); it = rail_tile_y_.erase(it); } else ++it;
        }
        for (int i = 0; i < kTabs; ++i) {
            const bool there = i < n;
            b.show(rail_[i], there);
            if (!there) { b.show(rail_label_[i], false); continue; }
            const auto& tile = v.rail[std::size_t(i)].id;
            float& at_y = rail_tile_y_.try_emplace(tile, -9999.0f).first->second;
            at_y = at_y < -9000 ? y0 + i * kRailStep : approach(at_y, y0 + i * kRailStep, dt, 0.045f, 0.05f);
            rail_tile_a_[tile] = approach(rail_tile_a_[tile], 1, dt, 0.06f);
            const float y = at_y;
            b.picture(rail_[i], tile_icon(v.rail[std::size_t(i)].icon)); b.words(rail_label_[i], v.rail[std::size_t(i)].title);
            b.geometry(rail_[i], {kRailX - kRailIcon / 2, y - kRailIcon / 2, kRailIcon, kRailIcon});
            b.geometry(rail_label_[i], {kRailX - kRailLabelW / 2, y + kRailDisc / 2 + kRailLabelBelow, kRailLabelW, kRailLabelH});
            const bool lit = on_rail && at == i;
            rail_icon_a_[i] = approach(rail_icon_a_[i], lit ? 1 : on_rail ? 0.62f : v.tile == tile ? 1 : 0.38f, dt, 0.09f);
            rail_ink_[i] = approach(rail_ink_[i], lit ? 0 : 1, dt, 0.08f);
            rail_label_a_[i] = approach(rail_label_a_[i], lit ? 1 : 0, dt, 0.09f);
            const float ink = rail_ink_[i];
            b.tint(rail_[i], {kRailInk.r + (1 - kRailInk.r) * ink, kRailInk.g + (1 - kRailInk.g) * ink, kRailInk.b + (1 - kRailInk.b) * ink}, 1);
            b.alpha(rail_[i], rail_icon_a_[i] * rail_tile_a_[tile]);
            b.alpha(rail_label_[i], rail_label_a_[i] * rail_tile_a_[tile]); b.show(rail_label_[i], rail_label_a_[i] > 0);
        }
    }

    // Inside a playlist or album, its cover (corners rounded like the icon's tile) fades in over the icon.
    void header_art(const NativeView& v, float dt) {
        const std::string& cover = v.header_art;
        if (!cover.empty() && cover != header_art_uri_) { header_art_uri_ = cover; b.picture(header_art_, cover); header_art_a_ = 0; }
        header_art_a_ = approach(header_art_a_, cover.empty() ? 0 : 1, dt, 0.06f);
        for (W* w : {&header_art_, &header_corners_}) { b.alpha(*w, header_art_a_); b.show(*w, header_art_a_ > 0); }
        b.tint(header_art_, kWhite, 1); b.tint(header_corners_, kWhite, 1);
    }

    void title(const std::string& words, std::uint64_t now) {
        static constexpr float kTitleFadeMs = 180;
        if (words != head_words_[head_now_]) {
            const bool first = head_words_[head_now_].empty(); // an opening: no fade
            head_now_ ^= 1; head_words_[head_now_] = words; head_at_ = first ? 0 : now;
            if (first) head_words_[head_now_ ^ 1].clear();
        }
        W* label[] = {&header_, &header_b_};
        const float t = head_at_ ? std::min(1.0f, time(now, head_at_, kTitleFadeMs)) : 1;
        b.words(*label[head_now_], head_words_[head_now_]);
        b.alpha(*label[head_now_], ease_out(t)); b.show(*label[head_now_], true);
        b.alpha(*label[head_now_ ^ 1], 1 - ease_out(t));
        b.show(*label[head_now_ ^ 1], t < 1 && !head_words_[head_now_ ^ 1].empty());
        if (!head_words_[head_now_ ^ 1].empty()) b.words(*label[head_now_ ^ 1], head_words_[head_now_ ^ 1]);
    }

    void render(const NativeView& v, std::uint64_t now, float dt) {
        const bool playing = v.tab == kMedia;
        title(v.header, now);
        b.picture(header_icon_, tile_icon(v.header_icon));
        header_art(v, dt);
        for (W* w : {&cover_, &title_, &artist_, &bar_, &clock_, &duration_, &keys_[0], &keys_[1], &square_})
            b.show(*w, playing);
        for (auto& w : controls_) b.show(w, playing);
        now_playing(v, now, dt, playing);
        lists(v, now, dt, playing);
        system(v);
        settings(v, now, dt);
        if(plugin) { const auto f=plugin(card_,v,now,dt); if(v.tab==kCustom)want_=f; }
    }

    // Settings: shortcut, hold time, sounds, sidebar order, and Librespot visibility/status.
    void settings(const NativeView& v, std::uint64_t now, float dt) {
        const bool on = v.tab == kSettings, main = on && v.page == 0, side = on && v.page == 1, music = on && v.page == 2;
        for (W* w : {&set_note_, &set_track_, &set_check_, &set_knob_}) b.show(*w, main || music);
        b.show(set_chevron_, main); b.show(side_more_, main); b.show(libre_more_, main);
        for (int i = 0; i < kSettingRows; ++i) { b.show(set_label_[i], main || (music && i < 3)); b.show(set_value_[i], main || (music && i < 3)); }
        for (int i = 0; i < kSlots; ++i) { b.show(side_label_[i], side); b.show(side_icon_[i], side && v.slots[i].online); }
        b.show(side_star_, false);
        if (side) { sidebar_page(v, dt); return; }
        const bool lit = on && v.focused && !v.choosing;
        b.show(set_focus_, lit);
        pick_t_ = approach(pick_t_, on && v.choosing ? 1 : 0, dt, 0.05f);
        b.show(pick_, pick_t_ > 0);
        if (!on) { set_y_ = -9999; set_sw_ = music_sw_ = -1; return; }
        const auto& st = v.settings;
        const float row = kListTop + v.setting * kSettingH;
        set_y_ = approach(set_y_, row, dt, 0.045f, 0.05f);
        if (lit) {
            b.geometry(set_focus_, {12, set_y_, kCardW - 24, kSettingH - 6});
            want_ = {41 + v.setting, kCardX + 12, kCardY + row, kCardW - 24, kSettingH - 6, 10};
        }
        const float tx = kCardW - kPad - kSwitchW;
        float sw_y = 0;
        if (main) {
            const char* names[] = {"Open shortcut", "Hold to open", "Interface sounds", "Sidebar", "Librespot"};
            for (int i = 0; i < kSettingRows; ++i) b.words(set_label_[i], names[i]);
            // The shortcut: while recording, what is held so far, gently pulsing.
            if (v.recording) {
                b.words(set_value_[0], v.recorded ? chord_name(v.recorded) : "Press and hold...");
                b.alpha(set_value_[0], 0.6f + 0.4f * float(std::sin(now / 1e6 * 5.0) * 0.5 + 0.5));
            } else {
                b.words(set_value_[0], chord_name(st.chord));
                b.alpha(set_value_[0], 1);
            }
            char hold[16]; std::snprintf(hold, sizeof hold, "%.2g s", st.hold_ms / 1000.0);
            b.words(set_value_[1], hold);
            b.words(set_value_[2], "");
            char count[24]; std::snprintf(count, sizeof count, "%d tiles", int(v.rail.size()));
            b.words(set_value_[3], count);
            b.words(set_value_[4], v.system.librespot.empty() ? "Checking..." : v.system.librespot);
            b.words(set_note_, "");
            b.geometry(set_note_, {kPad, kListTop + 5 * kSettingH + 18, kContentW, kTimeSize});
            set_sw_ = set_sw_ < 0 ? (st.sounds ? 1.0f : 0.0f) : approach(set_sw_, st.sounds ? 1 : 0, dt, 0.05f, 0.002f);
            sw_y = kListTop + 2 * kSettingH + (kSettingH - 6) / 2;
        } else {
            b.words(set_label_[0], "Connection");
            b.words(set_label_[1], "Show music tiles");
            b.words(set_label_[2], "Stop Librespot");
            b.words(set_value_[0], v.system.librespot.empty() ? "Checking..." : v.system.librespot);
            b.alpha(set_value_[0], 1);
            b.words(set_value_[1], "");
            b.words(set_value_[2], "Stop");
            b.words(set_note_, "Cross stops librespot-ps5.elf; configure the receiver when launching it.");
            const float note_y = kListTop + 3 * kSettingH + 26;
            b.geometry(set_note_, {kPad, note_y, kContentW, kTimeSize});
            music_sw_ = music_sw_ < 0 ? (st.librespot_tiles ? 1.0f : 0.0f)
                                      : approach(music_sw_, st.librespot_tiles ? 1 : 0, dt, 0.05f, 0.002f);
            sw_y = kListTop + kSettingH + (kSettingH - 6) / 2;
        }
        // Console style switch: sounds on the main page, Librespot tiles on its page.
        const float switch_value = main ? set_sw_ : music_sw_;
        b.tint(set_track_, kWhite, 0.22f + 0.36f * switch_value);
        b.alpha(set_check_, switch_value); b.tint(set_check_, kDarkInk, 1);
        b.geometry(set_track_, {tx, sw_y - kSwitchH / 2, kSwitchW, kSwitchH});
        b.geometry(set_check_, {tx + 6, sw_y - 14, 28, 28});
        b.geometry(set_knob_, {tx + kSwitchH / 2 + (kSwitchW - kSwitchH) * switch_value - 24, sw_y - 24, 48, 48});
        // The hold-time list.
        if (pick_t_ > 0) {
            b.alpha(pick_, pick_t_);
            b.geometry(pick_, {kPickX, kPickY + (1 - ease_out(pick_t_)) * 12, kPickW, kPickH});
            pick_y_ = approach(pick_y_, 8 + v.choice * kPickRowH, dt, 0.045f, 0.05f);
            b.geometry(pick_focus_, {8, pick_y_, kPickW - 16, kPickRowH - 4});
            for (int i = 0; i < kChoices; ++i) {
                char text[16]; std::snprintf(text, sizeof text, "%.2g s", kHoldChoices[i] / 1000.0);
                b.words(pick_label_[i], text);
                b.show(pick_tick_[i], kHoldChoices[i] == st.hold_ms);
            }
            if (v.choosing) want_ = {47 + v.choice, kCardX + kPickX + 8, kCardY + kPickY + 8 + v.choice * kPickRowH, kPickW - 16, kPickRowH - 4, 10};
        } else pick_y_ = -9999;
    }

    void now_playing(const NativeView& v, std::uint64_t now, float dt, bool playing) {
        const auto& s = v.state;
        const bool buttons = playing && v.focused && !v.seek && !v.popup;
        const int key = buttons ? v.control : -1;
        if (key != focus_key_) { focus_key_ = key; focus_at_ = now; }
        focus_a_ = approach(focus_a_, buttons ? 1 : 0, dt, buttons ? 0.03f : 0.09f);
        b.show(focus_, playing && focus_a_ > 0); b.show(focus_ring_, playing && focus_a_ > 0 && !system_focus_);
        b.alpha(focus_, focus_a_);
        if (buttons) want_ = {v.control, kCardX + control_x(v.control) - kDisc / 2, kCardY + kControlsY - kDisc / 2, kDisc, kDisc, kDisc / 2};
        popups(v, dt, playing);
        // The hint: Square, Pause or Play, below the card. A new word fades in over the old.
        const bool sorting = v.tab == kSettings && v.page == 1 && v.focused && !v.picking;
        const std::string word = playing ? (s.playing ? "Pause" : "Play") : sorting ? (v.moving ? "Place" : "Move") : "";
        if (word != hint_words_[hint_now_]) { hint_now_ ^= 1; hint_words_[hint_now_] = word; hint_at_ = now; }
        const float hf = ease_out(time(now, hint_at_, kHintFadeMs));
        b.words(hint_[hint_now_], hint_words_[hint_now_]);
        b.alpha(hint_[hint_now_], hf); b.alpha(hint_[hint_now_ ^ 1], 1 - hf);
        b.show(hint_[hint_now_], !word.empty()); b.show(hint_[hint_now_ ^ 1], hf < 1 && !hint_words_[hint_now_ ^ 1].empty());
        b.show(hint_square_, !word.empty());
        if (!word.empty()) {
            const float target = kCardW - kHintInset - b.text_width(hint_[hint_now_], word) - kHintGap - kHintDisc;
            hint_left_ = approach(hint_left_, target, dt, 0.05f, 0.05f);
            b.geometry(hint_square_, {hint_left_ - kHintIcon / 2, kCardH + kHintBelow - kHintIcon / 2, kHintIcon, kHintIcon});
        }
        if (!playing) { scrub_ = 0; b.show(thumb_, false); b.show(thumb_ring_, false); b.show(sweep_, false); return; }
        b.words(title_, s.title); b.words(artist_, s.artist);
        b.picture(cover_, s.cover.empty() ? b.fallback_art() : s.cover);
        // Chosen (seek_ms), or sent and not yet heard there (seek_sent): the bar and clock hold it.
        const int position = v.seek_ms >= 0 ? v.seek_ms : v.seek_sent >= 0 ? v.seek_sent : s.position_ms;
        b.words(clock_, clock(position)); b.words(duration_, clock(s.duration_ms));
        const float done = s.duration_ms > 0 ? std::clamp(float(position) / s.duration_ms, 0.0f, 1.0f) : 0;
        // The scrub bar: focused, it thickens, its fill turns white and a ringed thumb sits at the spot.
        scrub_ = approach(scrub_, playing && v.focused && v.seek && !v.popup ? 1 : 0, dt, 0.04f);
        const float bar_h = kBarH + (kScrubBarH - kBarH) * scrub_, bar_y = (kScrubBarH - bar_h) / 2;
        b.geometry(bar_base_, {0, bar_y, kContentW, bar_h});
        b.geometry(bar_fill_, {0, bar_y, std::max(0.1f, kContentW * done), bar_h});
        b.tint(bar_fill_, {kBarFill.r + (1 - kBarFill.r) * scrub_, kBarFill.g + (1 - kBarFill.g) * scrub_, kBarFill.b + (1 - kBarFill.b) * scrub_}, 1);
        const float thumb_x = kPad + kContentW * done, bar_mid = kBarTop + kBarH / 2;
        const float ts = 0.6f + 0.4f * scrub_;
        b.show(thumb_, scrub_ > 0); b.show(thumb_ring_, scrub_ > 0 && !system_focus_);
        if (playing && v.focused && v.seek && !v.popup)
            want_ = {30, kCardX + thumb_x - kThumb / 2, kCardY + bar_mid - kThumb / 2, kThumb, kThumb, kThumb / 2};
        b.alpha(thumb_, scrub_); b.alpha(thumb_ring_, scrub_);
        b.geometry(thumb_, {thumb_x - kThumb * ts / 2, bar_mid - kThumb * ts / 2, kThumb * ts, kThumb * ts});
        b.geometry(thumb_ring_, {thumb_x - kThumbRing / 2, bar_mid - kThumbRing / 2, kThumbRing, kThumbRing});
        // Asked for and not heard yet (a play, a seek, a skip): a light runs along the bar.
        const bool waiting = v.seek_ms < 0 && (s.pending || v.seek_sent >= 0);
        b.show(sweep_, waiting);
        if (waiting) {
            const float band = kContentW * 0.3f, p = float(now / 1000 % std::uint64_t(kSweepMs)) / kSweepMs;
            b.geometry(sweep_, {-band + (kContentW + band) * p, 0, band, kScrubBarH});
        }
        b.picture(controls_[2], b.icon(s.playing ? "iconid_control_pause" : "iconid_control_play"));
        // The focused button: the white disc glides between buttons and grows in, its icon turns dark.
        if (buttons) {
            focus_x_ = approach(focus_x_, control_x(v.control), dt, 0.045f, 0.05f);
            const float grow = ease_out(time(now, focus_at_, kFocusMs)), d = kDisc * (0.86f + 0.14f * grow);
            b.geometry(focus_, {focus_x_ - d / 2, kControlsY - d / 2, d, d});
            b.geometry(focus_ring_, {focus_x_ - kRing / 2, kControlsY - kRing / 2, kRing, kRing});
            b.alpha(focus_ring_, grow * focus_a_);
        } else if (focus_a_ <= 0) focus_x_ = -9999;
        else b.alpha(focus_ring_, focus_a_);
        for (int i = 0; i < 5; ++i) {
            const bool enabled = !((i == 1 && !s.can_previous) || (i == 3 && !s.can_next));
            ink_[i] = approach(ink_[i], buttons && i == v.control ? 1 : 0, dt, buttons ? 0.03f : 0.09f);
            const float k = ink_[i];
            b.tint(controls_[i], {1 + (kDarkInk.r - 1) * k, 1 + (kDarkInk.g - 1) * k, 1 + (kDarkInk.b - 1) * k}, 1);
            b.alpha(controls_[i], enabled ? 1 : kDisabled);
        }
    }

    // Volume (a slider) and options (shuffle switch, repeat mode): the panel rises a little and
    // fades in over the bar and buttons; the slider's handle and the switch's knob glide.
    void popups(const NativeView& v, float dt, bool playing) {
        const auto& s = v.state;
        const bool open = playing && v.popup;
        if (open && popup_kind_ != v.popup) { popup_kind_ = v.popup; opt_y_ = -9999; vol_ = sw_ = -1; }
        popup_t_ = approach(popup_t_, open ? 1 : 0, dt, 0.05f);
        b.show(popup_, popup_t_ > 0);
        if (popup_t_ <= 0) { opt_y_ = -9999; vol_ = sw_ = -1; return; }
        b.alpha(popup_, popup_t_);
        b.geometry(popup_, {kPopupX, kPopupY + (1 - ease_out(popup_t_)) * 16, kPopupW, kPopupH});
        const bool volume = popup_kind_ == 1;
        for (W* w : {&vol_icon_, &vol_base_, &vol_fill_, &vol_knob_}) b.show(*w, volume);
        for (W* w : {&opt_focus_, &opt_icon_[0], &opt_icon_[1], &opt_label_[0], &opt_label_[1], &opt_value_, &sw_track_, &sw_check_, &sw_knob_})
            b.show(*w, !volume);
        if (volume) {
            b.words(popup_title_, "Volume");
            b.words(popup_value_, std::to_string(s.volume));
            b.words(popup_hint_, "");
            const float level = std::clamp(s.volume, 0, 100) / 100.0f;
            vol_ = vol_ < 0 ? level : approach(vol_, level, dt, 0.04f, 0.001f);
            const float x = kSliderX + kSliderW * vol_;
            b.geometry(vol_fill_, {kSliderX, kSliderY - 3, std::max(0.1f, kSliderW * vol_), 6});
            b.geometry(vol_knob_, {x - 24, kSliderY - 24, 48, 48});
            if (open) want_ = {36, kCardX + kPopupX + x - 24, kCardY + kPopupY + kSliderY - 24, 48, 48, 24};
            return;
        }
        b.words(popup_title_, "");
        b.words(popup_value_, "");
        b.words(popup_hint_, "");
        // The focused row: the highlight glides between them.
        const float row = kOptionTop + v.option * kOptionH;
        opt_y_ = approach(opt_y_, row, dt, 0.045f, 0.05f);
        b.geometry(opt_focus_, {12, opt_y_, kPopupW - 24, kOptionH - 6});
        if (open) want_ = {34 + v.option, kCardX + kPopupX + 12, kCardY + kPopupY + row, kPopupW - 24, kOptionH - 6, 10};
        // Shuffle: the console's switch. Off, a dim track with the knob at the left; on, a bright
        // track with the knob at the right and the check showing.
        sw_ = sw_ < 0 ? (s.shuffle ? 1.0f : 0.0f) : approach(sw_, s.shuffle ? 1 : 0, dt, 0.05f, 0.002f);
        const float cy = kOptionTop + (kOptionH - 6) / 2, tx = kPopupW - kPad - kSwitchW;
        b.geometry(sw_track_, {tx, cy - kSwitchH / 2, kSwitchW, kSwitchH});
        b.tint(sw_track_, kWhite, 0.22f + 0.36f * sw_);
        b.geometry(sw_check_, {tx + 6, cy - 14, 28, 28});
        b.alpha(sw_check_, sw_); b.tint(sw_check_, kDarkInk, 1);
        const float kx = tx + kSwitchH / 2 + (kSwitchW - kSwitchH) * sw_;
        b.geometry(sw_knob_, {kx - 24, cy - 24, 48, 48});
        // Repeat: its icon and the mode, which Cross steps through.
        b.picture(opt_icon_[1], b.icon(s.repeat == Repeat::one ? "iconid_control_repeat_single" : "iconid_control_repeat"));
        b.alpha(opt_icon_[0], s.shuffle ? 1 : 0.6f); b.alpha(opt_icon_[1], s.repeat == Repeat::off ? 0.6f : 1);
        b.geometry(opt_value_, text_box(kPopupW - kPad - 160, kOptionTop + kOptionH + (kOptionH - 6) / 2 + 9, 160, 24));
        b.words(opt_value_, s.repeat == Repeat::off ? "Off" : s.repeat == Repeat::all ? "All" : "One");
    }

    // The sidebar page: its seven slots (icon and name, or Empty); the cursor's row lights, and a
    // tile being moved lights more. Cross opens the tile list beside the slot.
    void sidebar_page(const NativeView& v, float dt) {
        for (auto it = side_tile_y_.begin(); it != side_tile_y_.end();) {
            const bool placed = std::any_of(std::begin(v.slots), std::end(v.slots), [&](const TileView& t) { return t.online && t.id == it->first; });
            if (!placed) { side_tile_a_.erase(it->first); it = side_tile_y_.erase(it); } else ++it;
        }
        float row_y[kSlots];
        for (int i = 0, empty = 0; i < kSlots; ++i) {
            const auto& t = v.slots[i];
            const float target = kListTop + i * kSideH;
            float& y = t.online ? side_tile_y_.try_emplace(t.id, -9999.0f).first->second : side_empty_y_[empty++];
            y = y < -9000 ? target : approach(y, target, dt, 0.045f, 0.05f);
            row_y[i] = y;
            float a = 0.45f;
            if (t.online) { float& ta = side_tile_a_[t.id]; ta = approach(ta, 1, dt, 0.06f); a = ta; }
            b.words(side_label_[i], t.online ? t.title : "Empty");
            b.tint(side_icon_[i], kWhite, 1);
            if (t.online) b.picture(side_icon_[i], tile_icon(t.icon));
            b.alpha(side_label_[i], a); b.alpha(side_icon_[i], a);
            b.geometry(side_icon_[i], {kPad, y + (kSideH - 6) / 2 - 16, 32, 32});
            b.geometry(side_label_[i], text_box(kPad + 52, y + (kSideH - 6) / 2 + 9, 360, 24));
        }
        // The first tile shown opens with the panel: a small star after its name.
        int first = 0;
        while (first < kSlots && !v.slots[first].online) ++first;
        if (first < kSlots) {
            const float y = row_y[first] + (kSideH - 6) / 2 - 11;
            const float x = kPad + 52 + b.text_width(side_label_[first], v.slots[first].title) + 10;
            b.show(side_star_, true); b.tint(side_star_, kSecondary, 1);
            b.geometry(side_star_, {x, y, 22, 22});
        }
        const bool lit = v.focused && !v.picking;
        b.show(set_focus_, lit);
        const float row = kListTop + v.slot * kSideH;
        set_y_ = approach(set_y_, row, dt, 0.045f, 0.05f);
        if (lit) {
            b.geometry(set_focus_, {12, set_y_, kCardW - 24, kSideH - 6});
            b.tint(set_focus_, kWhite, v.moving ? 70 / 255.f : 34 / 255.f);
            want_ = {41 + v.slot, kCardX + 12, kCardY + row, kCardW - 24, kSideH - 6, 10};
        }
        pick_t_ = approach(pick_t_, v.picking ? 1 : 0, dt, 0.05f);
        b.show(pick_, pick_t_ > 0);
        if (pick_t_ <= 0) { pick_y_ = -9999; return; }
        const float py = std::min(row + kSideH - 6, kCardH - kPickH - 12);
        b.alpha(pick_, pick_t_);
        b.geometry(pick_, {kPickX, py + (1 - ease_out(pick_t_)) * 12, kPickW, kPickH});
        const int entries = int(v.picks.size()) + 1;
        const int first_row = std::clamp(v.pick - (kChoices - 1), 0, std::max(0, entries - kChoices));
        const int row_at = v.pick - first_row;
        pick_y_ = approach(pick_y_, 8 + row_at * kPickRowH, dt, 0.045f, 0.05f);
        b.geometry(pick_focus_, {8, pick_y_, kPickW - 16, kPickRowH - 4});
        for (int i = 0; i < kChoices; ++i) {
            const int e = first_row + i;
            const bool none = e == entries - 1;
            b.show(pick_label_[i], e < entries);
            b.words(pick_label_[i], none ? "None" : e < entries - 1 ? v.picks[std::size_t(e)].title : "");
            b.show(pick_tick_[i], e < entries && (none ? !v.slots[v.slot].online : v.picks[std::size_t(e)].id == v.slots[v.slot].id));
        }
        if (v.picking) want_ = {47 + row_at, kCardX + kPickX + 8, kCardY + py + 8 + row_at * kPickRowH, kPickW - 16, kPickRowH - 4, 10};
    }

    static float stat_baseline(int i) {
        return i < 4 ? kListTop + 64 + i * kStatH : kSysConsole + 46 + (i - 4) * kInfoH;
    }

    // System: the console's readings, unavailable ones as a dash.
    void system(const NativeView& v) {
        const bool on = v.tab == kStats;
        for (auto& w : sys_heading_) b.show(w, on);
        for (auto& w : sys_label_) b.show(w, on);
        for (auto& w : sys_value_) b.show(w, on);
        for (auto& w : sys_base_) b.show(w, on);
        for (auto& w : sys_fill_) b.show(w, on);
        b.show(sys_sub_, on);
        if (!on) return;
        const auto& st = v.system;
        const auto number = [](float value, const char* format) {
            if (value < 0) return std::string("\xE2\x80\x94");
            char text[48]; std::snprintf(text, sizeof text, format, value); return std::string(text);
        };
        b.words(sys_value_[0], number(st.cpu, "%.0f%%"));
        b.words(sys_value_[1], st.mem_used_gb < 0 || st.mem_total_gb <= 0 ? std::string("\xE2\x80\x94")
                : number(st.mem_used_gb, "%.1f") + " / " + number(st.mem_total_gb, "%.1f GB"));
        b.words(sys_value_[2], number(st.fps, "%.0f fps"));
        b.words(sys_sub_, st.fps < 0 ? "" : "Average over 5 seconds, lowest " + number(st.fps_min, "%.0f"));
        b.words(sys_value_[3], st.cpu_temp < 0 && st.soc_temp < 0 ? std::string("\xE2\x80\x94")
                : "CPU " + number(st.cpu_temp, "%.0f\xC2\xB0" "C") + ", SoC " + number(st.soc_temp, "%.0f\xC2\xB0" "C"));
        const float fill[] = {st.cpu < 0 ? 0 : std::min(st.cpu, 100.0f) / 100,
                              st.mem_total_gb > 0 && st.mem_used_gb >= 0 ? std::min(1.0f, st.mem_used_gb / st.mem_total_gb) : 0};
        for (int i = 0; i < 2; ++i) b.geometry(sys_fill_[i], {kPad, stat_baseline(i) + 16, std::max(0.1f, kContentW * fill[i]), 4});
        char uptime[32] = "\xE2\x80\x94";
        if (st.uptime_s >= 0) std::snprintf(uptime, sizeof uptime, "%d h %02d min", st.uptime_s / 3600, st.uptime_s / 60 % 60);
        b.words(sys_value_[4], st.firmware.empty() ? "\xE2\x80\x94" : st.firmware);
        b.words(sys_value_[5], uptime);
        b.words(sys_value_[6], st.address.empty() ? "\xE2\x80\x94" : st.address);
        b.words(sys_value_[7], st.librespot.empty() ? "\xE2\x80\x94" : st.librespot);
    }

    // Placeholder rows while a list loads: tiles and bars of varied lengths, with a soft light
    // sweeping across each, one row after another (the queue's buffering light, made wide).
    void skeleton(std::uint64_t now, bool covers, float alpha) {
        b.show(skel_, alpha > 0);
        if (alpha <= 0) return;
        b.alpha(skel_, alpha);
        static constexpr float title_w[] = {0.62f, 0.48f, 0.70f, 0.55f, 0.66f, 0.50f, 0.60f, 0.58f};
        static constexpr float artist_w[] = {0.38f, 0.30f, 0.44f, 0.34f, 0.40f, 0.28f, 0.36f, 0.32f};
        const int visible = std::min(kRows - 1, int((kCardH - kListTop - kPad) / kRowH));
        b.geometry(skel_, {0, kListTop, kCardW, visible * kRowH});
        const float tx = covers ? kPad + kRowCover + 18 : kPad, tw = kCardW - kPad - tx;
        for (int i = 0; i < kRows; ++i) {
            const bool on = i < visible;
            for (W* w : {&skel_title_[i], &skel_artist_[i], &skel_sweep_[i]}) b.show(*w, on);
            b.show(skel_tile_[i], on && covers);
            if (!on) continue;
            const float y = i * kRowH;
            b.geometry(skel_tile_[i], {kPad, y + (kRowH - kRowCover) / 2, kRowCover, kRowCover});
            b.geometry(skel_title_[i], {tx, y + 21, tw * title_w[i], 14});
            b.geometry(skel_artist_[i], {tx, y + 55, tw * artist_w[i], 11});
            const float period = 1400, p = float((now / 1000 + std::uint64_t(i) * 110) % std::uint64_t(period)) / period;
            b.geometry(skel_sweep_[i], {-220 + (kCardW + 220) * p, y, 220, kRowH});
        }
    }

    void lists(const NativeView& v, std::uint64_t now, float dt, bool playing) {
        struct Row { std::string title, artist, cover; };
        std::vector<Row> rows;
        int selected = 0;
        bool covers = true;
        std::string first, second, heading;
        bool loading = false;
        if (v.tab == kList) {
            const auto& l = v.list;
            selected = l.selected; covers = l.covers; loading = l.loading;
            for (const auto& r : l.rows) rows.push_back({r.title, r.subtitle, r.art});
            if (!loading && rows.empty()) { first = l.empty; second = l.empty_detail; }
            else if (!rows.empty()) heading = l.heading;
        }
        // An empty list's words, and the rows themselves, fade in; so does the skeleton while loading.
        loading = loading && !playing;
        msg_a_ = msg_a_ < 0 ? (first.empty() ? 0.0f : 1.0f) : approach(msg_a_, first.empty() || playing ? 0 : 1, dt, 0.07f);
        if (!first.empty()) { b.words(message_, first); b.words(message2_, second); }
        // The last words stay while they fade out.
        b.show(message_, msg_a_ > 0); b.alpha(message_, msg_a_);
        b.show(message2_, msg_a_ > 0); b.alpha(message2_, msg_a_);
        skel_a_ = skel_a_ < 0 ? (loading ? 1.0f : 0.0f) : approach(skel_a_, loading ? 1 : 0, dt, 0.08f);
        skeleton(now, covers, skel_a_);
        b.show(list_heading_, !playing && rows.size() && !heading.empty()); b.words(list_heading_, heading);
        const float top = kListTop + (heading.empty() ? 0 : 36);
        const int visible = std::min(kRows - 1, int((kCardH - top - kPad) / kRowH));
        const int count = int(rows.size());
        selected = std::clamp(selected, 0, std::max(0, count - 1));
        const int at = std::clamp(selected - visible / 2, 0, std::max(0, count - visible));
        const bool shown = !playing && first.empty() && count > 0;
        const bool lit = shown && v.focused;
        // A new list (another tab or level) starts in place and fades in; within one, the rows
        // scroll and the selection glides.
        const std::string key = v.tab == kList ? v.list.key : std::to_string(v.tab);
        // Another list (opening a playlist, going back): the old rows fade out as they are (the
        // skeleton fading in over them), then the new list takes their place and fades in.
        if (key != list_key_ && list_key_ != "-" && rows_a_ > 0.01f && !rows_out_) rows_out_ = true;
        if (rows_out_) {
            rows_a_ = approach(rows_a_, 0, dt, 0.05f, 0.01f);
            b.alpha(list_, rows_a_);
            b.show(row_focus_, false);
            if (rows_a_ > 0) return;
            rows_out_ = false;
        }
        if (key != list_key_ || scroll_ < -9000) { list_key_ = key; scroll_ = float(at); sel_ = float(selected); }
        rows_a_ = rows_a_ < 0 ? (shown ? 1.0f : 0.0f) : approach(rows_a_, shown ? 1 : 0, dt, 0.08f);
        b.alpha(list_, rows_a_);
        scroll_ = approach(scroll_, float(at), dt, 0.07f, 0.002f);
        sel_ = approach(sel_, float(selected), dt, 0.045f, 0.002f);
        b.show(list_, shown && rows_a_ > 0);
        b.geometry(list_, {0, top, kCardW, visible * kRowH});
        b.show(row_focus_, lit);
        if (lit) {
            b.geometry(row_focus_, {12, (sel_ - scroll_) * kRowH + 3, kCardW - 24, kRowH - 6});
            want_ = {20 + selected % kRows, kCardX + 12, kCardY + top + (selected - at) * kRowH + 3, kCardW - 24, kRowH - 6, 10};
        }
        const int base = std::max(0, int(std::floor(scroll_)));
        bool used[kRows] = {};
        bool starting = false;
        for (int index = base; shown && index < std::min(count, base + visible + 1); ++index) {
            const int i = index % kRows;
            used[i] = true;
            const auto& row = rows[index];
            const float y = (index - scroll_) * kRowH, tx = covers ? kPad + kRowCover + 18 : kPad, tw = kCardW - kPad - tx;
            b.show(row_title_[i], true); b.show(row_artist_[i], true);
            b.show(row_tile_[i], covers); b.show(row_cover_[i], covers);
            // Title and artist as one block centred on the cover (y+14..y+74): the title's capitals
            // start level with its top, the artist's baseline sits just above its bottom.
            b.geometry(row_title_[i], text_box(tx, y + kRowTitle, tw, 24)); b.words(row_title_[i], row.title);
            b.geometry(row_artist_[i], text_box(tx, y + kRowArtist, tw, 20)); b.words(row_artist_[i], row.artist);
            const float cy = y + (kRowH - kRowCover) / 2;
            b.geometry(row_tile_[i], {kPad, cy, kRowCover, kRowCover});
            if (covers && row.cover.empty()) {
                b.geometry(row_cover_[i], {kPad + 14, cy + 14, kRowCover - 28, kRowCover - 28});
                b.picture(row_cover_[i], b.icon("iconid_music")); b.tint(row_cover_[i], kSecondary, 1);
            } else if (covers) {
                b.geometry(row_cover_[i], {kPad, cy, kRowCover, kRowCover});
                b.picture(row_cover_[i], row.cover); b.tint(row_cover_[i], kWhite, 1);
            }
            if (index == v.list.busy) {
                starting = true;
                const float p = float(now / 1000 % std::uint64_t(kSweepMs)) / kSweepMs;
                b.geometry(row_sweep_, {18 + p * (kCardW - 36 - 140), y + kRowH - 6, 140, 3});
            }
        }
        for (int i = 0; i < kRows; ++i)
            if (!used[i]) {
                b.show(row_title_[i], false); b.show(row_artist_[i], false);
                b.show(row_tile_[i], false); b.show(row_cover_[i], false);
                if (!shown) b.picture(row_cover_[i], "");
            }
        b.show(row_sweep_, starting);
    }
};

} // namespace overlay::native
