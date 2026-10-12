// SPDX-License-Identifier: GPL-3.0-or-later
// The touch UI on top of the scene, drawn with Dear ImGui (app/ui/): the top row with the status
// text on the left and the settings button on the right, the control bar that appears in that row
// on a hover or a tap (in the style Settings > View > Bar style picks), the settings sheet, and the
// countdown before a song plays on after the settings close. Holds no music state: it edits the
// UiModel the frame loop owns (the loop notices the change by comparing it, applies it to the
// scene and hands it to the saver), and drives the song through a ui::SongPort.
#pragma once

#include "toonval/screen_layout.hpp"
#include "ui/song_port.hpp"
#include "ui_model.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace toonval::app {

// Which page of the settings is showing.
enum class PanelTab : std::uint8_t { Quick, Look, Names, Effects, View, Presets, Practice };

// Parses "quick", "look", "names", ...; false for anything else. For the --panel option.
bool parse_panel_tab(const char* text, PanelTab* out);

// Output pixels per millimetre of the window's display, from SDL's pixel density the way the scene
// computes it (SDL does not report the panel's physical size).
float display_px_per_mm(SDL_Window* window);

struct UiFrameInfo {
    float width_px = 0.0f; // renderer output size
    float height_px = 0.0f;
    float px_per_mm = 0.0f;
    ScreenLayout layout; // compute_layout(...) for the current look
    std::int64_t now_ns = 0;
    // The player is at the piano: keys are held or were pressed a moment ago. The bar hides
    // sooner then.
    bool playing = false;
    // The status text in the top row: the keyboard name, then up to four notes (the song's line,
    // messages, warnings); empty ones are skipped.
    std::string_view status_label;
    std::array<std::string_view, 4> status_notes;
};

class Ui {
  public:
    Ui(SDL_Window* window, SDL_Renderer* renderer);
    ~Ui();
    Ui(const Ui&) = delete;
    Ui& operator=(const Ui&) = delete;

    // Feeds one SDL event. Returns true when the UI used it (typing into a text field), so the
    // caller must not also act on it.
    bool process_event(const SDL_Event& event, std::int64_t now_ns);

    // The settings sheet. While it is open it covers everything under the top row.
    bool panel_open() const;
    void open_panel(PanelTab tab);
    void close_panel();

    // Builds and draws the UI on top of the finished scene, editing `model` and acting on `song`.
    void draw(UiModel& model, ui::SongPort& song, const UiFrameInfo& info);

    // A short sentence about something the UI just did ("Saved as Night"), empty if none.
    std::string take_message();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace toonval::app
