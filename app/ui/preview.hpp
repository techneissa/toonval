// SPDX-License-Identifier: GPL-3.0-or-later
// The live look preview at the top of the Look and Quick pages. A second Scene, the one the
// main screen draws with, renders a short loop (a rising line, then a chord, the keys lighting)
// into a small render target that ImGui shows as an image, so the preview matches the main screen
// and follows every change of style, intensity, colour and names at once. It renders only on the
// frames it is drawn.
#pragma once

#include "kit.hpp"

#include "../scene.hpp"
#include "toonval/key_state.hpp"
#include "toonval/keyboard.hpp"
#include "toonval/keyboard_view.hpp"
#include "toonval/note_trail.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <memory>
#include <vector>

namespace toonval::app::ui {

class LivePreview {
  public:
    explicit LivePreview(SDL_Renderer* renderer);
    ~LivePreview();
    LivePreview(const LivePreview&) = delete;
    LivePreview& operator=(const LivePreview&) = delete;

    // Renders the preview for `look` and lays it out at the cursor, `width` points wide, 16:6.
    void draw(Kit& k, const LookSettings& look, float width);

  private:
    void feed(double t, std::int64_t now_ns);
    void feed_grades(double t);
    void reset(std::int64_t now_ns);

    SDL_Renderer* renderer_;
    std::unique_ptr<Scene> scene_;
    SDL_Texture* target_ = nullptr;
    int target_w_ = 0;
    int target_h_ = 0;
    KeyboardLayout layout_{KeyRange{48, 76}};
    KeyViewFrame view_; // the whole layout, never moving
    KeyState keys_;
    NoteTrail trail_;
    std::vector<TrailBar> bars_;
    std::array<GradePopup, 5> grades_{}; // the grade words the loop cycles through
    std::size_t grade_count_ = 0;
    std::array<std::int64_t, 128> last_press_{};
    std::array<std::uint8_t, 128> last_velocity_{};
    ChordLabel chord_;
    double start_ = -1.0;
    double last_t_ = -1.0;
    double last_drawn_ = -1.0;
};

} // namespace toonval::app::ui
