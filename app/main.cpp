// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio_out.hpp"
#include "bench.hpp"
#include "builtin_piano_sink.hpp"
#include "dev_script.hpp"
#include "midi_input.hpp"
#include "midi_output.hpp"
#include "persistence.hpp"
#include "runs_file.hpp"
#include "scene.hpp"
#include "song_controller.hpp"
#include "song_library.hpp"
#include "song_transport.hpp"
#include "synth_test.hpp"
#include "toonval/chord_display.hpp"
#include "toonval/computer_keyboard.hpp"
#include "toonval/effects_governor.hpp"
#include "toonval/key_state.hpp"
#include "toonval/keyboard.hpp"
#include "toonval/keyboard_view.hpp"
#include "toonval/latency_stats.hpp"
#include "toonval/look.hpp"
#include "toonval/note_style.hpp"
#include "toonval/note_trail.hpp"
#include "toonval/pitch.hpp"
#include "toonval/progress.hpp"
#include "toonval/run_record.hpp"
#include "toonval/screen_layout.hpp"
#include "toonval/session_restore.hpp"
#include "toonval/sound_sink.hpp"
#include "ui.hpp"
#include "virtual_input.hpp"
#include "window_icon.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using toonval::ChordDisplay;
using toonval::KeyboardLayout;
using toonval::KeyState;
using toonval::KeyTransition;
using toonval::KeyTransitionType;
using toonval::MidiEvent;
using toonval::NoteTrail;
using toonval::app::midi_clock_ns;
using toonval::app::MidiInput;
using toonval::app::set_window_icon;
using toonval::app::MidiOutput;
using toonval::app::VirtualInput;

// Everything the frame loop knows about the player's keyboard.
struct Session {
    KeyState keys;
    KeyboardLayout layout{toonval::kRange88};
    const toonval::KeyboardProfile* profile = nullptr;
    int open_inputs = 0;
    // Key presses outside the layout's range, a hint that the keyboard's octave shift is on.
    std::uint64_t out_of_range = 0;
    std::string label = "No keyboard";
    // The saved keyboard choice (Profile or Manual) is in force instead of the detected keyboard.
    bool overridden = false;
    toonval::KeyRange override_range{0, 0};
};

std::string keyboard_label(const Session& s) {
    if (s.open_inputs == 0) {
        return "No keyboard";
    }
    if (s.profile == nullptr) {
        return "Unknown keyboard, 88 keys";
    }
    return std::string(s.profile->name) + ", " + std::to_string(s.profile->range.key_count()) +
           " keys";
}

// Applies port changes: releases every key when an input went away and rebuilds the layout when
// the matched keyboard changed. Returns the release (AllReleased, or None) and sets `label_changed`
// when the keyboard label changed.
KeyTransition apply_port_changes(Session& s, const MidiInput& midi,
                                 toonval::app::PortChanges changes, bool& label_changed) {
    KeyTransition released;
    if (changes.closed > 0) {
        released = s.keys.clear();
    }
    label_changed = false;
    const auto* profile = midi.profile();
    const int open = midi.open_count();
    if (profile == s.profile && open == s.open_inputs) {
        return released;
    }
    if (profile != s.profile) {
        s.layout = KeyboardLayout(profile != nullptr ? profile->range : toonval::kRange88);
    }
    s.profile = profile;
    s.open_inputs = open;
    s.label = keyboard_label(s);
    label_changed = true;
    return released;
}

KeyTransition apply_event(Session& s, const MidiEvent& ev) {
    const KeyTransition t = s.keys.apply(ev);
    if (t.type == KeyTransitionType::Pressed && !s.layout.range().contains(t.note)) {
        ++s.out_of_range;
    }
    return t;
}

// The saved keyboard choice, from Settings > View. Auto (false) leaves the detected keyboard in
// charge; Profile and Manual give the range and label to use instead.
bool chosen_keyboard(const toonval::SessionState& saved, toonval::KeyRange* range,
                     std::string* label) {
    switch (saved.keyboard) {
    case toonval::KeyboardChoice::Auto:
        return false;
    case toonval::KeyboardChoice::Profile: {
        *range = saved.keyboard_range();
        std::string name = saved.keyboard_name;
        for (const toonval::KeyboardProfile& p : toonval::builtin_keyboard_profiles()) {
            if (p.name == saved.keyboard_name) {
                *range = p.range; // the profile's own range wins over the one saved with it
            }
        }
        *label = name + ", " + std::to_string(range->key_count()) + " keys";
        return true;
    }
    case toonval::KeyboardChoice::Manual:
        *range = saved.keyboard_range();
        *label = "Keyboard, " + std::to_string(range->key_count()) + " keys";
        return true;
    }
    return false;
}

// Applies the saved keyboard choice on top of what the ports gave, and takes it off again when
// the choice goes back to Auto. Returns true when the layout changed.
bool apply_keyboard_choice(Session& s, const toonval::SessionState& saved) {
    toonval::KeyRange range{0, 0};
    std::string label;
    if (chosen_keyboard(saved, &range, &label)) {
        const bool same = s.overridden && s.override_range.lowest == range.lowest &&
                          s.override_range.highest == range.highest && s.label == label;
        if (same) {
            return false;
        }
        s.layout = KeyboardLayout(range);
        s.label = label;
        s.override_range = range;
        s.overridden = true;
        return true;
    }
    if (!s.overridden) {
        return false;
    }
    s.overridden = false;
    s.layout = KeyboardLayout(s.profile != nullptr ? s.profile->range : toonval::kRange88);
    s.label = keyboard_label(s);
    return true;
}

// --- Window -----------------------------------------------------------------------------------

constexpr std::int64_t kLatencyReportNs = 5'000'000'000;

// --latency-log: time from each note-on's MIDI stamp to just after the SDL_RenderPresent of the
// first frame drawn with it, on the same clock. Covers queueing, the wait for the next frame,
// drawing and the present call; not the display's own scan-out and panel lag.
class LatencyLog {
  public:
    LatencyLog() { pending_.reserve(256); }

    void note_on(std::int64_t time_ns) {
        if (pending_.size() < pending_.capacity()) {
            pending_.push_back(time_ns);
        }
    }

    // Call right after SDL_RenderPresent.
    void presented(std::int64_t now_ns) {
        for (const std::int64_t t : pending_) {
            stats_.add(now_ns - t);
        }
        pending_.clear();
        if (next_report_ns_ == 0) {
            next_report_ns_ = now_ns + kLatencyReportNs;
        }
        if (now_ns < next_report_ns_) {
            return;
        }
        next_report_ns_ = now_ns + kLatencyReportNs;
        const toonval::LatencySummary s = stats_.take_summary();
        if (s.count == 0) {
            return;
        }
        std::printf("latency note-on to present: %zu notes, min %.2f ms, median %.2f ms, max %.2f "
                    "ms\n",
                    s.count, static_cast<double>(s.min_ns) / 1e6,
                    static_cast<double>(s.median_ns) / 1e6, static_cast<double>(s.max_ns) / 1e6);
    }

  private:
    std::vector<std::int64_t> pending_;
    toonval::LatencyStats stats_;
    std::int64_t next_report_ns_ = 0;
};

// Frame period for pacing when the driver refuses vsync, from the display's refresh rate.
std::int64_t fallback_frame_ns(SDL_Window* window) {
    float hz = 60.0f;
    if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
        mode != nullptr && mode->refresh_rate > 1.0f) {
        hz = mode->refresh_rate;
    }
    return static_cast<std::int64_t>(1e9 / static_cast<double>(hz));
}

// Window size from --window; zero means the default 1280x720 with the display's full pixel density.
struct WindowSize {
    int width = 0;
    int height = 0;
};

// What the command line asks of the window mode.
struct WindowOptions {
    bool latency_log = false;
    WindowSize size;
    // Startup look overrides, applied on top of the saved look (not saved by themselves).
    std::optional<toonval::PresetId> preset;
    const char* names = nullptr;              // off, letters or solfege
    const char* octaves = nullptr;            // scientific or yamaha
    const char* style = nullptr;              // a note style name
    const char* colors = nullptr;             // a colour scheme name, see parse_colors
    std::optional<toonval::KeySize> key_size; // --key-size
    bool show_typing_keys = false;            // --show-typing-keys
    bool debug_effects = false;               // the effects level in the status line
    std::string settings_dir;                 // empty: SDL's per-user folder
    std::optional<toonval::app::PanelTab> panel;
    toonval::app::DevScript script;
    toonval::app::AudioOptions audio;
    // Songs (this run): --song, --mode, --hands, --song-sound, --play, --robot.
    std::string song_path;
    std::optional<toonval::PracticeMode> mode;
    std::optional<toonval::HandFilter> hands;
    std::optional<toonval::app::SongSound> song_sound;
    bool play = false;    // start playing at once
    double seek_s = -1.0; // start the song here (seconds)
    bool robot = false;   // developer: play the practised notes, for checking the modes
};

// --- Songs ----------------------------------------------------------------------------------

// Developer stand-in for a player (--robot), for checking the practice modes on screen: in Flow
// Mode it presses each practised note as it reaches the hit line, most on time, some late, and
// misses every eleventh; in Wait Mode it presses the waiting chord after 0.4 s.
class Robot {
  public:
    void update(const toonval::app::SongController& songs, std::int64_t now,
                std::vector<MidiEvent>& out) {
        if (!songs.has_song()) {
            return;
        }
        const double pos = songs.position_s();
        if (songs.mode() == toonval::PracticeMode::Wait) {
            if (songs.waiting()) {
                if (waiting_since_ == 0) {
                    waiting_since_ = now;
                } else if (now - waiting_since_ > 400'000'000 && !pressed_) {
                    for (const auto& k : songs.waiting_keys()) {
                        queue(k.key, now, now + 150'000'000);
                    }
                    pressed_ = true;
                }
            } else {
                waiting_since_ = 0;
                pressed_ = false;
            }
        } else if (songs.playing() && last_pos_ >= 0.0 && pos > last_pos_ &&
                   pos - last_pos_ < 0.5) { // not across a seek
            const auto& notes = songs.song().song.notes;
            auto it =
                std::lower_bound(notes.begin(), notes.end(), last_pos_,
                                 [](const toonval::Note& n, double s) { return n.start_s <= s; });
            const double rate = songs.speed_percent() / 100.0;
            for (; it != notes.end() && it->start_s <= pos; ++it) {
                const auto i = static_cast<std::size_t>(it - notes.begin());
                const toonval::HandFilter h = songs.practice_hands();
                if (it->drum ||
                    (h == toonval::HandFilter::Left && it->hand != toonval::Hand::Left) ||
                    (h == toonval::HandFilter::Right && it->hand != toonval::Hand::Right)) {
                    continue;
                }
                if (i % 11 == 5) {
                    continue; // a miss
                }
                const std::int64_t late = i % 4 == 1 ? 40'000'000 : i % 4 == 3 ? 62'000'000 : 0;
                const auto arrival =
                    now - static_cast<std::int64_t>((pos - it->start_s) / rate * 1e9);
                const int key = songs.transposition().expected_pitch(*it).pitch;
                const auto length =
                    static_cast<std::int64_t>(std::min(0.4, it->duration_s / rate * 0.8) * 1e9);
                if (key >= 0) {
                    queue(key, arrival + late,
                          arrival + late + std::max<std::int64_t>(length, 50'000'000));
                }
            }
        }
        last_pos_ = pos;
        std::stable_sort(
            pending_.begin(), pending_.end(),
            [](const MidiEvent& a, const MidiEvent& b) { return a.time_ns < b.time_ns; });
        std::size_t n = 0;
        while (n < pending_.size() && pending_[n].time_ns <= now) {
            out.push_back(pending_[n]);
            ++n;
        }
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(n));
    }

  private:
    void queue(int key, std::int64_t on, std::int64_t off) {
        MidiEvent ev;
        ev.type = toonval::MidiEventType::NoteOn;
        ev.data1 = static_cast<std::uint8_t>(key);
        ev.data2 = 80;
        ev.time_ns = on;
        ev.source = toonval::InputSource::Pointer;
        pending_.push_back(ev);
        ev.type = toonval::MidiEventType::NoteOff;
        ev.data2 = 0;
        ev.time_ns = off;
        pending_.push_back(ev);
    }

    std::vector<MidiEvent> pending_;
    double last_pos_ = -1.0;
    std::int64_t waiting_since_ = 0;
    bool pressed_ = false;
};

// Seconds east of UTC here, now (for the day a run belongs to).
std::int32_t local_utc_offset_s(std::time_t t) {
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &t);
    long bias = 0;
    _get_timezone(&bias);
    return static_cast<std::int32_t>(-bias + (local.tm_isdst > 0 ? 3600 : 0));
#else
    localtime_r(&t, &local);
    return static_cast<std::int32_t>(local.tm_gmtoff);
#endif
}

// Colour scheme names as typed on the command line.
bool parse_colors(const char* text, toonval::ColorScheme* out) {
    struct Entry {
        const char* name;
        toonval::ColorScheme scheme;
    };
    static const Entry kEntries[] = {
        {"hand", toonval::ColorScheme::ByHand},     {"pitch", toonval::ColorScheme::ByPitchClass},
        {"octave", toonval::ColorScheme::ByOctave}, {"velocity", toonval::ColorScheme::ByVelocity},
        {"single", toonval::ColorScheme::Single},
    };
    for (const Entry& e : kEntries) {
        if (std::strcmp(text, e.name) == 0) {
            *out = e.scheme;
            return true;
        }
    }
    return false;
}

// Key size names as typed on the command line.
bool parse_key_size(const char* text, toonval::KeySize* out) {
    struct Entry {
        const char* name;
        toonval::KeySize size;
    };
    static const Entry kEntries[] = {
        {"fit-keyboard", toonval::KeySize::FitKeyboard},
        {"fit-song", toonval::KeySize::FitSong},
        {"small", toonval::KeySize::Small},
        {"medium", toonval::KeySize::Medium},
        {"large", toonval::KeySize::Large},
        {"extra-large", toonval::KeySize::ExtraLarge},
    };
    for (const Entry& e : kEntries) {
        if (std::strcmp(text, e.name) == 0) {
            *out = e.size;
            return true;
        }
    }
    return false;
}

void apply_look_options(const WindowOptions& o, toonval::LookSettings& look) {
    if (o.key_size) {
        look.key_size = *o.key_size;
    }
    if (o.show_typing_keys) {
        look.show_typing_keys = true;
    }
    if (o.preset) {
        const toonval::OctaveNumbering numbering = look.octave_numbering;
        look = toonval::preset(*o.preset);
        look.octave_numbering = numbering;
    }
    if (o.octaves != nullptr) {
        look.octave_numbering = std::strcmp(o.octaves, "yamaha") == 0
                                    ? toonval::OctaveNumbering::Yamaha
                                    : toonval::OctaveNumbering::Scientific;
    }
    if (o.names != nullptr) {
        // One switch for both places: names on the blocks and on the keys.
        look.name_mode = std::strcmp(o.names, "off") == 0       ? toonval::NameMode::Off
                         : std::strcmp(o.names, "solfege") == 0 ? toonval::NameMode::Solfege
                                                                : toonval::NameMode::Letters;
        look.names_on_blocks = true;
        look.names_on_keys = true;
    }
    if (o.style != nullptr) {
        toonval::parse_note_style(o.style, &look.note_style);
    }
    if (o.colors != nullptr) {
        parse_colors(o.colors, &look.color_scheme);
    }
}

// Where the settings file lives: the --settings-dir option, else SDL's per-user folder.
std::filesystem::path settings_directory(const std::string& option) {
    if (!option.empty()) {
        return option;
    }
    char* pref = SDL_GetPrefPath("toonval", "toonval");
    std::filesystem::path dir = pref != nullptr ? pref : ".";
    SDL_free(pref);
    return dir;
}

constexpr std::int64_t kStatusMessageNs = 6'000'000'000;
// The bar fades while keys are held or were pressed this recently.
constexpr std::int64_t kPlayingHoldNs = 3'000'000'000;

// State shared with the SDL event watch that redraws during a live window resize.
struct RedrawHook {
    std::function<void()> draw;
    SDL_WindowID window = 0;
    bool polling = false; // inside the SDL_PollEvent loop: the main loop draws right after
    bool drawing = false; // inside a frame already
};

// Called by SDL for every event, on the thread that produces it. Only the three that mean "the
// window has a new size or needs repainting", and only outside the main loop's own work.
bool SDLCALL on_window_event_watch(void* userdata, SDL_Event* event) {
    auto* hook = static_cast<RedrawHook*>(userdata);
    switch (event->type) {
    case SDL_EVENT_WINDOW_EXPOSED:
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        if (event->window.windowID == hook->window && !hook->polling && !hook->drawing &&
            SDL_IsMainThread()) {
            hook->drawing = true;
            hook->draw();
            hook->drawing = false;
        }
        break;
    default:
        break;
    }
    return true;
}

int run_window(WindowOptions& opt) {
    const bool latency_log = opt.latency_log;
    const WindowSize size = opt.size;
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    // An exact --window size also turns off the high pixel density, so the output has exactly
    // that many pixels, like a small panel.
    const bool exact = size.width > 0;
    if (!SDL_CreateWindowAndRenderer(
            "toonval", exact ? size.width : 1280, exact ? size.height : 720,
            SDL_WINDOW_RESIZABLE | (exact ? 0 : SDL_WINDOW_HIGH_PIXEL_DENSITY), &window,
            &renderer)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "window creation failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    set_window_icon(window);
    // The layout rules hold from 800x480; a window asked for smaller (--window) may go that small.
    SDL_SetWindowMinimumSize(window, exact ? std::min(size.width, 800) : 800,
                             exact ? std::min(size.height, 480) : 480);
    const bool vsync = SDL_SetRenderVSync(renderer, 1);
    const std::int64_t frame_ns = fallback_frame_ns(window);
    if (!vsync) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "vsync refused (%s), pacing frames at %.1f Hz",
                    SDL_GetError(), 1e9 / static_cast<double>(frame_ns));
    }
    if (latency_log) {
        std::setvbuf(stdout, nullptr, _IOLBF, 0);
        std::printf("renderer %s, vsync %s, display %.1f Hz\n", SDL_GetRendererName(renderer),
                    vsync ? "on" : "off", 1e9 / static_cast<double>(frame_ns));
    }

    {
        auto midi = std::make_unique<MidiInput>();
        Session session;
        NoteTrail trail;
        ChordDisplay chord;
        toonval::ChordLabel label;
        std::vector<std::uint8_t> held;
        held.reserve(128);
        std::vector<toonval::TrailBar> bars;
        bars.reserve(1024);
        std::array<std::int64_t, 128> last_press{};
        last_press.fill(toonval::app::kNeverPressed);
        std::array<std::uint8_t, 128> last_velocity{};
        LatencyLog latency;
        // The built-in piano. MIDI notes play it only with
        // --audio-latency-log, to measure press-to-sound: a connected piano makes its own sound.
        toonval::app::AudioOut audio;
        audio.start(opt.audio);

        // Saved state first: the look, the three user slots, the app settings and the session.
        toonval::app::Persistence persist(settings_directory(opt.settings_dir));
        toonval::app::UiModel model;
        std::string status_message = persist.load(model);
        std::int64_t status_since_ns = midi_clock_ns();

        // Songs: the library folders (the per-user Songs folder, and the repository's test songs
        // in a developer build), the practice history, and the controller that plays them.
        const std::filesystem::path data_dir = settings_directory(opt.settings_dir);
        std::vector<std::filesystem::path> song_roots{data_dir / "Songs"};
#ifdef TOONVAL_DEV_SONGS_DIR
        song_roots.emplace_back(TOONVAL_DEV_SONGS_DIR);
#endif
        toonval::app::SongLibrary library(song_roots);
        library.scan();
        toonval::app::RunsFile runs(data_dir);
        if (std::string m = runs.load(); !m.empty()) {
            status_message = std::move(m);
        }
        toonval::app::SongController songs;
        std::optional<std::size_t> song_index;
        std::string results_text; // the last run's results, shown until the song plays again
        const auto song_options = [&] {
            toonval::app::SongOptions o;
            o.hit_window_ms = model.app.hit_window_ms;
            o.latency_ms = model.app.latency_correction_ms;
            o.lookahead_s = model.look.visible_seconds;
            o.resume_countdown_s = model.app.resume_countdown_s;
            o.skip_back_seconds = model.app.skip_back_seconds;
            o.skip_forward_seconds = model.app.skip_forward_seconds;
            return o;
        };
        songs.set_options(song_options(), midi_clock_ns());
        const auto open_song = [&](const std::filesystem::path& path) -> bool {
            toonval::app::SongFileLoad load = toonval::app::load_song_file(path);
            if (!load.ok) {
                status_message = load.error;
                status_since_ns = midi_clock_ns();
                return false;
            }
            load.song.library_path = library.relative_path(path);
            const bool split = load.song.hands_split;
            const std::string title = load.song.title;
            songs.load(std::move(load.song), midi_clock_ns());
            song_index = library.index_of(path);
            results_text.clear();
            status_message = "Opened " + title + (split ? ", hands split by pitch" : "");
            status_since_ns = midi_clock_ns();
            return true;
        };

        // The saved session: plan_restore decides what of it comes back.
        if (!opt.song_path.empty()) {
            model.session = toonval::plan_restore(model.session, false, false, nullptr).state;
            songs.apply_session(model.session, midi_clock_ns());
            open_song(opt.song_path);
        } else if (model.session.has_song()) {
            const std::optional<std::filesystem::path> path =
                library.resolve(model.session.song_path);
            std::vector<std::uint8_t> bytes;
            const bool found =
                path.has_value() && toonval::app::read_song_bytes(*path, bytes, nullptr);
            toonval::app::SongFileLoad load;
            if (found) {
                load = toonval::app::load_song_bytes(bytes, path->filename().string());
            }
            const bool hash_matches = load.ok && (model.session.song_hash == 0 ||
                                                  load.song.hash == model.session.song_hash);
            const toonval::RestorePlan plan = toonval::plan_restore(
                model.session, found, hash_matches, load.ok ? &load.song.song : nullptr);
            const std::string name = model.session.song_path;
            model.session = plan.state;
            if (plan.load_song) {
                load.song.library_path = model.session.song_path;
                songs.load(std::move(load.song), midi_clock_ns());
                song_index = library.index_of(*path);
            }
            songs.apply_session(model.session, midi_clock_ns());
            switch (plan.outcome) {
            case toonval::RestoreOutcome::SongMissing:
                status_message = "Last song not found: " + name;
                break;
            case toonval::RestoreOutcome::SongUnreadable:
                status_message = "Last song can't be read: " + name;
                break;
            case toonval::RestoreOutcome::SongChanged:
                status_message = "Last song changed since; starting it from the top";
                break;
            case toonval::RestoreOutcome::Restored:
            case toonval::RestoreOutcome::NoSong:
                break;
            }
            status_since_ns = midi_clock_ns();
        } else {
            // Free play restores nothing but the preferences.
            model.session = toonval::plan_restore(model.session, false, false, nullptr).state;
            songs.apply_session(model.session, midi_clock_ns());
        }
        {
            const std::int64_t t = midi_clock_ns();
            if (opt.mode) {
                songs.set_mode(*opt.mode, t);
            }
            if (opt.hands) {
                songs.set_practice_hands(*opt.hands, t);
            }
            if (opt.song_sound) {
                songs.set_song_sound(*opt.song_sound, t);
            }
        }
        apply_look_options(opt, model.look);
        persist.adopt(model); // the options are for this run; only a later change is saved

        toonval::app::Scene scene(renderer, model.look);
        toonval::LookSettings applied_look = model.look;
        toonval::app::Ui ui(window, renderer);
        // The bar's transport, the song overview and the resume countdown act on `songs`.
        toonval::app::SongTransport song(songs);
        if (opt.panel) {
            ui.open_panel(*opt.panel);
        }
        const auto show_message = [&](std::string text) {
            if (!text.empty()) {
                status_message = std::move(text);
                status_since_ns = midi_clock_ns();
            }
        };
        std::int64_t last_activity_ns = INT64_MIN / 2;
        long frame_index = 0;
        char out_of_range_line[64] = "";
        char dropped_line[64] = "";
        // Steps effects down when frames run over the refresh period.
        toonval::GovernorConfig governor_config;
        governor_config.budget_ns = frame_ns;
        toonval::EffectsGovernor governor(governor_config);
        char effects_line[64] = "";
        std::int64_t last_present_ns = midi_clock_ns();

        // The part of the keyboard on screen (Settings > View > Key size): the core picks the
        // window each frame and the follower slides the drawn one there.
        toonval::KeyViewFollower view_follower;
        std::optional<toonval::KeyRange> view_range;
        std::vector<int> held_notes;
        held_notes.reserve(128);
        std::optional<KeyboardLayout> view_layout;
        toonval::KeyViewFrame view_frame;
        struct ViewInputs {
            int width = -1;
            toonval::KeySize size = toonval::KeySize::FitKeyboard;
            int keyboard_lowest = -1;
            int keyboard_highest = -1;
            float px_per_mm = 0.0f;
            bool operator==(const ViewInputs&) const = default;
        } view_inputs;
        std::int64_t view_ns = 0;

        // Playing without a MIDI keyboard: mouse, touch and typing become MidiEvents tagged as
        // virtual, go through on_event like MIDI, and also to the piano's MIDI output, or to the
        // built-in piano when no piano is connected.
        VirtualInput virtual_input;
        MidiOutput piano_out;
        toonval::app::BuiltinPianoSink builtin_sink(audio.sink());
        std::vector<MidiEvent> virtual_events;
        virtual_events.reserve(64);
        std::vector<std::string> input_names;
        std::array<std::string_view, 128> typing_labels{};
        toonval::KeyRange typing_keyboard{0, 0};
        int shown_octave_shift = virtual_input.typing().octave_shift();
        int shown_velocity = virtual_input.typing().velocity();
        const auto active_sink = [&]() -> toonval::SoundSink& {
            return piano_out.connected() ? static_cast<toonval::SoundSink&>(piano_out)
                                         : static_cast<toonval::SoundSink&>(builtin_sink);
        };

        // The song's sound goes where the player's does: the piano when connected, else the
        // built-in piano. A change of output silences what the old one had of the song.
        toonval::app::BuiltinSongSink builtin_song(audio.sink());
        bool song_to_midi = false;
        std::vector<toonval::PlaybackMessage> song_messages;
        song_messages.reserve(256);
        const auto send_song = [&](std::vector<toonval::PlaybackMessage>& messages) {
            for (const toonval::PlaybackMessage& m : messages) {
                if (song_to_midi) {
                    piano_out.play(m);
                } else {
                    builtin_song.play(m);
                }
            }
            messages.clear();
        };
        // The player's note events of this frame, for the evaluators, in arrival order.
        std::vector<MidiEvent> frame_events;
        frame_events.reserve(64);
        Robot robot;
        std::vector<MidiEvent> robot_events;
        bool panel_was_open = false;
        int logged_state = -1;
        bool logged_song = false;
        std::int64_t last_skip_ns = 0;
        std::vector<toonval::VisibleNote> visible_notes;
        std::vector<toonval::app::SongBar> song_bars;
        std::vector<toonval::app::GradePopup> grade_popups;
        std::array<std::uint8_t, 128> hint_hands{};
        std::vector<int> upcoming;
        std::string song_status;
        if (opt.seek_s >= 0.0) {
            songs.seek(opt.seek_s, midi_clock_ns());
        }
        if (opt.play) {
            songs.play(midi_clock_ns());
        }

        // Saves finished runs and shows their results.
        const auto save_runs = [&] {
            for (toonval::app::FinishedRun& f : songs.take_finished_runs()) {
                const std::time_t wall = std::time(nullptr);
                toonval::RunRecord r = f.record;
                r.end_utc_s = static_cast<std::int64_t>(wall);
                r.start_utc_s = r.end_utc_s - (f.ended_ns - f.started_ns) / 1'000'000'000;
                r.local_day = toonval::local_day_from_utc(
                    r.start_utc_s, local_utc_offset_s(static_cast<std::time_t>(r.start_utc_s)));
                r = toonval::sanitized(r);
                const toonval::AttemptChange change =
                    toonval::compare_with_last_attempt(runs.runs(), r);
                std::string error;
                if (!runs.append(r, &error)) {
                    show_message("Could not save the run: " + error);
                }
                results_text = toonval::app::results_line(r, change);
                std::printf("%s (graded %d, hit %d, wrong %d)\n", results_text.c_str(),
                            r.all.graded, r.all.hit, r.wrong);
            }
        };

        // The shortcuts while the UI has none. None of them is a
        // typing key, so playing on the computer keyboard never triggers one.
        const auto shortcut = [&](const SDL_KeyboardEvent& key) {
            const std::int64_t now = midi_clock_ns();
            // Auto-repeat acts only for skip (Left/Right, throttled below) and speed (Up/Down);
            // every other shortcut acts once per press.
            const bool repeats =
                key.scancode == SDL_SCANCODE_LEFT || key.scancode == SDL_SCANCODE_RIGHT ||
                key.scancode == SDL_SCANCODE_UP || key.scancode == SDL_SCANCODE_DOWN;
            if (key.repeat && !repeats) {
                return;
            }
            switch (key.scancode) {
            case SDL_SCANCODE_SPACE:
                songs.toggle_play(now);
                break;
            case SDL_SCANCODE_LEFT:
            case SDL_SCANCODE_RIGHT: {
                // One step per tap (Settings: skip back / forward seconds); held, the key's
                // repeats step again, at most five times a second.
                if (!key.repeat || now - last_skip_ns >= 200'000'000) {
                    if (key.scancode == SDL_SCANCODE_RIGHT) {
                        songs.skip_forward(now);
                    } else {
                        songs.skip_back(now);
                    }
                    last_skip_ns = now;
                }
                break;
            }
            case SDL_SCANCODE_HOME:
                songs.replay(now);
                break;
            case SDL_SCANCODE_UP:
                songs.change_speed(toonval::kSpeedStepPercent, now);
                break;
            case SDL_SCANCODE_DOWN:
                songs.change_speed(-toonval::kSpeedStepPercent, now);
                break;
            case SDL_SCANCODE_LEFTBRACKET:
                songs.set_loop_start_here(now);
                break;
            case SDL_SCANCODE_RIGHTBRACKET:
                songs.set_loop_end_here(now);
                break;
            case SDL_SCANCODE_BACKSLASH:
                songs.clear_loop(now);
                break;
            case SDL_SCANCODE_1:
                songs.set_practice_hands(toonval::HandFilter::Right, now);
                break;
            case SDL_SCANCODE_2:
                songs.set_practice_hands(toonval::HandFilter::Left, now);
                break;
            case SDL_SCANCODE_3:
                songs.set_practice_hands(toonval::HandFilter::Both, now);
                break;
            case SDL_SCANCODE_M:
                songs.cycle_song_sound(now);
                show_message(std::string("Song sound: ") +
                             toonval::app::song_sound_name(songs.song_sound()));
                break;
            case SDL_SCANCODE_Q:
                songs.toggle_mode(now);
                break;
            case SDL_SCANCODE_PAGEDOWN:
            case SDL_SCANCODE_PAGEUP:
                if (const auto next =
                        library.step(song_index, key.scancode == SDL_SCANCODE_PAGEDOWN ? 1 : -1)) {
                    open_song(library.songs()[*next].path);
                }
                break;
            default:
                break;
            }
        };

        // One event, from a MIDI port or from the player's own presses: keys, trail, chord,
        // latency.
        const auto on_event = [&](const MidiEvent& ev) {
            const KeyTransition t = apply_event(session, ev);
            trail.apply(t, ev.time_ns);
            if (t.type != KeyTransitionType::None) {
                last_activity_ns = ev.time_ns;
            }
            if (t.type == KeyTransitionType::Pressed) {
                last_press[t.note] = ev.time_ns;
                last_velocity[t.note] = t.velocity;
                if (latency_log && session.layout.range().contains(t.note)) {
                    latency.note_on(ev.time_ns);
                }
            }
            if (ev.is_virtual()) {
                toonval::forward_to_sink(active_sink(), ev);
            }
            frame_events.push_back(ev);
            if (opt.script.active() && ev.type == toonval::MidiEventType::NoteOn) {
                std::printf("player note-on %d (source %d)\n", ev.data1,
                            static_cast<int>(ev.source));
            }
        };

        std::int64_t next_frame_ns = midi_clock_ns();
        bool running = true;
        RedrawHook hook;
        hook.window = SDL_GetWindowID(window);

        // Waits for a free back buffer: Metal blocks for one in the first draw after a present, and
        // that wait has to come before the MIDI drain, not after. Returns when the frame's work
        // began.
        const auto begin_frame = [&]() {
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderClear(renderer);
            SDL_FlushRenderer(renderer);
            return midi_clock_ns();
        };

        const auto handle_events = [&]() {
            hook.polling = true;
            opt.script.before_events(window, frame_index);
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                const bool used_by_ui = ui.process_event(event, midi_clock_ns());
                audio.handle_event(event);
                const bool playing_key =
                    virtual_input.handle(event, window, used_by_ui, virtual_events);
                if (event.type == SDL_EVENT_KEY_DOWN && !playing_key && !used_by_ui &&
                    (event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) == 0) {
                    shortcut(event.key);
                }
                if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
                    songs.lost_focus(midi_clock_ns()); // nothing of the song keeps sounding
                    if (opt.script.active()) {
                        std::printf("frame %ld: focus lost\n", frame_index);
                    }
                }
                if (event.type == SDL_EVENT_QUIT) {
                    running = false;
                } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE &&
                           !used_by_ui) {
                    // Escape closes the settings panel first, then quits.
                    if (ui.panel_open()) {
                        ui.close_panel();
                    } else {
                        running = false;
                    }
                }
            }
            hook.polling = false;
            // Say so when the typing keys' octave or loudness changed.
            const toonval::ComputerKeyboard& typing = virtual_input.typing();
            if (typing.octave_shift() != shown_octave_shift ||
                typing.velocity() != shown_velocity) {
                shown_octave_shift = typing.octave_shift();
                shown_velocity = typing.velocity();
                show_message(
                    "Typing keys: A plays " +
                    toonval::note_name(typing.base_note(), false, model.look.octave_offset()) +
                    ", loudness " + std::to_string(typing.velocity()));
            }
        };

        // Everything after the events: drain MIDI, move the view, draw, present. `live` is a redraw
        // from inside the OS's window-resize loop, which must not feed the effects governor (its
        // frame times are the resize's, not the scene's).
        const auto finish_frame = [&](std::int64_t work_start_ns, bool live) {
            // Drain MIDI last, right before drawing, so a key press is in this frame.
            bool label_changed = false;
            const KeyTransition unplugged =
                apply_port_changes(session, *midi, midi->update(), label_changed);
            if (apply_keyboard_choice(session, model.session)) {
                label_changed = true;
            }
            if (unplugged.type == KeyTransitionType::AllReleased) {
                trail.apply(unplugged, midi_clock_ns());
                chord.reset();
                last_press.fill(toonval::app::kNeverPressed);
            }
            if (session.layout.range().lowest != typing_keyboard.lowest ||
                session.layout.range().highest != typing_keyboard.highest) {
                typing_keyboard = session.layout.range();
                virtual_input.set_keyboard(typing_keyboard);
            }
            // The player's own presses first (they were stamped as the OS delivered them), then
            // MIDI.
            for (const MidiEvent& ev : virtual_events) {
                on_event(ev);
            }
            virtual_events.clear();
            midi->drain([&](const MidiEvent& ev) {
                if (opt.audio.latency_log) {
                    audio.play_midi(ev);
                }
                on_event(ev);
            });
            if (label_changed) {
                input_names = midi->open_names();
            }
            piano_out.update(input_names, midi_clock_ns(), label_changed);
            const std::int64_t now = midi_clock_ns();
            audio.update(now);

            // The song: settings, the menu, the player's notes, then its MIDI out.
            songs.set_keyboard(session.layout.range(), now);
            songs.set_options(song_options(), now);
            if (const bool open = ui.panel_open(); open != panel_was_open) {
                panel_was_open = open;
                if (opt.script.active()) {
                    std::printf("frame %ld: settings %s\n", frame_index, open ? "open" : "closed");
                }
                if (open) {
                    songs.menu_opened(now);
                } else {
                    songs.menu_closed(now);
                }
            }
            if (opt.robot) {
                robot_events.clear();
                robot.update(songs, now, robot_events);
                for (const MidiEvent& ev : robot_events) {
                    on_event(ev);
                }
            }
            songs.update(now, frame_events);
            frame_events.clear();
            song.update(now);
            if (opt.script.active()) {
                // Scripted checks: say when the song's state changes, to line up with captures.
                const auto state = static_cast<int>(songs.state());
                if (state != logged_state || songs.has_song() != logged_song) {
                    logged_state = state;
                    logged_song = songs.has_song();
                    std::printf("frame %ld, %.3f s: song %s, state %d, at %.2f s\n", frame_index,
                                static_cast<double>(now) / 1e9,
                                songs.has_song() ? songs.song().title.c_str() : "none", state,
                                songs.position_s());
                }
            }
            if (piano_out.connected() != song_to_midi) {
                if (!song_to_midi) {
                    builtin_song.all_off();
                }
                song_to_midi = piano_out.connected();
                songs.silence(now);
            }
            songs.take_due_messages(now, song_messages);
            send_song(song_messages);
            save_runs();
            if (songs.playing()) {
                results_text.clear();
            } else if (const toonval::SessionState saved = songs.session_state(model.session);
                       !(saved == model.session)) {
                model.session = saved; // saved while paused; a playing song moves every frame
            }
            if (!(model.look == applied_look)) {
                scene.set_settings(model.look);
                applied_look = model.look;
            }
            const std::int64_t trail_span_ns = scene.trail_span_ns();
            trail.prune(now, trail_span_ns);
            trail.visible(now, trail_span_ns, bars);
            session.keys.held_notes(held);
            label = chord.update(held, now);

            out_of_range_line[0] = '\0';
            if (session.out_of_range > 0) {
                std::snprintf(out_of_range_line, sizeof out_of_range_line,
                              "%llu %s outside the keyboard's range",
                              static_cast<unsigned long long>(session.out_of_range),
                              session.out_of_range == 1 ? "note" : "notes");
            }
            dropped_line[0] = '\0';
            if (const std::uint64_t dropped = midi->dropped(); dropped > 0) {
                std::snprintf(dropped_line, sizeof dropped_line, "%llu MIDI events dropped",
                              static_cast<unsigned long long>(dropped));
            }

            if (!status_message.empty() && now - status_since_ns > kStatusMessageNs) {
                status_message.clear();
            }
            // The top row, the bar and the settings go on top of the finished scene.
            int out_w = 0;
            int out_h = 0;
            SDL_GetRenderOutputSize(renderer, &out_w, &out_h);
            toonval::app::UiFrameInfo info;
            info.width_px = static_cast<float>(out_w);
            info.height_px = static_cast<float>(out_h);
            info.px_per_mm = toonval::app::display_px_per_mm(window);
            info.layout =
                toonval::compute_layout(info.width_px, info.height_px, info.px_per_mm, model.look);
            info.now_ns = now;
            info.playing = session.keys.held_count() > 0 ||
                           now - last_activity_ns < kPlayingHoldNs || songs.playing();

            // The song's notes on screen, in their hands' colours; the hands not practised faint.
            song_bars.clear();
            upcoming.clear();
            grade_popups.clear();
            hint_hands.fill(0);
            if (songs.has_song()) {
                songs.visible_notes(model.look.visible_seconds, visible_notes);
                const auto& notes = songs.song().song.notes;
                const toonval::HandFilter practised = songs.practice_hands();
                for (const toonval::VisibleNote& v : visible_notes) {
                    const toonval::Note& n = notes[v.index];
                    if (!v.playable || n.drum) {
                        continue;
                    }
                    const bool mine =
                        practised == toonval::HandFilter::Both ||
                        (practised == toonval::HandFilter::Left && n.hand == toonval::Hand::Left) ||
                        (practised == toonval::HandFilter::Right && n.hand == toonval::Hand::Right);
                    song_bars.push_back({static_cast<std::uint8_t>(v.pitch), n.velocity, n.hand,
                                         static_cast<float>(v.bottom), static_cast<float>(v.top),
                                         mine, static_cast<std::int8_t>(songs.grade_of(v.index)),
                                         static_cast<std::int8_t>(v.folded ? v.fold_octaves : 0)});
                    // The view keeps the notes about to be played in sight.
                    if (mine && v.top > 0.0 && v.bottom < 0.5) {
                        upcoming.push_back(v.pitch);
                    }
                }
                if (songs.mode() == toonval::PracticeMode::Flow) {
                    for (const toonval::app::GradeMark& g : songs.recent_grades()) {
                        grade_popups.push_back(
                            {static_cast<std::uint8_t>(g.key & 127), g.grade,
                             static_cast<float>(now - g.at_ns) /
                                 static_cast<float>(toonval::app::kGradeShowNs)});
                    }
                }
                for (const auto& k : songs.waiting_keys()) {
                    hint_hands[static_cast<std::size_t>(k.key & 127)] =
                        static_cast<std::uint8_t>(static_cast<int>(k.hand) + 1);
                }
            }
            song_status = songs.status_line();
            info.status_label = session.label;

            // The window on the keyboard. A resize, a new key size or a different keyboard jumps
            // straight there (the layout must be right in the very frame); only the music moves it
            // smoothly.
            held_notes.assign(held.begin(), held.end());
            toonval::KeyViewParams view_params;
            view_params.keyboard = session.layout.range();
            view_params.width_px = info.width_px;
            view_params.px_per_mm = info.px_per_mm;
            view_params.size = model.look.key_size;
            view_params.held = held_notes;
            view_params.upcoming = upcoming;
            view_params.song = songs.key_range();
            view_params.current = view_range;
            const toonval::KeyViewTarget view_target = toonval::compute_key_view(view_params);
            view_range = view_target.range;
            const ViewInputs inputs{out_w, model.look.key_size, session.layout.range().lowest,
                                    session.layout.range().highest, info.px_per_mm};
            if (!view_follower.has_position() || !(inputs == view_inputs)) {
                view_follower.snap(session.layout.range(), view_target.range);
            } else {
                view_follower.advance(session.layout.range(), view_target.range,
                                      static_cast<double>(now - view_ns) / 1e9);
            }
            view_inputs = inputs;
            view_ns = now;
            view_frame = view_follower.frame();
            if (!view_layout || view_layout->range().lowest != view_frame.layout_range.lowest ||
                view_layout->range().highest != view_frame.layout_range.highest) {
                view_layout.emplace(view_frame.layout_range);
            }

            const bool show_typing = model.look.show_typing_keys;
            if (show_typing) {
                for (int note = 0; note < 128; ++note) {
                    typing_labels[static_cast<std::size_t>(note)] =
                        virtual_input.typing().label_for_note(note);
                }
            }
            // The settings sheet covers everything under the top row: no key presses through it.
            virtual_input.set_geometry(*view_layout, view_frame, info.layout.key_area,
                                       ui.panel_open() ? 0.0f : -1.0f);

            if (opt.debug_effects) {
                std::snprintf(
                    effects_line, sizeof effects_line, "effects: %s",
                    std::string(toonval::effects_level_name(scene.effects_level())).c_str());
            }
            std::string_view message = dropped_line;
            if (!status_message.empty()) {
                message = status_message;
            }
            info.status_notes = {song_status, message, effects_line, out_of_range_line};
            // The keyboard prompt is for free play: a loaded song, and the countdown ring that
            // sits in the middle of the note area, take its place.
            const bool keyboard_prompt =
                session.open_inputs == 0 && !songs.has_song() && !songs.counting_down();
            toonval::app::FrameInput frame{*view_layout,
                                           view_frame,
                                           session.keys,
                                           bars,
                                           last_press,
                                           last_velocity,
                                           now,
                                           label,
                                           keyboard_prompt,
                                           show_typing ? &typing_labels : nullptr};
            frame.song_bars = song_bars;
            frame.grades = grade_popups;
            frame.hint_hands = songs.waiting() ? &hint_hands : nullptr;
            frame.trail_alpha = songs.has_song() ? 0.4f : 1.0f;
            frame.results = results_text;
            scene.draw(frame);

            ui.draw(model, song, info);
            show_message(ui.take_message());
            show_message(persist.update(model, now));
            if (opt.script.after_draw(renderer, frame_index)) {
                running = false;
            }
            ++frame_index;
            SDL_RenderPresent(renderer);
            const std::int64_t presented_ns = midi_clock_ns();
            if (latency_log) {
                latency.presented(presented_ns);
            }
            if (!live) {
                scene.set_effects_level(governor.add_frame(toonval::frame_cost_ns(
                    presented_ns - work_start_ns, presented_ns - last_present_ns, frame_ns)));
            }
            last_present_ns = presented_ns;
        };

        // While the OS runs its own loop for a window drag (macOS live resize, the Windows move and
        // size loop), SDL_PollEvent is not called and the window would show a stretched old frame.
        // The event watch is called inside that loop, so it redraws from there: layout, keys,
        // notes, text and controls follow the new size in the same frame.
        hook.draw = [&] {
            if (running) {
                finish_frame(begin_frame(), true);
            }
        };
        SDL_AddEventWatch(on_window_event_watch, &hook);

        while (running) {
            if (!vsync) {
                // Sleep before draining, so the events read are as fresh as possible.
                const std::int64_t now = midi_clock_ns();
                if (next_frame_ns > now) {
                    SDL_DelayPrecise(static_cast<Uint64>(next_frame_ns - now));
                    next_frame_ns += frame_ns;
                } else {
                    next_frame_ns = now + frame_ns; // fell behind: start the cadence again
                }
            }
            hook.drawing = true;
            const std::int64_t work_start_ns = begin_frame();
            handle_events();
            finish_frame(work_start_ns, false);
            hook.drawing = false;
        }
        SDL_RemoveEventWatch(on_window_event_watch, &hook);
        // Nothing may keep sounding on the piano after the app is gone: the song first (its run is
        // saved, its notes stopped, every queued message sent), then the player's notes.
        {
            const std::int64_t now = midi_clock_ns();
            if (const toonval::SessionState saved = songs.session_state(model.session);
                !(saved == model.session)) {
                model.session = saved;
            }
            songs.shutdown(now, song_messages);
            send_song(song_messages);
            builtin_song.all_off();
            save_runs();
        }
        virtual_events.clear();
        virtual_input.release_all(virtual_events);
        for (const MidiEvent& ev : virtual_events) {
            toonval::forward_to_sink(active_sink(), ev);
        }
        piano_out.all_off();
        persist.flush(model);
    }

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

// --- Headless MIDI log --------------------------------------------------------------------------

volatile std::sig_atomic_t g_stop = 0;

// From --octaves: added to the octave number in every note name the log prints.
int g_octave_offset = 0;

void on_signal(int /*sig*/) { g_stop = 1; }

const char* event_name(toonval::MidiEventType type) {
    switch (type) {
    case toonval::MidiEventType::NoteOn:
        return "note-on";
    case toonval::MidiEventType::NoteOff:
        return "note-off";
    case toonval::MidiEventType::ControlChange:
        return "cc";
    case toonval::MidiEventType::Other:
        break;
    }
    return "other";
}

void log_transition(const Session& s, const KeyTransition& t) {
    switch (t.type) {
    case KeyTransitionType::Pressed:
        std::printf("    key %s down%s, %d held\n",
                    toonval::note_name(t.note, false, g_octave_offset).c_str(),
                    s.layout.range().contains(t.note) ? "" : " (outside the keyboard's range)",
                    s.keys.held_count());
        break;
    case KeyTransitionType::Released:
        std::printf("    key %s up, %d held\n",
                    toonval::note_name(t.note, false, g_octave_offset).c_str(),
                    s.keys.held_count());
        break;
    case KeyTransitionType::AllReleased:
        std::printf("    all keys up (%d released)\n", t.released_count);
        break;
    case KeyTransitionType::None:
        break;
    }
}

// Prints decoded events and key transitions to stdout without opening a window, for checking MIDI
// where no display is available. Runs until Ctrl-C.
int run_midi_log(const toonval::app::AudioOptions& audio_options) {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    auto midi = std::make_unique<MidiInput>();
    Session session;
    std::printf("Listening for MIDI. Ctrl-C to stop.\n");

    // --audio-latency-log: the MIDI notes also play the built-in piano, to measure press-to-sound
    // without a window (e.g. with `just midi-demo`).
    toonval::app::AudioOut audio;
    if (audio_options.latency_log) {
        audio.start(audio_options);
    }

    bool sustain = false;
    while (g_stop == 0) {
        MidiInput::pump_system_events();
        const auto changes = midi->update();
        const int held_before = session.keys.held_count();
        bool label_changed = false;
        apply_port_changes(session, *midi, changes, label_changed);
        if (label_changed || changes.closed > 0) {
            std::printf("keyboard: %s (range %d..%d)", session.label.c_str(),
                        session.layout.range().lowest, session.layout.range().highest);
            for (const auto& name : midi->open_names()) {
                std::printf(" [%s]", name.c_str());
            }
            std::printf("\n");
            if (changes.closed > 0) {
                std::printf("    input removed, keys cleared (%d were held)\n", held_before);
            }
        }

        midi->drain([&](const MidiEvent& ev) {
            audio.play_midi(ev);
            const double delay_ms =
                static_cast<double>(toonval::app::midi_clock_ns() - ev.time_ns) / 1e6;
            std::printf("t=%lld ns (+%.3f ms in queue) %s ch%d %d %d\n",
                        static_cast<long long>(ev.time_ns), delay_ms, event_name(ev.type),
                        ev.channel + 1, ev.data1, ev.data2);
            log_transition(session, apply_event(session, ev));
            if (session.keys.sustain_down() != sustain) {
                sustain = session.keys.sustain_down();
                std::printf("    sustain %s\n", sustain ? "down" : "up");
            }
        });
        audio.update(toonval::app::midi_clock_ns());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    audio.report(toonval::app::midi_clock_ns());

    std::printf("stopped. dropped %llu events, %llu presses outside the range\n",
                static_cast<unsigned long long>(midi->dropped()),
                static_cast<unsigned long long>(session.out_of_range));
    return 0;
}

void print_usage() {
    std::printf(
        "usage: toonval [--song FILE] [--mode flow|wait] [--hands both|left|right]\n"
        "               [--song-sound mute|other-hand|both-hands] [--play] [--seek S]\n"
        "               [--midi-log | --latency-log] [--look PRESET] [--style STYLE]\n"
        "               [--colors SCHEME] [--names MODE] [--octaves NUMBERING] [--window WxH]\n"
        "               [--key-size SIZE] [--show-typing-keys] [--settings-dir DIR]\n"
        "               [--debug-effects] [--sound-font SF2] [--audio-latency-log]\n"
        "       toonval --synth-test [--audio-latency-log] [--sound-font SF2]\n"
        "       toonval --bench | --bench-all [--style STYLE] [--notes N] [--seconds S]\n"
        "               [--size WxH | --window WxH] [--no-degrade] [--screenshot PNG [--at S]]\n"
        "  --song FILE    open a song: .mid, .musicxml/.xml, .mxl or .mscz. Without it the\n"
        "                 last session's song comes back, paused where you left it\n"
        "  --mode MODE    flow (the song keeps going, each note graded) or wait (the song\n"
        "                 waits at each chord until you play it)\n"
        "  --hands HANDS  the hands you practise: both, left or right\n"
        "  --song-sound S what you hear of the song: mute (default), other-hand, both-hands\n"
        "  --play         start playing the song at once\n"
        "  --seek S       start the song S seconds in\n"
        "  --robot        developer: a stand-in player presses the practised notes\n"
        "song keys (none of them plays a note):\n"
        "  Space play/pause   Left/Right skip back/forward (skip_back_seconds and\n"
        "  skip_forward_seconds in [app], 1..30 s, default 5; hold to repeat)   Home replay\n"
        "  Up/Down speed +/-5%% (25..150)   [ ] loop start/end at this bar   \\ clear loop\n"
        "  1 right hand  2 left hand  3 both hands   M song sound (mute, other hand, both)\n"
        "  Q Flow/Wait mode   PageDown/PageUp next/previous song in the library\n"
        "  --midi-log     print MIDI events and key changes to stdout, no window\n"
        "  --latency-log  every 5 s, print min/median/max time from note-on to the\n"
        "                 frame presented with it\n"
        "  --synth-test   play a scale, a pedalled arpeggio and a loud chord on the built-in\n"
        "                 piano, no window, then print the audio device and its counters\n"
        "  --audio-latency-log  every 5 s, print min/median/max time from a note-on to the\n"
        "                 first sample of it the audio callback wrote, plus the device's\n"
        "                 latency; MIDI notes play the built-in piano for this\n"
        "  --sound-font SF2  piano samples to use instead of toonval-piano.sf2 beside the app\n"
        "  --look PRESET  start from a built-in look: default, classic, neon-night, glass,\n"
        "                 frosted-pastel, high-contrast, minimal (for this run only; the\n"
        "                 settings panel changes and saves the look)\n"
        "  --names MODE   note names, off, letters or solfege, on blocks and keys (this run)\n"
        "  --octaves NUMBERING  octave numbers everywhere: scientific (middle C is C4, the\n"
        "                 default) or yamaha (middle C is C3), the log included\n"
        "  --window WxH   open at exactly this many pixels, e.g. 800x480\n"
        "  --key-size SIZE  how wide the keys are (this run): fit-keyboard (default), fit-song,\n"
        "                 small (12 mm), medium (16), large (20), extra-large (24); the fixed\n"
        "                 sizes show part of the keyboard and follow the notes you play\n"
        "  --show-typing-keys  show the computer-key letters on the piano keys (this run)\n"
        "  --settings-dir DIR  keep settings.conf in DIR instead of the per-user folder\n"
        "  --style STYLE  note style (this run): filled, glassy, frosted, shiny, ghost, neon\n"
        "  --colors SCHEME  note colours (this run): hand, pitch, octave, velocity, single\n"
        "  --debug-effects  show the effects level (auto-degrade) in the status line\n"
        "  --bench        draw a synthetic scene without MIDI and print each frame's CPU+GPU\n"
        "                 time (min/median/p95/max) and the effects level reached; without\n"
        "                 --look every effect is on, motion background included. The saved\n"
        "                 settings are not read\n"
        "  --bench-all    the same for all six styles in turn (one with --style)\n"
        "  --notes N      notes on screen in the bench (default 200)\n"
        "  --seconds S    measured seconds per style (default 5)\n"
        "  --size WxH     pixels the bench draws (default 1920x1080), offscreen if the window\n"
        "                 is smaller; --window WxH draws straight into a window that size\n"
        "  --no-degrade   bench at full effects, auto-degrade off\n"
        "  --screenshot PNG  bench scene stepped at 60 Hz to just after a chord near --at S\n"
        "                 seconds (default 3), saved as an exact PNG\n"
        "  --panel PAGE   open the settings on quick, look, names, effects, view, presets or\n"
        "                 practice\n"

        "developer scripting, on a frame number (window points):\n"
        "  --tap F:X,Y  --drag F:X,Y:X2,Y2  --move F:X,Y  --type F:TEXT  --shot F:FILE.png\n"
        "  --key F:NAME[:N]  --press F:X,Y[:N]  --resize F:WxH  --info F  --quit-frame F\n");
}

// Preset names as typed on the command line.
bool parse_preset(const char* text, toonval::PresetId* out) {
    struct Entry {
        const char* name;
        toonval::PresetId id;
    };
    static const Entry kEntries[] = {
        {"default", toonval::PresetId::Default},
        {"classic", toonval::PresetId::Classic},
        {"neon-night", toonval::PresetId::NeonNight},
        {"glass", toonval::PresetId::Glass},
        {"frosted-pastel", toonval::PresetId::FrostedPastel},
        {"high-contrast", toonval::PresetId::HighContrast},
        {"minimal", toonval::PresetId::MinimalReduced},
    };
    for (const Entry& e : kEntries) {
        if (std::strcmp(text, e.name) == 0) {
            *out = e.id;
            return true;
        }
    }
    return false;
}

} // namespace

int main(int argc, char* argv[]) {
    bool midi_log = false;
    bool synth_test = false;
    WindowOptions opt;
    bool bench = false;
    toonval::app::BenchOptions bench_options;
    // --octaves also sets the numbering of the headless log.
    toonval::LookSettings log_look;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--midi-log") == 0) {
            midi_log = true;
        } else if (std::strcmp(argv[i], "--latency-log") == 0) {
            opt.latency_log = true;
        } else if (std::strcmp(argv[i], "--audio-latency-log") == 0) {
            opt.audio.latency_log = true;
        } else if (std::strcmp(argv[i], "--synth-test") == 0) {
            synth_test = true;
        } else if (std::strcmp(argv[i], "--sound-font") == 0 && i + 1 < argc) {
            opt.audio.sound_font = argv[++i];
        } else if (std::strcmp(argv[i], "--look") == 0 && i + 1 < argc) {
            toonval::PresetId id{};
            if (!parse_preset(argv[++i], &id)) {
                std::fprintf(stderr, "unknown look: %s\n", argv[i]);
                print_usage();
                return 2;
            }
            opt.preset = id;
        } else if (std::strcmp(argv[i], "--style") == 0 && i + 1 < argc) {
            opt.style = argv[++i];
            toonval::NoteStyle parsed{};
            if (!toonval::parse_note_style(opt.style, &parsed)) {
                std::fprintf(stderr, "unknown style: %s\n", opt.style);
                print_usage();
                return 2;
            }
        } else if (std::strcmp(argv[i], "--colors") == 0 && i + 1 < argc) {
            opt.colors = argv[++i];
            toonval::ColorScheme parsed{};
            if (!parse_colors(opt.colors, &parsed)) {
                std::fprintf(stderr, "unknown colours: %s\n", opt.colors);
                print_usage();
                return 2;
            }
        } else if (std::strcmp(argv[i], "--key-size") == 0 && i + 1 < argc) {
            toonval::KeySize parsed{};
            if (!parse_key_size(argv[++i], &parsed)) {
                std::fprintf(stderr, "unknown key size: %s\n", argv[i]);
                print_usage();
                return 2;
            }
            opt.key_size = parsed;
        } else if (std::strcmp(argv[i], "--show-typing-keys") == 0) {
            opt.show_typing_keys = true;
        } else if (std::strcmp(argv[i], "--debug-effects") == 0) {
            opt.debug_effects = true;
        } else if (std::strcmp(argv[i], "--bench") == 0) {
            bench = true;
        } else if (std::strcmp(argv[i], "--bench-all") == 0) {
            bench = true;
            bench_options.styles.assign(std::begin(toonval::kAllNoteStyles),
                                        std::end(toonval::kAllNoteStyles));
        } else if (std::strcmp(argv[i], "--notes") == 0 && i + 1 < argc) {
            bench_options.notes = std::atoi(argv[++i]);
            if (bench_options.notes < 1 || bench_options.notes > 2000) {
                std::fprintf(stderr, "bad note count: %s\n", argv[i]);
                return 2;
            }
        } else if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            bench_options.seconds = std::atof(argv[++i]);
            if (!(bench_options.seconds > 0.0 && bench_options.seconds <= 600.0)) {
                std::fprintf(stderr, "bad seconds: %s\n", argv[i]);
                return 2;
            }
        } else if (std::strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            if (std::sscanf(argv[++i], "%dx%d", &bench_options.width, &bench_options.height) != 2 ||
                bench_options.width < 100 || bench_options.height < 100) {
                std::fprintf(stderr, "bad size: %s\n", argv[i]);
                return 2;
            }
        } else if (std::strcmp(argv[i], "--no-degrade") == 0) {
            bench_options.degrade = false;
        } else if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            bench_options.screenshot = argv[++i];
        } else if (std::strcmp(argv[i], "--at") == 0 && i + 1 < argc) {
            bench_options.at_seconds = std::atof(argv[++i]);
            if (!(bench_options.at_seconds > 0.5 && bench_options.at_seconds < 600.0)) {
                std::fprintf(stderr, "bad time: %s\n", argv[i]);
                return 2;
            }
        } else if (std::strcmp(argv[i], "--names") == 0 && i + 1 < argc) {
            opt.names = argv[++i];
            if (std::strcmp(opt.names, "off") != 0 && std::strcmp(opt.names, "letters") != 0 &&
                std::strcmp(opt.names, "solfege") != 0) {
                std::fprintf(stderr, "unknown names mode: %s\n", opt.names);
                print_usage();
                return 2;
            }
        } else if (std::strcmp(argv[i], "--octaves") == 0 && i + 1 < argc) {
            opt.octaves = argv[++i];
            if (std::strcmp(opt.octaves, "scientific") != 0 &&
                std::strcmp(opt.octaves, "yamaha") != 0) {
                std::fprintf(stderr, "unknown octave numbering: %s\n", opt.octaves);
                print_usage();
                return 2;
            }
        } else if (std::strcmp(argv[i], "--window") == 0 && i + 1 < argc) {
            if (std::sscanf(argv[++i], "%dx%d", &opt.size.width, &opt.size.height) != 2 ||
                opt.size.width < 100 || opt.size.height < 100) {
                std::fprintf(stderr, "bad window size: %s\n", argv[i]);
                print_usage();
                return 2;
            }
        } else if (std::strcmp(argv[i], "--settings-dir") == 0 && i + 1 < argc) {
            opt.settings_dir = argv[++i];
        } else if (std::strcmp(argv[i], "--song") == 0 && i + 1 < argc) {
            opt.song_path = argv[++i];
        } else if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            const char* m = argv[++i];
            if (std::strcmp(m, "flow") == 0) {
                opt.mode = toonval::PracticeMode::Flow;
            } else if (std::strcmp(m, "wait") == 0) {
                opt.mode = toonval::PracticeMode::Wait;
            } else {
                std::fprintf(stderr, "unknown mode: %s\n", m);
                print_usage();
                return 2;
            }
        } else if (std::strcmp(argv[i], "--hands") == 0 && i + 1 < argc) {
            const char* h = argv[++i];
            if (std::strcmp(h, "both") == 0) {
                opt.hands = toonval::HandFilter::Both;
            } else if (std::strcmp(h, "left") == 0) {
                opt.hands = toonval::HandFilter::Left;
            } else if (std::strcmp(h, "right") == 0) {
                opt.hands = toonval::HandFilter::Right;
            } else {
                std::fprintf(stderr, "unknown hands: %s\n", h);
                print_usage();
                return 2;
            }
        } else if (std::strcmp(argv[i], "--song-sound") == 0 && i + 1 < argc) {
            const char* m = argv[++i];
            if (std::strcmp(m, "mute") == 0) {
                opt.song_sound = toonval::app::SongSound::Mute;
            } else if (std::strcmp(m, "other-hand") == 0) {
                opt.song_sound = toonval::app::SongSound::OtherHand;
            } else if (std::strcmp(m, "both-hands") == 0) {
                opt.song_sound = toonval::app::SongSound::BothHands;
            } else {
                std::fprintf(stderr, "unknown song sound: %s\n", m);
                print_usage();
                return 2;
            }
        } else if (std::strcmp(argv[i], "--play") == 0) {
            opt.play = true;
        } else if (std::strcmp(argv[i], "--seek") == 0 && i + 1 < argc) {
            opt.seek_s = std::atof(argv[++i]);
            if (!(opt.seek_s >= 0.0 && opt.seek_s < 86400.0)) {
                std::fprintf(stderr, "bad seek time: %s\n", argv[i]);
                return 2;
            }
        } else if (std::strcmp(argv[i], "--robot") == 0) {
            opt.robot = true;
        } else if (std::strcmp(argv[i], "--panel") == 0 && i + 1 < argc) {
            toonval::app::PanelTab tab{};
            if (!toonval::app::parse_panel_tab(argv[++i], &tab)) {
                std::fprintf(stderr, "unknown panel page: %s\n", argv[i]);
                print_usage();
                return 2;
            }
            opt.panel = tab;
        } else if ((std::strcmp(argv[i], "--tap") == 0 || std::strcmp(argv[i], "--drag") == 0 ||
                    std::strcmp(argv[i], "--move") == 0 || std::strcmp(argv[i], "--type") == 0 ||
                    std::strcmp(argv[i], "--shot") == 0 || std::strcmp(argv[i], "--resize") == 0 ||
                    std::strcmp(argv[i], "--info") == 0 || std::strcmp(argv[i], "--key") == 0 ||
                    std::strcmp(argv[i], "--press") == 0 ||
                    std::strcmp(argv[i], "--quit-frame") == 0) &&
                   i + 1 < argc) {
            if (!opt.script.parse(argv[i], argv[i + 1])) {
                std::fprintf(stderr, "bad value for %s: %s\n", argv[i], argv[i + 1]);
                print_usage();
                return 2;
            }
            ++i;
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            print_usage();
            return 0;
        } else {
            std::fprintf(stderr, "unknown option: %s\n", argv[i]);
            print_usage();
            return 2;
        }
    }
    if (opt.octaves != nullptr) {
        log_look.octave_numbering = std::strcmp(opt.octaves, "yamaha") == 0
                                        ? toonval::OctaveNumbering::Yamaha
                                        : toonval::OctaveNumbering::Scientific;
    }
    g_octave_offset = log_look.octave_offset();
    if (bench || !bench_options.screenshot.empty()) {
        // The bench starts from the defaults, not the saved look, so runs compare.
        toonval::LookSettings look;
        if (!opt.preset) {
            look.background = toonval::BackgroundStyle::Motion; // every effect on: the worst case
        }
        apply_look_options(opt, look);
        if (opt.style != nullptr) {
            bench_options.styles.clear(); // --style narrows --bench-all to that one style
        }
        if (opt.size.width > 0) {
            bench_options.width = opt.size.width;
            bench_options.height = opt.size.height;
            bench_options.exact_window = true;
        }
        return toonval::app::run_bench(bench_options, look);
    }
    if (synth_test) {
        return toonval::app::run_synth_test(opt.audio);
    }
    return midi_log ? run_midi_log(opt.audio) : run_window(opt);
}
