// SPDX-License-Identifier: GPL-3.0-or-later
// Draws one frame: background, the song's falling notes and the player's rising note blocks,
// keyboard and chord name, each in its own region from toonval::compute_layout (the top row is
// the UI's), coloured, styled and named by a LookSettings. Holds no music state; the frame loop
// passes everything in. Each layer is a few SDL_RenderGeometry batches plus cached text textures
// (note names are cached per name and size and capped per frame), so the cost per frame stays
// flat. The effects level (from the core
// EffectsGovernor) decides which effects the frame can afford.
#pragma once

#include "effects.hpp"
#include "look.hpp"
#include "mesh.hpp"
#include "text.hpp"
#include "toonval/chord_display.hpp"
#include "toonval/effects_governor.hpp"
#include "toonval/evaluator.hpp"
#include "toonval/hand.hpp"
#include "toonval/key_state.hpp"
#include "toonval/keyboard.hpp"
#include "toonval/keyboard_view.hpp"
#include "toonval/look.hpp"
#include "toonval/note_style.hpp"
#include "toonval/note_trail.hpp"
#include "toonval/screen_layout.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace toonval::app {

// Never pressed, for FrameInput::last_press_ns.
inline constexpr std::int64_t kNeverPressed = INT64_MIN / 2;

// A note of the song on its way down to the hit line: NoteWindow's VisibleNote, placed on a key.
struct SongBar {
    std::uint8_t key = 0; // after transposition
    std::uint8_t velocity = 80;
    Hand hand = Hand::Unknown;
    // Fractions of the note area: 0 is the hit line, 1 the top. A note that has reached the line
    // has bottom <= 0; the bed hides what is below it.
    float bottom = 0.0f;
    float top = 0.0f;
    bool practised = true;  // false: a hand you are not practising, drawn faint
    std::int8_t grade = -1; // its NoteGrade once graded, else -1
    std::int8_t fold = 0;   // moved by this many octaves to fit the keyboard: a marker shows it
};

// A grade shown briefly where a note met the hit line (Flow Mode).
struct GradePopup {
    std::uint8_t key = 0;
    NoteGrade grade = NoteGrade::Missed;
    float age = 0.0f; // 0 when it appears, 1 when it is gone
};

struct FrameInput {
    // Built from `view.layout_range`: every key at least partly in the window.
    const KeyboardLayout& layout;
    const KeyViewFrame& view; // where the window sits on `layout`, see KeyViewFrame::to_window
    const KeyState& keys;
    std::span<const TrailBar> bars; // from NoteTrail::visible with Scene::trail_span_ns()
    // Latest note-on time per MIDI note, kept after release, so a tap shorter than a frame still
    // flashes its key.
    const std::array<std::int64_t, 128>& last_press_ns;
    // Note-on velocity of that latest press, so a lit key matches its block's colour.
    const std::array<std::uint8_t, 128>& last_velocity;
    std::int64_t now_ns;
    const ChordLabel& chord;
    bool no_keyboard; // shows the "connect a keyboard" prompt in the note area
    // The computer-key letter that plays each note right now ("A", ";"), empty for none, drawn
    // as small keycaps on the piano keys; null to draw none (--show-typing-keys).
    const std::array<std::string_view, 128>* typing_labels = nullptr;

    // The song, when one is loaded. Set after construction.
    std::span<const SongBar> song_bars{};
    std::span<const GradePopup> grades{};
    // Wait Mode: per key, 1 + the Hand of the chord note waiting there, 0 for none. Those keys
    // pulse in their hand's colour until pressed.
    const std::array<std::uint8_t, 128>* hint_hands = nullptr;
    // The last run's results, one line centred in the note area while the song is stopped; empty
    // for none.
    std::string_view results{};
    // The player's rising blocks are drawn this opaque: fainter while a song falls, so the two
    // don't compete.
    float trail_alpha = 1.0f;
};

class Scene {
  public:
    Scene(SDL_Renderer* renderer, const LookSettings& settings);

    const LookSettings& settings() const { return settings_; }
    void set_settings(const LookSettings& settings);
    // Time a note takes to cross the note area (LookSettings::visible_seconds), for NoteTrail.
    std::int64_t trail_span_ns() const;

    // Which effects the frame can afford (EffectsGovernor). Notes and keys are drawn at every
    // level.
    void set_effects_level(EffectsLevel level);
    EffectsLevel effects_level() const { return level_; }

    // Clears the screen and draws the frame, sized from the current render target in pixels.
    void draw(const FrameInput& in);

  private:
    struct KeyPx {
        float x = 0.0f;
        float width = 0.0f;
        bool black = false;
        bool in_range = false;
    };

    // The colours of one note, shared by its block and its key so the two always match. The
    // style (StylePainter) is applied on top of these.
    struct NotePaint {
        SDL_FColor base;   // the note's colour, flash applied: the far end of a block
        SDL_FColor lower;  // key end of a block (lighter while held), the front of a lit key
        float glow = 0.0f; // glow alpha, 0 for none
    };
    // The note's colour from the core look, so a block and its key always match.
    SDL_FColor fill_for(int note, int velocity) const;
    NotePaint paint(int note, int velocity, bool held, float flash,
                    Hand hand = kFreePlayHand) const;
    SDL_FColor hand_fill(int note, int velocity, Hand hand) const;
    void draw_song_bars(const FrameInput& in, float base_y, float min_height, float gap,
                        float glow_size);
    void draw_grades(const FrameInput& in);
    // Queues the name of `note` on `block` when it fits (see place_block_name), `alpha` applied.
    void add_block_name(int note, const RectF& block, const RectF& clip, SDL_FColor colour,
                        float alpha);
    void draw_results(const FrameInput& in);
    void update_recipe();
    void spawn_sparks(const FrameInput& in);
    void draw_sparks(const FrameInput& in);
    float flash_at(std::int64_t now_ns, std::int64_t pressed_ns) const;

    // The grade words, placed by core's place_grade_words so none overlaps another, a note name
    // or the chord label. Its text slots are measured at the current size.
    float grade_font_px() const;

    // A note name of `size` px centred on `centre_x` with its lower edge at `bottom`. The glyphs
    // are cached per (text, size), so a name is only rasterised the first time it is drawn.
    // Returns the width and height of the drawn text.
    SDL_FPoint draw_name(const std::string& text, float size, float centre_x, float bottom,
                         SDL_FColor colour, bool bold = false);
    SDL_FColor label_colour_for(int note, int velocity) const;
    SDL_FColor key_label_colour_for(int note, int velocity) const; // on a lit key
    void update_geometry(const KeyboardLayout& layout, const KeyViewFrame& view, float width,
                         float height);
    void rebuild_names();
    void draw_background(std::int64_t now_ns);
    SDL_Rect effects_clip() const; // the note area and the key bed
    void draw_notes(const FrameInput& in);
    void draw_edge_indicators();
    void draw_keyboard(const FrameInput& in);
    void draw_chord(const ChordLabel& chord);
    void draw_prompt(const FrameInput& in);

    SDL_Renderer* renderer_;
    LookSettings settings_;
    Look look_; // render-only constants
    Text text_;
    Mesh mesh_;
    Mesh overlay_; // pressed-key bands, lips and the bed's shadow over the styled keys
    Mesh glow_;    // block and key glow sprites, drawn clipped to the notes and again to the bed
    Mesh fx_;      // sparks and the motion background, drawn with the glow sprite
    EffectTextures textures_;
    StylePainter painter_;
    Sparks sparks_;
    EffectsLevel level_ = EffectsLevel::Full;
    EffectsBudget budget_;
    StyleRecipe recipe_;                         // the look's style at the current effects level
    std::array<std::int64_t, 128> seen_press_{}; // last note-on each note threw sparks for
    std::int64_t epoch_ns_ = 0;                  // first frame, for the motion background clock
    bool has_epoch_ = false;

    // Pixel geometry, rebuilt when the output size, the keyboard range or the look changes.
    bool geometry_valid_ = false;
    float width_ = 0.0f;
    float height_ = 0.0f;
    KeyRange range_{0, 0};
    float pan_ = -1.0f; // the view the keys_ were placed for
    float span_ = -1.0f;
    ScreenLayout layout_;
    float px_per_mm_ = kDefaultPxPerMm;
    float unit_ = 1.0f; // pixels per 1080p pixel
    float white_width_ = 0.0f;
    std::array<KeyPx, 128> keys_{};

    // Per-note data from the settings: colours (all but ByVelocity, which needs the velocity),
    // names, and the key-name font fitted to the white and black keys.
    std::array<SDL_FColor, 128> fill_{};
    std::array<SDL_FColor, 128> label_colour_{};
    std::array<SDL_FColor, 128> key_label_colour_{};
    std::array<std::string, 128> names_;
    std::array<std::string, 128> short_names_; // the letter alone, for blocks too narrow for a name
    float min_name_px_ = kMinNameFontPx;       // the smallest name drawn, for this display
    std::array<std::string, 128> c_names_;     // C markers, empty for notes that are not a C
    float c_marker_px_ = 0.0f;
    float white_name_px_ = 0.0f;
    float black_name_px_ = 0.0f;

    struct PendingName {
        const std::string* text;
        float size;
        float centre_x;
        float bottom;
        SDL_FColor colour;
        RectF rect; // the text box, kept clear of the grade words
    };
    std::vector<PendingName> block_names_; // collected while the blocks are built
    // Played notes whose key is off the left or right edge of the window, for the edge arrows.
    struct EdgeMark {
        int side;     // -1 left, 1 right
        float centre; // y of the block's middle
        float half_h; // half the block's height
        SDL_FColor colour;
    };
    std::vector<EdgeMark> edge_marks_;
    std::map<std::tuple<std::string, int, bool>, TextSlot> name_cache_;

    std::array<TextSlot, 6> grade_slot_; // one per NoteGrade, bold, at grade_font_px()
    TextSlot chord_slot_;
    TextSlot prompt_slot_;
    TextSlot results_slot_;
};

} // namespace toonval::app
