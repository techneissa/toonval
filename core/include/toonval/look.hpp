// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "toonval/color.hpp"
#include "toonval/hand.hpp"

namespace toonval {

// The "Look and feel" options as plain data. The settings panel writes it, the
// renderer reads it, and nothing in here touches SDL, files or clocks.

enum class NoteStyle : std::uint8_t { Filled, Glassy, Frosted, Shiny, Ghost, Neon };
enum class ColorScheme : std::uint8_t { ByHand, ByPitchClass, ByOctave, ByVelocity, Single };
enum class NameMode : std::uint8_t { Off, Letters, Solfege };
enum class BackgroundStyle : std::uint8_t { Plain, Gradient, Motion };
// Octave numbers shown to the player: scientific has middle C (MIDI 60) as C4, Yamaha manuals
// (the PSR-E383's included) call it C3.
enum class OctaveNumbering : std::uint8_t { Scientific, Yamaha };
// How wide the keys are drawn (the sizes in millimetres are in
// keyboard_view.hpp). The two fit modes fill the window width; the fixed sizes keep a real key
// width and the view follows the music across the keyboard.
enum class KeySize : std::uint8_t { FitKeyboard, FitSong, Small, Medium, Large, ExtraLarge };
// Where Flow Mode's grade words (Perfect, Good, ...) sit in the note area: just above the keys, or
// at the top of the note area below the chord label.
enum class GradePosition : std::uint8_t { Bottom, Top };

// Background colors the palettes are built against. `kBackgroundColor` is the plain background and
// the bottom of the gradient, `kBackgroundGradientTop` its lighter top. Motion stays between them.
inline constexpr RGBA kBackgroundColor{18, 20, 26, 255};
inline constexpr RGBA kBackgroundGradientTop{34, 38, 52, 255};

// Documented ranges. `LookSettings::clamped()` pulls every value into them.
inline constexpr float kMinStyleIntensity = 0.0f;
inline constexpr float kMaxStyleIntensity = 1.0f;
inline constexpr float kMinChordLabelSize = 0.5f; // multiplier on the default label size
inline constexpr float kMaxChordLabelSize = 2.0f;
inline constexpr float kMinVisibleSeconds = 1.0f; // seconds of notes that fit above the keyboard
inline constexpr float kMaxVisibleSeconds = 12.0f;
inline constexpr float kMinKeyboardHeight = 0.10f; // fraction of the window height
inline constexpr float kMaxKeyboardHeight = 0.30f;
inline constexpr int kMinGradeOpacity = 0; // percent; 0 hides the grade words
inline constexpr int kMaxGradeOpacity = 100;

// WCAG ratio the high-contrast palette reaches against the backgrounds above, for white-key notes
// (`kHighContrastTarget`) and, after the black-key darkening, for black-key notes
// (`kHighContrastMinimum`). Label text on a block always reaches `kLabelMinContrast`.
inline constexpr double kHighContrastTarget = 7.0;
inline constexpr double kHighContrastMinimum = 4.5;
inline constexpr double kLabelMinContrast = 4.5;

// The default 12-color pitch-class wheel, C first: evenly spaced hues at one OKLCH lightness, so
// neighbors differ clearly and none is much brighter than another.
std::array<RGBA, 12> default_pitch_colors();

struct LookSettings {
    // Note style, with an intensity 0..1 (how strong the gloss, frost, glow or outline is).
    NoteStyle note_style = NoteStyle::Glassy;
    float style_intensity = 0.6f;

    // Colors. Left/right/unknown are for ByHand, `pitch_colors` (C first) for ByPitchClass and
    // `single_color` for Single. ByOctave and ByVelocity use fixed ramps. `high_contrast` lifts
    // whatever the scheme gives until it reaches `kHighContrastTarget` on the dark background.
    ColorScheme color_scheme = ColorScheme::ByHand;
    RGBA left_color{255, 169, 77, 255};     // warm orange
    RGBA right_color{90, 170, 255, 255};    // the app's accent blue
    RGBA unknown_color{169, 177, 192, 255}; // cool grey
    std::array<RGBA, 12> pitch_colors = default_pitch_colors();
    RGBA single_color{90, 170, 255, 255};
    bool high_contrast = false;

    // Note names.
    NameMode name_mode = NameMode::Letters;
    bool names_show_octave = false;
    bool names_prefer_flats = false;
    bool names_on_blocks = true;
    bool names_on_keys = false;
    // Every C key carries its name and octave, middle C emphasized. Independent of the switches
    // above, so it works with names Off.
    bool c_markers = true;
    // Applies to every octave number shown: names with the octave, C markers, status text.
    OctaveNumbering octave_numbering = OctaveNumbering::Scientific;

    // Effects.
    bool key_flash = true;
    bool held_glow = true;
    bool sparks = true;
    bool trail_fade = true;
    BackgroundStyle background = BackgroundStyle::Gradient;
    // Forces the background motion and the particles (sparks) off in the effective accessors.
    bool reduced_effects = false;

    // Chord label and the view.
    KeySize key_size = KeySize::FitKeyboard;
    // While playing with the computer keyboard, show its letters on the piano keys.
    bool show_typing_keys = false;
    bool chord_label = true;
    float chord_label_size = 1.0f;
    float visible_seconds = 4.0f;
    float keyboard_height = 0.18f;

    // Flow Mode's grade words. `grade_words` is the switch, `grade_opacity` the strength in
    // percent; 0 hides them just like the switch, so the two never disagree (see
    // `set_grade_words` and `set_grade_opacity`).
    bool grade_words = true;
    GradePosition grade_position = GradePosition::Bottom;
    int grade_opacity = kMaxGradeOpacity;

    // Whether the words show at all, and how opaque (0..1).
    bool grade_words_shown() const { return grade_words && grade_opacity > kMinGradeOpacity; }
    float grade_alpha() const {
        return grade_words_shown() ? static_cast<float>(grade_opacity) / 100.0f : 0.0f;
    }

    // Where names are drawn: only when the mode is not Off and that place is switched on. The two
    // places are independent (blocks only, keys only, both, or neither).
    bool shows_block_names() const { return name_mode != NameMode::Off && names_on_blocks; }
    bool shows_key_names() const { return name_mode != NameMode::Off && names_on_keys; }

    // Added to the scientific octave number: 0, or -1 for Yamaha numbering.
    int octave_offset() const { return octave_numbering == OctaveNumbering::Yamaha ? -1 : 0; }

    // What the renderer should actually draw. Everything but these two is read directly.
    BackgroundStyle effective_background() const;
    bool effective_sparks() const;

    // True when every value is inside its documented range (and no enum holds a stray number).
    bool is_valid() const;
    // Brings every out-of-range value back: numbers to the nearest end of their range (NaN to the
    // default), a stray enum value to its default.
    LookSettings clamped() const;
    void validate() { *this = clamped(); }

    friend bool operator==(const LookSettings&, const LookSettings&) = default;
};

// Named looks that set every option at once.
enum class PresetId : std::uint8_t {
    Default,
    Classic,
    NeonNight,
    Glass,
    FrostedPastel,
    HighContrast,
    MinimalReduced,
};
inline constexpr std::array<PresetId, 7> kAllPresets{
    PresetId::Default,       PresetId::Classic,      PresetId::NeonNight,     PresetId::Glass,
    PresetId::FrostedPastel, PresetId::HighContrast, PresetId::MinimalReduced};

LookSettings preset(PresetId id);
std::string_view preset_name(PresetId id);

// A built-in preset applied over `current`: every look option comes from the preset, but the
// player's own preferences stay as they were: the octave numbering, the key size and the
// typing-key letters. User slots save and load the whole look, these three included.
LookSettings apply_preset(const LookSettings& current, PresetId id);
// True when `look` is the preset `id`, those three preferences aside.
bool is_preset(const LookSettings& look, PresetId id);

// The grade-word switch as the player sets it. On shows the words: a stored opacity of 0 comes
// back as `restore` (the last non-zero one the caller knows, 100 when it knows none). Off hides
// them and keeps the opacity, so turning them on again restores it.
void set_grade_words(LookSettings& look, bool on, int restore = kMaxGradeOpacity);
// The opacity slider as the player sets it (percent, clamped). Above 0 it switches the words on;
// 0 switches them off and puts `restore` back as the stored opacity, so the switch can bring the
// last non-zero strength back.
void set_grade_opacity(LookSettings& look, int percent, int restore = kMaxGradeOpacity);

// Block color for a note. White keys get the scheme's color; black keys get the same color
// darkened, in every scheme. `velocity` is 0..127 and only ByVelocity reads it. The alpha is the
// scheme color's own, and the style (gloss, frost, glow) is the renderer's job.
RGBA note_color(const LookSettings& look, int note, std::uint8_t velocity, Hand hand);
// Black or white, whichever reads better on `block`. At least `kLabelMinContrast` on any color.
RGBA label_color_for(RGBA block);
// `label_color_for(note_color(...))`.
RGBA note_label_color(const LookSettings& look, int note, std::uint8_t velocity, Hand hand);
// The color a pressed key lights in. It is `note_color` itself, so a key and the note block above
// it always match, under every scheme and for black keys too. A key pressed with no note to play
// (free play, or a wrong key in practice) passes `Hand::Unknown` and the velocity it was hit with.
inline RGBA key_light_color(const LookSettings& look, int note, std::uint8_t velocity, Hand hand) {
    return note_color(look, note, velocity, hand);
}

// The octave number the player sees for `note`: the scientific octave plus
// `LookSettings::octave_offset()`. Note 60 is 4 (scientific) or 3 (Yamaha); notes 0..11 are -1
// (scientific) or -2 (Yamaha).
int octave_number(int note, const LookSettings& look);

// "C", "C#" or "Db", "Do", "Do#" or "Reb" (fixed-do), with the octave ("C#4") when
// `names_show_octave` is set. Empty when names are Off or the note is outside 0..127.
std::string note_label(int note, const LookSettings& look);

// The name on a C key: letter "C" ("Do" in solfege mode) and the octave number, always, whatever
// the name mode and `names_show_octave`: "C4" or "C3" for middle C. Empty when `c_markers` is off,
// the note is not a C or is outside 0..127.
std::string c_marker_label(int note, const LookSettings& look);
// `note_label` cut to its letter: no accidental, no octave ("C", "Do"). What a block too narrow
// for the whole name shows instead. Empty under the same conditions as `note_label`.
std::string note_short_label(int note, const LookSettings& look);
// The key middle C (MIDI 60), emphasized among the C markers whatever the numbering says.
bool is_middle_c(int note);

// Three user slots that save the current look under a name and load it back. The built-in
// presets above stay functions; these are the player's own.
inline constexpr int kUserPresetSlots = 3;
inline constexpr std::size_t kMaxPresetNameBytes = 48; // UTF-8 bytes, cut at a character boundary

struct UserPresetSlot {
    bool used = false;
    std::string name;  // empty when the slot is unused, never empty when it is used
    LookSettings look; // the defaults when the slot is unused

    friend bool operator==(const UserPresetSlot&, const UserPresetSlot&) = default;
};

struct UserPresets {
    std::array<UserPresetSlot, kUserPresetSlots> slots;

    // `slot` is 0..2; every function refuses anything else (false, or an empty name).
    bool is_used(int slot) const;
    std::string_view slot_name(int slot) const;
    // Stores `look` (clamped) under `name`. The name is trimmed, loses control characters and is
    // capped at `kMaxPresetNameBytes`; one that ends up empty or is not valid UTF-8 becomes
    // "Preset <n>". Replaces whatever the slot held.
    bool save_slot(int slot, std::string_view name, const LookSettings& look);
    // Copies the slot's look into `*out` (when not null). False, and `*out` untouched, for an
    // empty slot.
    bool load_slot(int slot, LookSettings* out) const;
    bool clear_slot(int slot);

    friend bool operator==(const UserPresets&, const UserPresets&) = default;
};

// Everything the look screen saves between sessions.
struct LookDocument {
    LookSettings look;
    UserPresets user_presets;

    friend bool operator==(const LookDocument&, const LookDocument&) = default;
};

// Text form: a versioned `key = value` list, one per line. Unknown keys and bad values are
// skipped on the way back in.
inline constexpr int kLookFormatVersion = 1;
// Text of the look alone.
std::string serialize(const LookSettings& look);
// Text of the look and the three user slots: `slot_1_used = true`, `slot_1_name = ...`, then the
// slot's settings under the same names with a `slot_1_` prefix. An empty slot is one
// `slot_N_used = false` line. Names are written as raw UTF-8.
std::string serialize_document(const LookDocument& doc);

struct ParseIssue {
    enum class Kind : std::uint8_t {
        BadLine,      // no `key = value` shape
        UnknownKey,   // the key is not a setting; value ignored
        BadValue,     // the value does not parse; setting keeps its default
        OutOfRange,   // a number outside its range; the nearest end is used
        NewerVersion, // the file says it is a newer format than this build knows
    };
    Kind kind = Kind::BadLine;
    int line = 0; // 1-based
    std::string key;
};

struct ParseReport {
    int version = 0; // 0 when the text has no version line
    int applied = 0; // settings read from the text
    std::vector<ParseIssue> issues;
    bool clean() const { return issues.empty(); }
};

// Reads `serialize()` output. Starts from the defaults, so a missing key keeps its default. Never
// throws, and always returns valid settings. A later line for the same key wins.
LookSettings parse_look(std::string_view text, ParseReport* report = nullptr);
// The same for the whole document. A slot the text marks unused is empty, whatever else its lines
// say; a used slot with no valid name gets "Preset <n>". A damaged line costs that one setting.
LookDocument parse_look_document(std::string_view text, ParseReport* report = nullptr);

} // namespace toonval
