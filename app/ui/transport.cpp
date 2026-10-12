// SPDX-License-Identifier: GPL-3.0-or-later
#include "transport.hpp"

#include "icons.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace toonval::app::ui {

namespace {

constexpr float kHoldRepeatS = 0.35f; // a held skip repeats its step this often
constexpr float kPi = 3.14159265f;

std::string clock_text(double s) {
    const int total = static_cast<int>(std::max(0.0, s));
    char buf[16];
    std::snprintf(buf, sizeof buf, "%d:%02d", total / 60, total % 60);
    return buf;
}

// A skip button: the press moves one step at once; held past the long-press time it repeats the
// step every kHoldRepeatS until let go.
void skip_button(Kit& k, SongPort& song, bool forward, ImVec2 centre, ImVec2 hit, float drawn,
                 bool live, bool caption) {
    const SongView& v = song.view();
    const int seconds = forward ? v.skip_forward_s : v.skip_back_s;
    ImGui::SetCursorScreenPos({centre.x - hit.x * 0.5f, centre.y - hit.y * 0.5f});
    ImGui::InvisibleButton(forward ? "##fwd" : "##back", hit);
    const bool held = ImGui::IsItemActive() && live;
    const bool hover = live && !k.touch && ImGui::IsItemHovered();
    char label[48];
    std::snprintf(label, sizeof label, "%s %d seconds", forward ? "Forward" : "Back", seconds);
    if (live && !caption && held && ImGui::GetIO().MouseDownDuration[0] > tok::kLongPress) {
        k.tip = label;
        k.tip_min = ImGui::GetItemRectMin();
        k.tip_max = ImGui::GetItemRectMax();
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cap_h = caption ? k.s(14.0f) : 0.0f;
    const ImVec2 ic{centre.x, centre.y - cap_h * 0.5f};
    const float half = drawn * 0.5f;
    if (held || hover) {
        dl->AddRectFilled({ic.x - half, ic.y - half}, {ic.x + half, ic.y + half},
                          u32(held ? col::kFillPressed : col::kFill), k.s(tok::kRadiusMd));
    }
    skip_icon(k, dl, ic, k.s(tok::kIcon), seconds, forward, held ? col::kText : col::kText2);
    if (caption) {
        char text[16];
        std::snprintf(text, sizeof text, "%s%d s", forward ? "+" : "\xE2\x88\x92", seconds);
        const ImVec2 ts = text_size(k.regular, k.s(tok::kCaption), text);
        text_at(k, dl, k.regular, k.s(tok::kCaption), {centre.x - ts.x * 0.5f, ic.y + half},
                col::kText2, text);
    }
    if (!live) {
        return;
    }
    if (ImGui::IsItemActivated()) {
        forward ? song.skip_forward(k.now_ns) : song.skip_back(k.now_ns);
    }
    // The repeats: one each time the hold crosses the long-press time plus a whole number of
    // repeat periods.
    if (held) {
        const ImGuiIO& io = ImGui::GetIO();
        const float now = io.MouseDownDuration[0];
        const float before = io.MouseDownDurationPrev[0];
        const auto repeats = [](float t) {
            return t < tok::kLongPress ? -1
                                       : static_cast<int>((t - tok::kLongPress) / kHoldRepeatS);
        };
        for (int r = repeats(before) + 1; r <= repeats(now); ++r) {
            forward ? song.skip_forward(k.now_ns) : song.skip_back(k.now_ns);
        }
    }
}

} // namespace

void skip_icon(const Kit& k, ImDrawList* dl, ImVec2 centre, float size, int seconds, bool forward,
               ImVec4 colour) {
    const float r = size * 0.47f;
    const float stroke = std::max(1.5f, size * 0.08f);
    const ImU32 ink = u32(colour);
    // The arc runs 300 degrees and ends at 12 o'clock, where the arrowhead points the way it
    // turns: back from 10 o'clock anticlockwise, forward from 2 o'clock clockwise (y is down, so
    // clockwise is increasing angle).
    const float top = -0.5f * kPi;
    const float sweep = 300.0f / 180.0f * kPi;
    const float dir = forward ? 1.0f : -1.0f;
    const float end = top - dir * 0.10f; // stop short of the head so the stroke does not poke out
    dl->PathArcTo(centre, r, end - dir * sweep, end, 40);
    dl->PathStroke(ink, ImDrawFlags_None, stroke);
    // The head: a solid triangle on the circle at 12 o'clock, pointing along the turn.
    const float head = size * 0.17f;
    const ImVec2 at{centre.x, centre.y - r};
    dl->AddTriangleFilled({at.x + dir * head * 1.05f, at.y},
                          {at.x - dir * head * 0.35f, at.y - head},
                          {at.x - dir * head * 0.35f, at.y + head}, ink);
    // The step's seconds in the middle, bold, sized so two digits fit inside the ring.
    const std::string text = std::to_string(seconds);
    const float font = size * 0.56f;
    const ImVec2 ts = text_size(k.bold, font, text.c_str());
    const float max_w = (r - stroke) * 1.7f;
    const float fs = ts.x > max_w ? font * max_w / ts.x : font;
    const ImVec2 fts = text_size(k.bold, fs, text.c_str());
    // Digits sit a little high in their line box; nudge down to centre them by eye.
    dl->AddText(
        k.bold, fs,
        {std::round(centre.x - fts.x * 0.5f), std::round(centre.y - fts.y * 0.5f + fs * 0.04f)},
        ink, text.c_str());
}

float transport_width(float cell) { return cell * 5.0f; }

void transport(Kit& k, SongPort& song, float x, float cy, float cell, float hit_h, float drawn,
               bool live, bool prominent, bool captions) {
    const SongView& v = song.view();
    const ImVec2 hit{cell, hit_h};
    if (icon_toggle(k, "##replay", icon::kReplay, captions ? nullptr : "Replay",
                    {x + cell * 0.5f, cy}, hit, drawn, false, live,
                    captions ? "Replay" : nullptr)) {
        song.replay(k.now_ns);
    }
    x += cell;
    skip_button(k, song, false, {x + cell * 0.5f, cy}, hit, drawn, live, captions);
    x += cell;
    {
        const ImVec2 c{x + cell * 0.5f, cy};
        const char* label = v.playing ? "Pause" : "Play";
        if (prominent) {
            ImGui::SetCursorScreenPos({c.x - cell * 0.5f, c.y - hit_h * 0.5f});
            const bool pressed = ImGui::InvisibleButton("##play", hit);
            const bool held = ImGui::IsItemActive() && live;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float r = drawn * 0.5f * (held ? 0.94f : 1.0f);
            dl->AddCircleFilled(
                c, r, u32(held ? mix(col::kAccent, {1, 1, 1, 1}, 0.2f) : col::kAccent), 40);
            // The play triangle sits a little right of centre to look centred.
            glyph(k, dl, v.playing ? icon::kPause : icon::kPlay,
                  {c.x + (v.playing ? 0.0f : k.s(1.5f)), c.y}, k.s(tok::kIcon), col::kAccentInk);
            if (pressed && live) {
                song.toggle_play(k.now_ns);
            }
        } else if (icon_toggle(k, "##play", v.playing ? icon::kPause : icon::kPlay,
                               captions ? nullptr : label, c, hit, drawn, false, live,
                               captions ? label : nullptr)) {
            song.toggle_play(k.now_ns);
        }
    }
    x += cell;
    skip_button(k, song, true, {x + cell * 0.5f, cy}, hit, drawn, live, captions);
    x += cell;
    // The song's sound: mute (the default), the other hand, both hands.
    static constexpr const char* kSoundIcon[] = {icon::kMute, icon::kSoundOther, icon::kSoundBoth};
    static constexpr const char* kSoundLabel[] = {"Song muted", "Song plays the other hand",
                                                  "Song plays both hands"};
    static constexpr const char* kSoundCaption[] = {"Muted", "Other", "Both"};
    const auto si = static_cast<std::size_t>(v.sound);
    if (icon_toggle(k, "##sound", kSoundIcon[si], captions ? nullptr : kSoundLabel[si],
                    {x + cell * 0.5f, cy}, hit, drawn, v.sound != SongSound::Mute, live,
                    captions ? kSoundCaption[si] : nullptr)) {
        const auto next = static_cast<SongSound>((si + 1) % 3);
        song.set_sound(next, k.now_ns);
        static constexpr const char* kSaid[] = {"Song sound off", "Song plays the other hand",
                                                "Song plays both hands"};
        k.message = kSaid[static_cast<std::size_t>(next)];
    }
}

float scrubber_height(const Kit& k) { return k.s(tok::kHit) - k.s(8.0f); }

bool scrubber(Kit& k, SongPort& song, ImVec2 min, ImVec2 max, bool live, bool show_title) {
    const SongView& v = song.view();
    if (!v.has_song || v.duration_s <= 0.0) {
        return false;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cy = (min.y + max.y) * 0.5f;
    const float cap = k.s(tok::kCaption);
    float x0 = min.x + k.s(tok::kS3);
    if (show_title) {
        const float title_w = std::min((max.x - min.x) * 0.26f, k.s(200.0f));
        const std::string t = fit(k.bold, cap, v.title, title_w);
        const ImVec2 ts = text_size(k.bold, cap, t.c_str());
        text_at(k, dl, k.bold, cap, {x0, cy - ts.y * 0.5f}, col::kText, t.c_str());
        x0 += title_w + k.s(tok::kS3);
    }
    const std::string times = clock_text(v.position_s) + " / " + clock_text(v.duration_s);
    const ImVec2 tts = text_size(k.regular, cap, times.c_str());
    const float x1 = max.x - k.s(tok::kS3) - tts.x - k.s(tok::kS3);
    text_at(k, dl, k.regular, cap, {max.x - k.s(tok::kS3) - tts.x, cy - tts.y * 0.5f}, col::kText2,
            times.c_str());
    if (x1 - x0 < k.s(60.0f)) {
        return false;
    }
    // The hit area is the whole row, so a finger need not find the thin track.
    ImGui::SetCursorScreenPos({x0 - k.s(8.0f), min.y});
    ImGui::InvisibleButton("##scrub", {x1 - x0 + k.s(16.0f), max.y - min.y});
    const bool active = ImGui::IsItemActive() && live;
    if (active) {
        k.slider_active = true;
        const double t = std::clamp((ImGui::GetIO().MousePos.x - x0) / (x1 - x0), 0.0f, 1.0f);
        const double to = t * v.duration_s;
        if (std::fabs(to - v.position_s) > 1e-3) {
            song.seek(to, k.now_ns);
        }
    }
    const auto at = [&](double s) {
        return x0 + static_cast<float>(std::clamp(s / v.duration_s, 0.0, 1.0)) * (x1 - x0);
    };
    const float th = k.s(4.0f);
    // Bar lines every four bars, faint, so the overview reads as music, not a progress bar.
    for (std::size_t b = 4; b < v.bar_starts_s.size(); b += 4) {
        const float bx = at(v.bar_starts_s[b]);
        dl->AddLine({bx, cy - k.s(5.0f)}, {bx, cy + k.s(5.0f)},
                    u32({1, 1, 1, b % 16 == 0 ? 0.16f : 0.08f}), 1.0f);
    }
    if (v.loop_enabled && v.loop_end_s > v.loop_start_s) {
        dl->AddRectFilled({at(v.loop_start_s), cy - k.s(7.0f)}, {at(v.loop_end_s), cy + k.s(7.0f)},
                          u32(with_alpha(col::kAccent, 0.16f)), k.s(3.0f));
    }
    dl->AddRectFilled({x0, cy - th * 0.5f}, {x1, cy + th * 0.5f}, u32({1, 1, 1, 0.14f}), th);
    const float px = at(v.position_s);
    dl->AddRectFilled({x0, cy - th * 0.5f}, {px, cy + th * 0.5f}, u32(col::kAccent), th);
    const float r = k.s(active ? 8.0f : 6.5f);
    dl->AddCircleFilled({px, cy + k.s(1.0f)}, r + k.s(1.0f), u32({0, 0, 0, 0.3f}), 24);
    dl->AddCircleFilled({px, cy}, r, u32({0.96f, 0.97f, 1.0f, 1.0f}), 24);
    return active;
}

void countdown_overlay(Kit& k, const SongView& song, ImVec2 min, ImVec2 max) {
    const double left = song.countdown_left_s;
    if (left <= 0.0) {
        return;
    }
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 c{(min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f};
    const int number = static_cast<int>(std::ceil(left));
    // Within the current second: 0 as the number appears, 1 as it ends.
    const auto in_second = static_cast<float>(1.0 - (left - std::floor(left - 1e-9)));
    const float appear =
        k.reduced_motion ? 1.0f : ease_out(std::min(1.0f, in_second / tok::kSheet));
    // As big as the room allows, up to the design size: a short window keeps it inside.
    const float r = std::min(k.s(78.0f), std::max(0.0f, (max.y - min.y) * 0.5f - k.s(14.0f)));
    if (r < k.s(24.0f)) {
        return;
    }
    const float scale = r / k.s(78.0f);
    // A dark disc with a soft edge, so the number reads over any notes.
    card(k, dl, {c.x - r - k.s(10.0f), c.y - r - k.s(10.0f)},
         {c.x + r + k.s(10.0f), c.y + r + k.s(10.0f)}, r + k.s(10.0f),
         with_alpha(col::kSurface, 0.97f));
    // The ring empties over the whole countdown, smoothly; nothing ticks visually but the number.
    const float ring = k.s(5.0f) * scale;
    dl->AddCircle(c, r - ring, u32({1, 1, 1, 0.08f}), 64, ring);
    const float frac = static_cast<float>(left / std::max(1, song.countdown_total_s));
    const float a0 = -0.5f * kPi;
    dl->PathArcTo(c, r - ring, a0, a0 + 2.0f * kPi * std::min(1.0f, frac), 64);
    dl->PathStroke(u32(col::kAccent), 0, ring);
    // The number fades in and settles from a little larger.
    const float size = k.s(86.0f) * scale * (1.10f - 0.10f * appear);
    const std::string text = std::to_string(number);
    const ImVec2 ts = text_size(k.bold, size, text.c_str());
    dl->AddText(k.bold, size, {std::round(c.x - ts.x * 0.5f), std::round(c.y - ts.y * 0.52f)},
                u32(with_alpha(col::kText, 0.35f + 0.65f * appear)), text.c_str());
}

} // namespace toonval::app::ui
