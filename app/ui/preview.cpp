// SPDX-License-Identifier: GPL-3.0-or-later
#include "preview.hpp"

#include "../song_controller.hpp" // kGradeShowNs
#include "../ui.hpp"              // display_px_per_mm
#include "toonval/midi_event.hpp"
#include "toonval/screen_layout.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace toonval::app::ui {

namespace {

// One pass of the loop, in seconds: a rising C major line, then the chord, then a rest.
struct Step {
    double at;
    std::uint8_t note;
    std::uint8_t velocity; // 0 releases
};
constexpr Step kLoop[] = {
    {0.00, 60, 92}, {0.62, 60, 0},   {0.40, 64, 96}, {1.02, 64, 0},  {0.80, 67, 100},
    {1.42, 67, 0},  {1.20, 72, 108}, {1.90, 72, 0},  {2.10, 60, 88}, {2.10, 64, 84},
    {2.10, 67, 90}, {3.20, 60, 0},   {3.20, 64, 0},  {3.20, 67, 0},
};
constexpr double kLoopS = 4.0;
// The grade words: one every kGradeEveryS in the order Perfect, Good, Early, Late, Miss, each on
// a key the loop plays and shown for as long as in play (kGradeShowNs), so two or three overlap
// in time and the placement has something to do.
constexpr double kGradeEveryS = 0.5;
constexpr std::uint8_t kGradeKeys[5] = {64, 67, 62, 65, 69};
constexpr NoteGrade kGradeOrder[5] = {NoteGrade::Perfect, NoteGrade::Good, NoteGrade::Early,
                                      NoteGrade::Late, NoteGrade::Missed};
constexpr double kGradeCycleS = kGradeEveryS * 6.0; // five words and a rest
constexpr float kAspect = 6.0f / 16.0f;
constexpr float kShortAspect = 5.0f / 16.0f; // on a short screen, so the page still fits

} // namespace

LivePreview::LivePreview(SDL_Renderer* renderer) : renderer_(renderer) {
    last_press_.fill(kNeverPressed);
    KeyViewFollower follower;
    follower.snap(layout_.range(), layout_.range());
    view_ = follower.frame();
}

LivePreview::~LivePreview() {
    if (target_ != nullptr) {
        SDL_DestroyTexture(target_);
    }
}

void LivePreview::reset(std::int64_t now_ns) {
    const KeyTransition released = keys_.clear();
    trail_.apply(released, now_ns);
    last_press_.fill(kNeverPressed);
    last_t_ = -1.0;
}

void LivePreview::feed(double t, std::int64_t now_ns) {
    const double phase = std::fmod(t, kLoopS);
    const double prev = last_t_ < 0.0 ? -1.0 : std::fmod(last_t_, kLoopS);
    const bool wrapped = last_t_ >= 0.0 && phase < prev;
    for (const Step& s : kLoop) {
        const bool due = wrapped ? (s.at > prev || s.at <= phase) : (s.at > prev && s.at <= phase);
        if (!due) {
            continue;
        }
        MidiEvent ev;
        ev.time_ns = now_ns;
        ev.type = s.velocity > 0 ? MidiEventType::NoteOn : MidiEventType::NoteOff;
        ev.data1 = s.note;
        ev.data2 = s.velocity;
        const KeyTransition tr = keys_.apply(ev);
        trail_.apply(tr, now_ns);
        if (tr.type == KeyTransitionType::Pressed) {
            last_press_[tr.note] = now_ns;
            last_velocity_[tr.note] = tr.velocity;
        }
    }
    last_t_ = t;
}

void LivePreview::feed_grades(double t) {
    const double show_s = static_cast<double>(kGradeShowNs) * 1e-9;
    const double phase = std::fmod(t, kGradeCycleS);
    grade_count_ = 0;
    for (std::size_t i = 0; i < 5; ++i) {
        const double age_s = phase - kGradeEveryS * static_cast<double>(i);
        if (age_s >= 0.0 && age_s < show_s) {
            grades_[grade_count_++] = {kGradeKeys[i], kGradeOrder[i],
                                       static_cast<float>(age_s / show_s)};
        }
    }
}

void LivePreview::draw(Kit& k, const LookSettings& look, float width) {
    const float aspect = k.short_screen ? kShortAspect : kAspect;
    const float height = std::round(width * aspect);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy({width, height});
    if (!ImGui::IsItemVisible()) {
        return;
    }
    const auto now_ns = static_cast<std::int64_t>(k.now * 1e9);
    if (start_ < 0.0) {
        start_ = k.now;
    }
    if (last_drawn_ >= 0.0 && k.now - last_drawn_ > 0.5) {
        // The page was away: start the loop again rather than replay what was missed.
        reset(now_ns);
        start_ = k.now;
    }
    last_drawn_ = k.now;

    // The target is the image's size in output pixels plus the rows the scene keeps above its
    // note area (the top row, a chord row), which the image leaves out.
    const int w = std::max(16, static_cast<int>(std::lround(width * k.fb_scale)));
    const float visible = std::round(static_cast<float>(w) * aspect);
    const float ppmm = display_px_per_mm(SDL_GetRenderWindow(renderer_));
    float total = visible;
    ScreenLayout lay;
    for (int i = 0; i < 4; ++i) {
        LookSettings sized = look;
        sized.keyboard_height = kMaxKeyboardHeight;
        lay = compute_layout(static_cast<float>(w), total, ppmm, sized);
        total = visible + lay.note_area.y;
    }
    const int h = static_cast<int>(total);
    if (target_ == nullptr || w != target_w_ || h != target_h_) {
        if (target_ != nullptr) {
            SDL_DestroyTexture(target_);
        }
        target_ =
            SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, w, h);
        target_w_ = w;
        target_h_ = h;
    }
    if (target_ == nullptr) {
        return;
    }
    // The look as set, but with fewer seconds on screen and taller keys: in a strip this small
    // the notes would otherwise hardly leave the keys. Style, colours and names are untouched.
    LookSettings shown = look;
    shown.visible_seconds = std::min(look.visible_seconds, 1.6f);
    shown.keyboard_height = kMaxKeyboardHeight;
    if (!scene_) {
        scene_ = std::make_unique<Scene>(renderer_, shown);
    } else if (!(scene_->settings() == shown)) {
        scene_->set_settings(shown);
    }

    feed(k.now - start_, now_ns);
    feed_grades(k.now - start_);
    const std::int64_t span = scene_->trail_span_ns();
    trail_.prune(now_ns, span);
    trail_.visible(now_ns, span, bars_);

    SDL_Texture* previous = SDL_GetRenderTarget(renderer_);
    SDL_SetRenderTarget(renderer_, target_);
    FrameInput frame{layout_,        view_,  keys_,  bars_, last_press_,
                     last_velocity_, now_ns, chord_, false, nullptr};
    frame.grades = std::span<const GradePopup>(grades_.data(), grade_count_);
    scene_->draw(frame);
    SDL_SetRenderTarget(renderer_, previous);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = k.s(tok::kRadiusMd);
    const float crop = lay.note_area.y / total;
    const ImTextureRef tex(static_cast<ImTextureID>(reinterpret_cast<std::intptr_t>(target_)));
    dl->AddImageRounded(tex, pos, {pos.x + width, pos.y + height}, {0.0f, crop}, {1.0f, 1.0f},
                        ImGui::GetColorU32(ImVec4(1, 1, 1, 1)), r);
    dl->AddRect(pos, {pos.x + width, pos.y + height}, u32(col::kHairline), r, 0, 1.0f);
}

} // namespace toonval::app::ui
