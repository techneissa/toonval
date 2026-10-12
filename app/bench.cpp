// SPDX-License-Identifier: GPL-3.0-or-later
#include "bench.hpp"

#include "scene.hpp"
#include "song_transport.hpp"
#include "window_icon.hpp"
#include "toonval/chord_display.hpp"
#include "toonval/effects_governor.hpp"
#include "toonval/key_state.hpp"
#include "toonval/keyboard.hpp"
#include "toonval/keyboard_view.hpp"
#include "toonval/note_style.hpp"
#include "toonval/note_trail.hpp"
#include "toonval/screen_layout.hpp"
#include "ui.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace toonval::app {

namespace {

constexpr std::int64_t kSecond = 1'000'000'000;
constexpr double kWarmupSeconds = 0.5; // after the screen has filled (the visible seconds)
constexpr double kFirstBeat = 0.2;     // seconds
constexpr double kAvgNoteSeconds = 0.55;
// A chord of 3 or 4, a melody note, a bass note every 4th beat: about 5 notes a beat, measured
// down to 4.1 so the count on screen lands near the asked-for number.
constexpr double kNotesPerBeat = 4.1;

// A fixed, made-up piece: chords on every beat, a melody note between them, a held bass note every
// fourth beat and a long held chord every third. Sized so about `notes` blocks are on screen.
struct Schedule {
    std::vector<MidiEvent> events; // sorted by time, relative to the start
    double beat = 0.5;
};

class Rng {
  public:
    double next() {
        state_ = state_ * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<double>(state_ >> 11) / 9007199254740992.0;
    }
    int range(int lo, int hi) { return lo + static_cast<int>(next() * (hi - lo + 1)); }

  private:
    std::uint64_t state_ = 0x243f6a8885a308d3ull;
};

Schedule make_schedule(int notes, double visible_seconds, double total_seconds, KeyRange range) {
    Schedule s;
    const double rate = std::max(1.0, notes / (visible_seconds + kAvgNoteSeconds));
    s.beat = kNotesPerBeat / rate;
    Rng rng;
    const auto add = [&](int note, double start, double length, int velocity) {
        note = std::clamp(note, range.lowest, range.highest);
        const auto on = static_cast<std::int64_t>(start * 1e9);
        const auto off = static_cast<std::int64_t>((start + std::max(0.06, length)) * 1e9);
        s.events.push_back({on, MidiEventType::NoteOn, 0, static_cast<std::uint8_t>(note),
                            static_cast<std::uint8_t>(std::clamp(velocity, 1, 127)), false});
        s.events.push_back(
            {off, MidiEventType::NoteOff, 0, static_cast<std::uint8_t>(note), 0, false});
    };
    const int mid = (range.lowest + range.highest) / 2;
    int beat_index = 0;
    for (double t = kFirstBeat; t < total_seconds; t += s.beat, ++beat_index) {
        // Chord: a triad with the octave on top, major or minor, sometimes held long.
        const int root = std::clamp(mid - 12 + rng.range(-5, 7), range.lowest, range.highest - 12);
        const int third = rng.next() < 0.5 ? 4 : 3;
        const bool long_hold = beat_index % 3 == 0;
        const double length = long_hold ? s.beat * (2.5 + rng.next()) : s.beat * (0.5 + rng.next());
        const int velocity = rng.range(60, 120);
        add(root, t, length, velocity);
        add(root + third, t, length, velocity);
        add(root + 7, t, length, velocity);
        if (rng.next() < 0.75) {
            add(root + 12, t, length, velocity);
        }
        // Melody between the beats, above the chord.
        add(mid + rng.range(4, 26), t + 0.5 * s.beat, s.beat * (0.3 + 0.5 * rng.next()),
            rng.range(50, 127));
        if (beat_index % 4 == 0) {
            add(range.lowest + rng.range(4, 20), t, s.beat * 3.5, rng.range(70, 110));
        }
    }
    std::stable_sort(s.events.begin(), s.events.end(), [](const MidiEvent& a, const MidiEvent& b) {
        if (a.time_ns != b.time_ns) {
            return a.time_ns < b.time_ns;
        }
        return a.type == MidiEventType::NoteOff && b.type != MidiEventType::NoteOff;
    });
    return s;
}

double ms(std::int64_t ns) { return static_cast<double>(ns) / 1e6; }

struct Stats {
    std::size_t frames = 0;
    double min = 0.0;
    double median = 0.0;
    double p95 = 0.0;
    double max = 0.0;
};

Stats summarise(std::vector<std::int64_t>& samples) {
    Stats st;
    if (samples.empty()) {
        return st;
    }
    std::sort(samples.begin(), samples.end());
    st.frames = samples.size();
    st.min = ms(samples.front());
    st.max = ms(samples.back());
    st.median = ms(samples[samples.size() / 2]);
    st.p95 = ms(samples[std::min(samples.size() - 1, samples.size() * 95 / 100)]);
    return st;
}

// One style's run: draws frames until `seconds` of measurement (after warm-up) have passed, or,
// with a screenshot path, steps a 60 Hz clock to `at` and saves that frame.
struct RunResult {
    Stats stats;
    EffectsLevel lowest = EffectsLevel::Full;
    EffectsLevel last = EffectsLevel::Full;
    int notes_on_screen = 0;
    bool ok = true;
};

RunResult run_one(SDL_Renderer* renderer, SDL_Texture* target, const BenchOptions& o,
                  const LookSettings& look) {
    RunResult result;
    const KeyboardLayout keyboard(kRange88);
    Scene scene(renderer, look);
    const std::int64_t span_ns = scene.trail_span_ns();
    const bool shot = !o.screenshot.empty();
    const double warmup = look.visible_seconds + kWarmupSeconds;
    const double total = (shot ? o.at_seconds : warmup + o.seconds) + 1.0;
    const Schedule schedule = make_schedule(o.notes, look.visible_seconds, total, keyboard.range());
    // A screenshot lands just after a chord's note-on, while its sparks are in the air.
    const double at =
        std::floor((o.at_seconds - kFirstBeat) / schedule.beat) * schedule.beat + kFirstBeat + 0.12;

    KeyState keys;
    NoteTrail trail;
    ChordDisplay chord;
    ChordLabel label;
    std::vector<std::uint8_t> held;
    held.reserve(128);
    // The key size of the look moves the view to follow the played notes, as in the app.
    std::vector<int> held_notes;
    held_notes.reserve(128);
    KeyViewFollower follower;
    std::optional<KeyRange> view_range;
    std::int64_t view_ns = 0;
    int view_w = 0;
    std::optional<KeyboardLayout> layout(keyboard.range());
    follower.snap(keyboard.range(), keyboard.range());
    KeyViewFrame view = follower.frame();
    std::vector<TrailBar> bars;
    bars.reserve(4096);
    std::array<std::int64_t, 128> last_press{};
    last_press.fill(kNeverPressed);
    std::array<std::uint8_t, 128> last_velocity{};
    std::vector<std::int64_t> samples;
    samples.reserve(65536);
    EffectsGovernor governor;
    const std::string status = std::string("Bench: ") +
                               std::string(note_style_name(look.note_style)) + ", " +
                               std::to_string(o.notes) + " notes";
    char level_line[64] = "";
    // The UI as the app draws it while playing: the top row (status text and gear), the bar
    // hidden. No song, so no transport.
    UiModel model;
    model.look = look;
    SongController songs;
    SongTransport song(songs);
    Ui ui(SDL_GetRenderWindow(renderer), renderer);
    const auto draw_ui = [&](std::int64_t now_ns, std::string_view note) {
        int out_w = 0;
        int out_h = 0;
        SDL_GetCurrentRenderOutputSize(renderer, &out_w, &out_h);
        UiFrameInfo info;
        info.width_px = static_cast<float>(out_w);
        info.height_px = static_cast<float>(out_h);
        info.px_per_mm = kDefaultPxPerMm;
        info.layout = compute_layout(info.width_px, info.height_px, info.px_per_mm, model.look);
        info.now_ns = now_ns;
        info.playing = true;
        info.status_label = status;
        info.status_notes = {note, "", "", ""};
        song.update(now_ns);
        ui.draw(model, song, info);
    };

    const std::int64_t start = static_cast<std::int64_t>(SDL_GetTicksNS());
    std::size_t next_event = 0;
    std::int64_t frame = 0;
    const SDL_Rect one{0, 0, 1, 1};
    for (;;) {
        // Let the GPU finish everything queued so far (the last frame and its present, which waits
        // for the display) before the clock starts.
        if (target != nullptr) {
            SDL_SetRenderTarget(renderer, target);
        }
        SDL_DestroySurface(SDL_RenderReadPixels(renderer, &one));
        const std::int64_t frame_start = static_cast<std::int64_t>(SDL_GetTicksNS());
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                result.ok = false;
                return result;
            }
        }
        const std::int64_t real_now = static_cast<std::int64_t>(SDL_GetTicksNS());
        const std::int64_t now = shot ? start + frame * kSecond / 60 : real_now;
        const double elapsed = static_cast<double>(now - start) / 1e9;
        if (shot ? elapsed >= at : elapsed >= warmup + o.seconds) {
            break;
        }

        while (next_event < schedule.events.size() &&
               start + schedule.events[next_event].time_ns <= now) {
            MidiEvent ev = schedule.events[next_event++];
            ev.time_ns += start;
            const KeyTransition t = keys.apply(ev);
            trail.apply(t, ev.time_ns);
            if (t.type == KeyTransitionType::Pressed) {
                last_press[t.note] = ev.time_ns;
                last_velocity[t.note] = t.velocity;
            }
        }
        trail.prune(now, span_ns);
        trail.visible(now, span_ns, bars);
        keys.held_notes(held);
        label = chord.update(held, now);
        {
            int out_w = 0;
            int out_h = 0;
            SDL_GetCurrentRenderOutputSize(renderer, &out_w, &out_h);
            held_notes.assign(held.begin(), held.end());
            KeyViewParams vp;
            vp.keyboard = keyboard.range();
            vp.width_px = static_cast<float>(out_w);
            vp.px_per_mm = kDefaultPxPerMm;
            vp.size = look.key_size;
            vp.held = held_notes;
            vp.current = view_range;
            const KeyViewTarget wanted = compute_key_view(vp);
            view_range = wanted.range;
            if (!follower.has_position() || out_w != view_w) {
                follower.snap(keyboard.range(), wanted.range);
            } else {
                follower.advance(keyboard.range(), wanted.range,
                                 static_cast<double>(now - view_ns) / 1e9);
            }
            view_w = out_w;
            view_ns = now;
            view = follower.frame();
            if (!layout || layout->range().lowest != view.layout_range.lowest ||
                layout->range().highest != view.layout_range.highest) {
                layout.emplace(view.layout_range);
            }
        }
        result.notes_on_screen = std::max(result.notes_on_screen, static_cast<int>(bars.size()));

        if (target != nullptr) {
            SDL_SetRenderTarget(renderer, target);
        }
        scene.draw(
            {*layout, view, keys, bars, last_press, last_velocity, now, label, false, nullptr});
        draw_ui(now, level_line);
        // Reading one pixel back waits for the GPU to finish the frame, so the time below is the
        // frame's whole cost on CPU and GPU, without the wait for the display's refresh (macOS
        // keeps vsync on whatever the renderer asks).
        SDL_DestroySurface(SDL_RenderReadPixels(renderer, &one));
        const std::int64_t frame_ns = static_cast<std::int64_t>(SDL_GetTicksNS()) - frame_start;
        if (target != nullptr) {
            SDL_SetRenderTarget(renderer, nullptr);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderClear(renderer);
            SDL_RenderTexture(renderer, target, nullptr, nullptr);
        }
        SDL_RenderPresent(renderer);
        ++frame;

        if (!shot && elapsed >= warmup) {
            samples.push_back(frame_ns);
        }
        if (o.degrade && !shot) {
            const EffectsLevel before = scene.effects_level();
            scene.set_effects_level(governor.add_frame(frame_ns));
            if (scene.effects_level() != before) {
                std::snprintf(level_line, sizeof level_line, "effects: %s",
                              std::string(effects_level_name(scene.effects_level())).c_str());
            }
        }
    }

    if (shot) {
        // Draw the frame once more into the target and read it back.
        if (target != nullptr) {
            SDL_SetRenderTarget(renderer, target);
        }
        scene.draw({*layout, view, keys, bars, last_press, last_velocity,
                    start + frame * kSecond / 60, label, false, nullptr});
        draw_ui(start + frame * kSecond / 60, "");
        SDL_Surface* pixels = SDL_RenderReadPixels(renderer, nullptr);
        if (target != nullptr) {
            SDL_SetRenderTarget(renderer, nullptr);
        }
        if (pixels == nullptr || !SDL_SavePNG(pixels, o.screenshot.c_str())) {
            std::fprintf(stderr, "screenshot failed: %s\n", SDL_GetError());
            result.ok = false;
        } else {
            std::printf("saved %s (%dx%d, %s, t=%.2f s, %d notes on screen)\n",
                        o.screenshot.c_str(), pixels->w, pixels->h,
                        std::string(note_style_name(look.note_style)).c_str(), at,
                        static_cast<int>(bars.size()));
        }
        SDL_DestroySurface(pixels);
        return result;
    }

    result.stats = summarise(samples);
    result.lowest = governor.lowest();
    result.last = governor.level();
    return result;
}

} // namespace

int run_bench(const BenchOptions& o, const LookSettings& base) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    // Draw straight into an exact window when asked (the Pi at its panel size); otherwise into a
    // target texture of the requested size, shown scaled in a small window.
    const int ww = o.exact_window ? o.width : 960;
    const int wh = o.exact_window ? o.height : std::max(1, 960 * o.height / std::max(1, o.width));
    if (!SDL_CreateWindowAndRenderer("toonval bench", ww, wh, 0, &window, &renderer)) {
        std::fprintf(stderr, "window creation failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    set_window_icon(window);
    SDL_SetRenderVSync(renderer, 0); // measure what a frame costs, not the refresh rate
    int out_w = 0;
    int out_h = 0;
    SDL_GetRenderOutputSize(renderer, &out_w, &out_h);
    SDL_Texture* target = nullptr;
    if (out_w != o.width || out_h != o.height) {
        target = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET,
                                   o.width, o.height);
        if (target == nullptr) {
            std::fprintf(stderr, "render target failed: %s\n", SDL_GetError());
            SDL_DestroyRenderer(renderer);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }
        SDL_SetTextureScaleMode(target, SDL_SCALEMODE_LINEAR);
    }

    std::vector<NoteStyle> styles = o.styles;
    if (styles.empty()) {
        styles.push_back(base.note_style);
    }
    if (o.screenshot.empty()) {
        std::printf("bench: %dx%d%s, ~%d notes on screen, %.1f s per style, renderer %s, "
                    "auto-degrade %s\n",
                    o.width, o.height, target != nullptr ? " (offscreen target)" : "", o.notes,
                    o.seconds, SDL_GetRendererName(renderer), o.degrade ? "on" : "off");
        std::printf("%-8s %7s %8s %8s %8s %8s %7s %6s  %s\n", "style", "frames", "min ms", "median",
                    "p95", "max", "fps cap", "notes", "effects level reached (final)");
    }
    int code = 0;
    for (NoteStyle style : styles) {
        LookSettings look = base;
        look.note_style = style;
        const RunResult r = run_one(renderer, target, o, look);
        if (!r.ok) {
            code = 1;
            break;
        }
        if (o.screenshot.empty()) {
            std::printf("%-8s %7zu %8.2f %8.2f %8.2f %8.2f %7.0f %6d  %s (%s)\n",
                        std::string(note_style_name(style)).c_str(), r.stats.frames, r.stats.min,
                        r.stats.median, r.stats.p95, r.stats.max,
                        r.stats.median > 0.0 ? 1000.0 / r.stats.median : 0.0, r.notes_on_screen,
                        std::string(effects_level_name(r.lowest)).c_str(),
                        std::string(effects_level_name(r.last)).c_str());
        }
    }

    if (target != nullptr) {
        SDL_DestroyTexture(target);
    }
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return code;
}

} // namespace toonval::app
