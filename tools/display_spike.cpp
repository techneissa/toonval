// SPDX-License-Identifier: GPL-3.0-or-later
// Hardware spike: which display, render and touch setup works on the Pi.
// Prints a PASS/FAIL/SKIP line per step. See --help and deploy/pi/CHECKLIST.md.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr double kReportSeconds = 2.0;
constexpr double kWarmupSeconds = 1.0; // frames before this don't count toward the verdict
constexpr Uint64 kMotionLogGapNs = 100'000'000;
constexpr int kFlashFrames = 12; // latency mode: white for this many frames
constexpr float kDefaultRefreshHz = 60.0f;

struct Options {
    bool help = false;
    bool list = false;
    bool latency = false;
    bool vsync = true;
    bool all_motion = false;
    double seconds = 0.0; // 0 = until Escape
    int displays = 0;     // 0 = all
    SDL_DisplayID display = 0;
    int pacing = 0; // 0 = off
};

volatile std::sig_atomic_t g_stop = 0;

void on_signal(int /*sig*/) { g_stop = 1; }

// --- results -------------------------------------------------------------------------------

enum class Status { Pass, Fail, Skip };

struct Result {
    Status status;
    std::string text;
};

std::vector<Result> g_results;

void report(Status status, const std::string& step, const std::string& detail) {
    const char* tag = status == Status::Pass ? "PASS" : status == Status::Fail ? "FAIL" : "SKIP";
    std::string text = step + ": " + detail;
    std::printf("[%s] %s\n", tag, text.c_str());
    g_results.push_back({status, std::move(text)});
}

std::string fmt(const char* format, ...) SDL_PRINTF_VARARG_FUNC(1);

std::string fmt(const char* format, ...) {
    char buffer[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof buffer, format, args);
    va_end(args);
    return buffer;
}

void info(const std::string& text) { std::printf("[INFO] %s\n", text.c_str()); }

// --- arguments -----------------------------------------------------------------------------

void print_help() {
    std::puts(
        "display-spike: display, renderer and touch checks.\n"
        "\n"
        "Usage: display-spike [options]\n"
        "  --list            print SDL drivers, displays and input devices, open no windows\n"
        "  --displays N      open a fullscreen window on the first N displays (default: all)\n"
        "  --display ID      open one window on the display with this id (see --list)\n"
        "  --seconds N       exit after N seconds (default: run until Escape)\n"
        "  --latency         black screen that flashes white on touch, mouse click or Space\n"
        "  --pacing N        flash a corner square for one frame every N frames\n"
        "  --no-vsync        do not ask for vsync (compare the frame rate)\n"
        "  --all-motion      log every touch and mouse motion event (default: one per 100 ms)\n"
        "  --help            show this text\n"
        "\n"
        "Choose the SDL backend with SDL_VIDEO_DRIVER (kmsdrm, wayland, x11) and the renderer\n"
        "with SDL_RENDER_DRIVER (opengles2, software). Exit: Escape, SIGTERM/SIGINT or --seconds.\n"
        "Exit code: 0 when no step failed, 1 when any step failed (for example a window that\n"
        "would not open), 2 for a usage error.");
}

bool parse_number(const char* text, double& out) {
    char* end = nullptr;
    out = std::strtod(text, &end);
    return end != text && *end == '\0' && std::isfinite(out);
}

// Returns false and prints a message on a usage error.
bool parse_args(int argc, char** argv, Options& opts) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto number = [&](double& out) {
            if (i + 1 >= argc || !parse_number(argv[i + 1], out) || out < 0) {
                std::fprintf(stderr, "display-spike: %s needs a non-negative number\n",
                             arg.c_str());
                return false;
            }
            ++i;
            return true;
        };
        double n = 0;
        if (arg == "--help" || arg == "-h") {
            opts.help = true;
        } else if (arg == "--list") {
            opts.list = true;
        } else if (arg == "--latency") {
            opts.latency = true;
        } else if (arg == "--no-vsync") {
            opts.vsync = false;
        } else if (arg == "--all-motion") {
            opts.all_motion = true;
        } else if (arg == "--seconds") {
            if (!number(opts.seconds)) {
                return false;
            }
        } else if (arg == "--displays") {
            if (!number(n) || n < 1) {
                std::fprintf(stderr, "display-spike: --displays needs a number of 1 or more\n");
                return false;
            }
            opts.displays = static_cast<int>(n);
        } else if (arg == "--display") {
            if (!number(n) || n < 1) {
                std::fprintf(stderr, "display-spike: --display needs a display id (see --list)\n");
                return false;
            }
            opts.display = static_cast<SDL_DisplayID>(n);
        } else if (arg == "--pacing") {
            if (!number(n) || n < 1) {
                std::fprintf(stderr, "display-spike: --pacing needs a frame count of 1 or more\n");
                return false;
            }
            opts.pacing = static_cast<int>(n);
        } else {
            std::fprintf(stderr, "display-spike: unknown option %s (try --help)\n", arg.c_str());
            return false;
        }
    }
    if (opts.display != 0 && opts.displays != 0) {
        std::fprintf(stderr, "display-spike: use either --display or --displays, not both\n");
        return false;
    }
    return true;
}

// --- system info ---------------------------------------------------------------------------

std::string text_or(const char* text, const char* fallback) {
    return (text != nullptr && text[0] != '\0') ? text : fallback;
}

const char* touch_type_name(SDL_TouchDeviceType type) {
    switch (type) {
    case SDL_TOUCH_DEVICE_DIRECT:
        return "direct (touch screen)";
    case SDL_TOUCH_DEVICE_INDIRECT_ABSOLUTE:
        return "indirect absolute (trackpad)";
    case SDL_TOUCH_DEVICE_INDIRECT_RELATIVE:
        return "indirect relative (trackpad)";
    default:
        return "unknown";
    }
}

std::vector<SDL_DisplayID> get_displays() {
    int count = 0;
    SDL_DisplayID* ids = SDL_GetDisplays(&count);
    std::vector<SDL_DisplayID> out;
    if (ids != nullptr) {
        out.assign(ids, ids + count);
        SDL_free(ids);
    }
    return out;
}

float display_refresh(SDL_DisplayID id) {
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(id);
    return (mode != nullptr && mode->refresh_rate > 1.0f) ? mode->refresh_rate : kDefaultRefreshHz;
}

void print_display(SDL_DisplayID id) {
    const std::string name = text_or(SDL_GetDisplayName(id), "(unnamed)");
    SDL_Rect bounds{};
    SDL_GetDisplayBounds(id, &bounds);
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(id);
    int mode_count = 0;
    SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(id, &mode_count);
    SDL_free(modes);
    std::printf("  display id=%u name=\"%s\"\n", id, name.c_str());
    std::printf("    bounds %d,%d %dx%d", bounds.x, bounds.y, bounds.w, bounds.h);
    if (mode != nullptr) {
        std::printf("  mode %dx%d @ %.2f Hz (%s, pixel density %.2f)", mode->w, mode->h,
                    static_cast<double>(mode->refresh_rate), SDL_GetPixelFormatName(mode->format),
                    static_cast<double>(mode->pixel_density));
    } else {
        std::printf("  mode unknown (%s)", SDL_GetError());
    }
    std::printf("  content scale %.2f  (%d fullscreen modes)\n",
                static_cast<double>(SDL_GetDisplayContentScale(id)), mode_count);
}

void print_environment() {
    const int version = SDL_GetVersion();
    std::printf("SDL %d.%d.%d (%s)\n", SDL_VERSIONNUM_MAJOR(version), SDL_VERSIONNUM_MINOR(version),
                SDL_VERSIONNUM_MICRO(version), SDL_GetRevision());
    std::printf("platform %s\n", SDL_GetPlatform());
    std::printf("SDL_VIDEO_DRIVER=%s  SDL_RENDER_DRIVER=%s\n",
                text_or(SDL_getenv("SDL_VIDEO_DRIVER"), "(unset)").c_str(),
                text_or(SDL_getenv("SDL_RENDER_DRIVER"), "(unset)").c_str());
    std::string drivers;
    for (int i = 0; i < SDL_GetNumVideoDrivers(); ++i) {
        drivers += std::string(i == 0 ? "" : " ") + SDL_GetVideoDriver(i);
    }
    std::printf("video drivers built in: %s\n", drivers.c_str());
    std::string renderers;
    for (int i = 0; i < SDL_GetNumRenderDrivers(); ++i) {
        renderers += std::string(i == 0 ? "" : " ") + SDL_GetRenderDriver(i);
    }
    std::printf("render drivers built in: %s\n", renderers.c_str());
    std::printf("video driver in use: %s\n",
                text_or(SDL_GetCurrentVideoDriver(), "(none)").c_str());
}

void print_input_devices() {
    int touch_count = 0;
    SDL_TouchID* touches = SDL_GetTouchDevices(&touch_count);
    std::printf("touch devices (%d)\n", touch_count);
    for (int i = 0; i < touch_count; ++i) {
        std::printf("  touch id=%llu name=\"%s\" type=%s\n",
                    static_cast<unsigned long long>(touches[i]),
                    text_or(SDL_GetTouchDeviceName(touches[i]), "?").c_str(),
                    touch_type_name(SDL_GetTouchDeviceType(touches[i])));
    }
    SDL_free(touches);

    int mouse_count = 0;
    SDL_MouseID* mice = SDL_GetMice(&mouse_count);
    std::printf("mice (%d)  (a touch panel can enumerate here instead)\n", mouse_count);
    for (int i = 0; i < mouse_count; ++i) {
        std::printf("  mouse id=%u name=\"%s\"\n", mice[i],
                    text_or(SDL_GetMouseNameForID(mice[i]), "?").c_str());
    }
    SDL_free(mice);
}

// --- windows -------------------------------------------------------------------------------

struct Finger {
    SDL_WindowID window = 0;
    float x = 0;
    float y = 0;
    Uint64 last_log_ns = 0;
};

struct Win {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_WindowID id = 0;
    SDL_DisplayID display = 0;
    std::string name;
    SDL_Color color{};
    float refresh_hz = kDefaultRefreshHz;
    int out_w = 0;
    int out_h = 0;
    std::string renderer_name;

    Uint64 last_present = 0;
    std::uint64_t frame = 0;
    // Steady-state statistics, counted after the warm-up.
    std::uint64_t frames = 0;
    std::uint64_t slow_frames = 0; // frame time above 1.5 refresh periods
    double worst_ms = 0;
    double steady_start_s = -1;
    double steady_end_s = 0;
    // Statistics for the current report interval.
    std::uint64_t interval_frames = 0;
    double interval_worst_ms = 0;
    double shown_fps = 0;

    int flash_left = 0; // latency mode
    bool latency_pending = false;
    Uint64 trigger_ns = 0;

    bool has_last_touch = false;
    float last_touch_x = 0;
    float last_touch_y = 0;
    bool has_mouse = false;
    float mouse_x = 0;
    float mouse_y = 0;
};

struct TouchStats {
    std::uint64_t events = 0;
    std::uint64_t down = 0;
    float min_x = 1e9f;
    float max_x = -1e9f;
    float min_y = 1e9f;
    float max_y = -1e9f;
    std::map<SDL_TouchID, std::uint64_t> per_device;
    std::map<SDL_WindowID, std::uint64_t> per_window;
};

struct App {
    Options opts;
    std::vector<Win> wins;
    std::map<SDL_FingerID, Finger> fingers;
    TouchStats touch;
    std::uint64_t mouse_real = 0;
    std::uint64_t mouse_from_touch = 0;
    Uint64 last_mouse_log_ns = 0;
    std::uint64_t triggers = 0;
    bool running = true;
};

Win* find_window(App& app, SDL_WindowID id) {
    for (auto& w : app.wins) {
        if (w.id == id) {
            return &w;
        }
    }
    return nullptr;
}

constexpr SDL_Color kPalette[] = {{170, 40, 40, 255},  {30, 120, 60, 255},  {40, 70, 170, 255},
                                  {160, 100, 20, 255}, {110, 50, 150, 255}, {20, 120, 130, 255}};

// Opens a fullscreen window and renderer on one display. Returns false after reporting a FAIL.
bool open_window(App& app, SDL_DisplayID display, std::size_t index) {
    const std::string name = text_or(SDL_GetDisplayName(display), "(unnamed)");
    const std::string step = fmt("window display %u", display);
    SDL_Rect bounds{};
    SDL_GetDisplayBounds(display, &bounds);

    const SDL_PropertiesID props = SDL_CreateProperties();
    const std::string title = "display-spike " + name;
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title.c_str());
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER,
                          SDL_WINDOWPOS_UNDEFINED_DISPLAY(display));
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER,
                          SDL_WINDOWPOS_UNDEFINED_DISPLAY(display));
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, bounds.w);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, bounds.h);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN, true);
    SDL_Window* window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (window == nullptr) {
        report(Status::Fail, step, fmt("SDL_CreateWindow failed: %s", SDL_GetError()));
        return false;
    }
    SDL_SyncWindow(window);

    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (renderer == nullptr) {
        report(Status::Fail, step,
               fmt("window opened but SDL_CreateRenderer failed: %s", SDL_GetError()));
        SDL_DestroyWindow(window);
        return false;
    }

    Win w;
    w.window = window;
    w.renderer = renderer;
    w.id = SDL_GetWindowID(window);
    w.display = display;
    w.name = name;
    w.color = kPalette[index % std::size(kPalette)];
    w.refresh_hz = display_refresh(display);
    w.renderer_name = text_or(SDL_GetRendererName(renderer), "?");
    SDL_GetRenderOutputSize(renderer, &w.out_w, &w.out_h);

    int win_w = 0;
    int win_h = 0;
    SDL_GetWindowSize(window, &win_w, &win_h);
    const bool fullscreen = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
    const SDL_DisplayID landed = SDL_GetDisplayForWindow(window);
    if (!fullscreen) {
        report(Status::Fail, step,
               fmt("window %u opened but is not fullscreen (%dx%d)", w.id, win_w, win_h));
    } else if (landed != display) {
        report(
            Status::Fail, step,
            fmt("window %u is fullscreen but landed on display %u, not %u", w.id, landed, display));
    } else {
        report(Status::Pass, step,
               fmt("window %u fullscreen on \"%s\", %dx%d points, %dx%d pixels", w.id, name.c_str(),
                   win_w, win_h, w.out_w, w.out_h));
    }

    const std::string rstep = fmt("renderer window %u", w.id);
    if (w.renderer_name == "software") {
        report(Status::Fail, rstep, "software renderer; the GPU renderer did not start");
    } else {
        report(Status::Pass, rstep,
               fmt("%s%s", w.renderer_name.c_str(),
                   w.renderer_name == "opengles2" ? " (the expected renderer on the Pi)" : ""));
    }
    if (app.opts.vsync) {
        int vsync = 0;
        const bool set = SDL_SetRenderVSync(renderer, 1);
        const bool got = SDL_GetRenderVSync(renderer, &vsync);
        if (set && got && vsync == 1) {
            report(Status::Pass, fmt("vsync window %u", w.id), "enabled");
        } else {
            report(Status::Fail, fmt("vsync window %u", w.id),
                   fmt("could not enable vsync: %s", SDL_GetError()));
        }
    } else {
        SDL_SetRenderVSync(renderer, 0);
        info(fmt("vsync window %u: off (--no-vsync)", w.id));
    }

    app.wins.push_back(std::move(w));
    return true;
}

// --- drawing -------------------------------------------------------------------------------

void draw_text(SDL_Renderer* r, float x, float y, float scale, const std::string& text) {
    SDL_SetRenderScale(r, scale, scale);
    SDL_RenderDebugText(r, x / scale, y / scale, text.c_str());
    SDL_SetRenderScale(r, 1.0f, 1.0f);
}

float text_width(const std::string& text, float scale) {
    return static_cast<float>(text.size()) * SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * scale;
}

void draw_finger(SDL_Renderer* r, float x, float y, float size, bool filled) {
    const SDL_FRect box{x - size / 2, y - size / 2, size, size};
    if (filled) {
        SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
        SDL_RenderFillRect(r, &box);
        SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    } else {
        SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
    }
    SDL_RenderRect(r, &box);
    SDL_RenderLine(r, x - size, y, x + size, y);
    SDL_RenderLine(r, x, y - size, x, y + size);
}

void draw_window(App& app, Win& w, bool flash_white) {
    SDL_Renderer* r = w.renderer;
    SDL_GetRenderOutputSize(r, &w.out_w, &w.out_h);
    const float width = static_cast<float>(w.out_w);
    const float height = static_cast<float>(w.out_h);

    if (flash_white) {
        SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
    } else if (app.opts.latency) {
        SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    } else {
        SDL_SetRenderDrawColor(r, w.color.r, w.color.g, w.color.b, 255);
    }
    SDL_RenderClear(r);

    if (!flash_white) {
        SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
        if (app.opts.latency) {
            draw_text(r, 60, height / 2, 3.0f, "LATENCY: touch the screen or press Space");
        } else {
            const float scale = std::clamp(0.9f * width / text_width(w.name, 1.0f), 1.0f, 10.0f);
            draw_text(r, (width - text_width(w.name, scale)) / 2, height * 0.35f, scale, w.name);
            const std::string line1 =
                fmt("display %u  window %u  %d x %d px @ %.1f Hz", w.display, w.id, w.out_w,
                    w.out_h, static_cast<double>(w.refresh_hz));
            const std::string line2 =
                fmt("%s / %s / vsync %s", text_or(SDL_GetCurrentVideoDriver(), "?").c_str(),
                    w.renderer_name.c_str(), app.opts.vsync ? "on" : "off");
            const std::string line3 = fmt("%.1f fps   Escape quits", w.shown_fps);
            draw_text(r, 40, height * 0.55f, 3.0f, line1);
            draw_text(r, 40, height * 0.55f + 40, 3.0f, line2);
            draw_text(r, 40, height * 0.55f + 80, 3.0f, line3);
        }
    }

    const float dot = std::max(24.0f, std::min(width, height) * 0.05f);
    if (w.has_mouse) {
        const SDL_FRect box{w.mouse_x - 6, w.mouse_y - 6, 12, 12};
        SDL_SetRenderDrawColor(r, 255, 220, 0, 255);
        SDL_RenderFillRect(r, &box);
    }
    if (w.has_last_touch && !flash_white) {
        draw_finger(r, w.last_touch_x * width, w.last_touch_y * height, dot * 0.6f, false);
    }
    for (const auto& [fid, f] : app.fingers) {
        const bool mine = f.window == w.id || (f.window == 0 && &w == &app.wins.front());
        if (mine) {
            draw_finger(r, f.x * width, f.y * height, dot, true);
        }
    }

    if (app.opts.pacing > 0) {
        const float side = std::min(width, height) * 0.15f;
        const SDL_FRect box{0, 0, side, side};
        const bool on = w.frame % static_cast<std::uint64_t>(app.opts.pacing) == 0;
        SDL_SetRenderDrawColor(r, on ? 255 : 0, on ? 255 : 0, on ? 255 : 0, 255);
        SDL_RenderFillRect(r, &box);
    }
}

// Draws and presents one frame, then updates the frame statistics.
void render_window(App& app, Win& w, double run_s, double freq) {
    const bool flash = app.opts.latency && w.flash_left > 0;
    draw_window(app, w, flash);
    SDL_RenderPresent(w.renderer);
    const Uint64 now = SDL_GetPerformanceCounter();
    const Uint64 now_ns = SDL_GetTicksNS();

    if (flash && w.latency_pending) {
        w.latency_pending = false;
        std::printf("[LAT] window %u: trigger to end of first white present: %.2f ms\n", w.id,
                    static_cast<double>(now_ns - w.trigger_ns) / 1e6);
    }
    if (flash) {
        --w.flash_left;
    }

    if (w.last_present != 0) {
        const double dt_ms = static_cast<double>(now - w.last_present) * 1000.0 / freq;
        w.interval_frames++;
        w.interval_worst_ms = std::max(w.interval_worst_ms, dt_ms);
        if (run_s >= kWarmupSeconds) {
            if (w.steady_start_s < 0) {
                w.steady_start_s = run_s;
            }
            w.steady_end_s = run_s;
            ++w.frames;
            w.worst_ms = std::max(w.worst_ms, dt_ms);
            if (dt_ms > 1.5 * 1000.0 / static_cast<double>(w.refresh_hz)) {
                ++w.slow_frames;
            }
        }
    }
    w.last_present = now;
    ++w.frame;
}

// --- events --------------------------------------------------------------------------------

void trigger_flash(App& app, Uint64 event_ns) {
    ++app.triggers;
    for (auto& w : app.wins) {
        w.flash_left = kFlashFrames;
        w.latency_pending = true;
        w.trigger_ns = event_ns;
    }
}

void handle_finger(App& app, const SDL_TouchFingerEvent& e) {
    const char* kind = e.type == SDL_EVENT_FINGER_DOWN       ? "down"
                       : e.type == SDL_EVENT_FINGER_UP       ? "up"
                       : e.type == SDL_EVENT_FINGER_CANCELED ? "canceled"
                                                             : "motion";
    TouchStats& stats = app.touch;
    ++stats.events;
    ++stats.per_device[e.touchID];
    ++stats.per_window[e.windowID];
    stats.min_x = std::min(stats.min_x, e.x);
    stats.max_x = std::max(stats.max_x, e.x);
    stats.min_y = std::min(stats.min_y, e.y);
    stats.max_y = std::max(stats.max_y, e.y);

    Finger& f = app.fingers[e.fingerID];
    f.window = e.windowID;
    f.x = e.x;
    f.y = e.y;

    Win* w = find_window(app, e.windowID);
    Win* mapped = w != nullptr ? w : (app.wins.empty() ? nullptr : &app.wins.front());
    if (e.type == SDL_EVENT_FINGER_DOWN) {
        ++stats.down;
        if (app.opts.latency) {
            trigger_flash(app, e.timestamp);
        }
    }
    if (mapped != nullptr && e.type != SDL_EVENT_FINGER_DOWN && e.type != SDL_EVENT_FINGER_MOTION) {
        mapped->has_last_touch = true;
        mapped->last_touch_x = e.x;
        mapped->last_touch_y = e.y;
    }

    const bool quiet = e.type == SDL_EVENT_FINGER_MOTION && !app.opts.all_motion &&
                       e.timestamp - f.last_log_ns < kMotionLogGapNs;
    if (!quiet) {
        f.last_log_ns = e.timestamp;
        const float px = mapped != nullptr ? e.x * static_cast<float>(mapped->out_w) : 0;
        const float py = mapped != nullptr ? e.y * static_cast<float>(mapped->out_h) : 0;
        std::printf("[TOUCH] %-8s dev=%llu \"%s\" finger=%llu norm=(%.4f, %.4f) window=%u%s "
                    "px=(%.0f, %.0f) pressure=%.2f\n",
                    kind, static_cast<unsigned long long>(e.touchID),
                    text_or(SDL_GetTouchDeviceName(e.touchID), "?").c_str(),
                    static_cast<unsigned long long>(e.fingerID), static_cast<double>(e.x),
                    static_cast<double>(e.y), e.windowID,
                    w == nullptr ? " (none; mapped to the first window)" : "",
                    static_cast<double>(px), static_cast<double>(py),
                    static_cast<double>(e.pressure));
    }
    if (e.type == SDL_EVENT_FINGER_UP || e.type == SDL_EVENT_FINGER_CANCELED) {
        app.fingers.erase(e.fingerID);
    }
}

void count_mouse(App& app, SDL_MouseID which) {
    if (which == SDL_TOUCH_MOUSEID) {
        ++app.mouse_from_touch;
    } else {
        ++app.mouse_real;
    }
}

std::string mouse_source(SDL_MouseID which) {
    return which == SDL_TOUCH_MOUSEID ? "synthesized from touch" : fmt("mouse id %u", which);
}

void handle_event(App& app, const SDL_Event& e) {
    switch (e.type) {
    case SDL_EVENT_QUIT:
        app.running = false;
        break;
    case SDL_EVENT_KEY_DOWN:
        if (e.key.key == SDLK_ESCAPE) {
            app.running = false;
        } else if (e.key.key == SDLK_SPACE && !e.key.repeat && app.opts.latency) {
            trigger_flash(app, e.key.timestamp);
        }
        break;
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_CANCELED:
        handle_finger(app, e.tfinger);
        break;
    case SDL_EVENT_MOUSE_MOTION: {
        count_mouse(app, e.motion.which);
        if (Win* w = find_window(app, e.motion.windowID)) {
            w->has_mouse = true;
            // Mouse positions are in window points, the renderer draws in pixels.
            int points_w = 1;
            int points_h = 1;
            SDL_GetWindowSize(w->window, &points_w, &points_h);
            w->mouse_x = e.motion.x * static_cast<float>(w->out_w) /
                         static_cast<float>(std::max(1, points_w));
            w->mouse_y = e.motion.y * static_cast<float>(w->out_h) /
                         static_cast<float>(std::max(1, points_h));
        }
        if (app.opts.all_motion || e.motion.timestamp - app.last_mouse_log_ns >= kMotionLogGapNs) {
            app.last_mouse_log_ns = e.motion.timestamp;
            std::printf("[MOUSE] motion window=%u pos=(%.0f, %.0f) %s\n", e.motion.windowID,
                        static_cast<double>(e.motion.x), static_cast<double>(e.motion.y),
                        mouse_source(e.motion.which).c_str());
        }
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        count_mouse(app, e.button.which);
        std::printf("[MOUSE] button %d %s window=%u pos=(%.0f, %.0f) %s\n", e.button.button,
                    e.button.down ? "down" : "up", e.button.windowID,
                    static_cast<double>(e.button.x), static_cast<double>(e.button.y),
                    mouse_source(e.button.which).c_str());
        if (e.button.down && app.opts.latency) {
            trigger_flash(app, e.button.timestamp);
        }
        break;
    case SDL_EVENT_DISPLAY_ADDED:
        std::printf("[EVENT] display %u added\n", e.display.displayID);
        break;
    case SDL_EVENT_DISPLAY_REMOVED:
        std::printf("[EVENT] display %u removed\n", e.display.displayID);
        break;
    case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
        std::printf("[EVENT] window %u moved to display %d\n", e.window.windowID, e.window.data1);
        break;
    default:
        break;
    }
}

// --- reports -------------------------------------------------------------------------------

void print_interval(App& app, double interval_s) {
    for (auto& w : app.wins) {
        w.shown_fps = static_cast<double>(w.interval_frames) / interval_s;
        std::printf("[FPS] window %u (%s): %.1f fps, worst frame %.1f ms, %llu frames\n", w.id,
                    w.name.c_str(), w.shown_fps, w.interval_worst_ms,
                    static_cast<unsigned long long>(w.interval_frames));
        w.interval_frames = 0;
        w.interval_worst_ms = 0;
    }
}

void summarize_frames(const App& app) {
    for (const auto& w : app.wins) {
        const std::string step = fmt("fps window %u", w.id);
        const double span = w.steady_end_s - w.steady_start_s;
        if (w.steady_start_s < 0 || span < 0.5) {
            report(Status::Skip, step, "run too short to measure (use --seconds 5 or more)");
            continue;
        }
        const double fps = static_cast<double>(w.frames) / span;
        const double want = static_cast<double>(w.refresh_hz);
        if (!app.opts.vsync) {
            report(Status::Skip, step,
                   fmt("%.1f fps without vsync (display is %.1f Hz)", fps, want));
        } else if (fps >= 0.95 * want) {
            report(Status::Pass, step,
                   fmt("%.1f fps of %.1f Hz, worst frame %.1f ms", fps, want, w.worst_ms));
        } else {
            report(Status::Fail, step,
                   fmt("%.1f fps, below 95%% of the display's %.1f Hz, worst frame %.1f ms", fps,
                       want, w.worst_ms));
        }
        const double slow_pct =
            100.0 * static_cast<double>(w.slow_frames) / static_cast<double>(w.frames);
        if (app.opts.vsync) {
            report(slow_pct <= 1.0 ? Status::Pass : Status::Fail, fmt("pacing window %u", w.id),
                   fmt("%llu of %llu frames (%.2f%%) took over 1.5 refresh periods",
                       static_cast<unsigned long long>(w.slow_frames),
                       static_cast<unsigned long long>(w.frames), slow_pct));
        }
    }
}

void summarize_input(const App& app) {
    const TouchStats& t = app.touch;
    if (t.events == 0) {
        report(Status::Skip, "touch", "no finger events; touch the panel while the spike runs");
    } else {
        std::string devices;
        for (const auto& [id, count] : t.per_device) {
            devices += fmt(" dev %llu:%llu", static_cast<unsigned long long>(id),
                           static_cast<unsigned long long>(count));
        }
        std::string windows;
        for (const auto& [id, count] : t.per_window) {
            windows += fmt(" window %u:%llu", id, static_cast<unsigned long long>(count));
        }
        report(Status::Pass, "touch",
               fmt("%llu finger events (%llu down); devices%s; delivered to%s",
                   static_cast<unsigned long long>(t.events),
                   static_cast<unsigned long long>(t.down), devices.c_str(), windows.c_str()));
        const bool tiny = t.max_x < 0.05f && t.max_y < 0.05f;
        const bool full = t.min_x < 0.1f && t.max_x > 0.9f && t.min_y < 0.1f && t.max_y > 0.9f;
        const std::string range = fmt("x %.4f..%.4f, y %.4f..%.4f", static_cast<double>(t.min_x),
                                      static_cast<double>(t.max_x), static_cast<double>(t.min_y),
                                      static_cast<double>(t.max_y));
        if (tiny) {
            report(Status::Fail, "touch range",
                   range +
                       "; looks like unscaled raw axes (the KMSDRM 65535 problem), check evtest");
        } else if (full) {
            report(Status::Pass, "touch range", range + "; reaches all four edges");
        } else {
            report(Status::Skip, "touch range", range + "; touch all four corners to confirm 0..1");
        }
    }
    if (app.mouse_real + app.mouse_from_touch == 0) {
        info("mouse: no events");
    } else {
        info(fmt("mouse: %llu real events, %llu synthesized from touch",
                 static_cast<unsigned long long>(app.mouse_real),
                 static_cast<unsigned long long>(app.mouse_from_touch)));
    }
    if (app.opts.latency) {
        info(fmt("latency triggers: %llu", static_cast<unsigned long long>(app.triggers)));
    }
}

int print_summary() {
    int pass = 0;
    int fail = 0;
    int skip = 0;
    std::puts("\n== Summary ==");
    for (const auto& r : g_results) {
        const char* tag = r.status == Status::Pass   ? "PASS"
                          : r.status == Status::Fail ? "FAIL"
                                                     : "SKIP";
        std::printf("[%s] %s\n", tag, r.text.c_str());
        (r.status == Status::Pass ? pass : r.status == Status::Fail ? fail : skip)++;
    }
    std::printf("RESULT: %s (%d pass, %d fail, %d skip)\n", fail == 0 ? "PASS" : "FAIL", pass, fail,
                skip);
    return fail == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    App app;
    if (!parse_args(argc, argv, app.opts)) {
        return 2;
    }
    if (app.opts.help) {
        print_help();
        return 0;
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        const std::string err = SDL_GetError();
        print_environment();
        report(Status::Fail, "sdl-init", "SDL_Init(VIDEO) failed: " + err);
        return print_summary();
    }
    std::signal(SIGTERM, on_signal);
    std::signal(SIGINT, on_signal);

    print_environment();
    report(Status::Pass, "sdl-init",
           fmt("video driver %s", text_or(SDL_GetCurrentVideoDriver(), "?").c_str()));

    const std::vector<SDL_DisplayID> displays = get_displays();
    std::printf("displays (%zu)\n", displays.size());
    for (const SDL_DisplayID id : displays) {
        print_display(id);
    }
    print_input_devices();

    if (displays.empty()) {
        report(Status::Fail, "displays", "SDL reports no displays");
        SDL_Quit();
        return print_summary();
    }
    report(Status::Pass, "displays", fmt("%zu display(s) found", displays.size()));
    if (app.opts.list) {
        SDL_Quit();
        return print_summary();
    }

    // Choose which displays get a window.
    std::vector<SDL_DisplayID> chosen;
    if (app.opts.display != 0) {
        if (std::find(displays.begin(), displays.end(), app.opts.display) == displays.end()) {
            report(Status::Fail, "select",
                   fmt("display id %u does not exist (see --list)", app.opts.display));
        } else {
            chosen.push_back(app.opts.display);
        }
    } else {
        const std::size_t want =
            app.opts.displays > 0 ? static_cast<std::size_t>(app.opts.displays) : displays.size();
        chosen.assign(displays.begin(), displays.begin() + static_cast<std::ptrdiff_t>(
                                                               std::min(want, displays.size())));
        if (want > displays.size()) {
            report(Status::Fail, "select",
                   fmt("%zu displays requested, only %zu found", want, displays.size()));
        }
    }
    if (app.opts.display == 0 && app.opts.displays == 0) {
        report(displays.size() >= 2 ? Status::Pass : Status::Skip, "dual display",
               displays.size() >= 2
                   ? "two or more displays are active"
                   : "only one display connected; connect the second monitor to test dual screen");
    }

    for (std::size_t i = 0; i < chosen.size(); ++i) {
        open_window(app, chosen[i], i);
    }
    if (app.wins.empty()) {
        SDL_Quit();
        return print_summary();
    }
    if (app.wins.size() > 1) {
        info("all windows must stay visible and keep animating; a window that goes black or frozen "
             "is a FAIL");
    }
    SDL_HideCursor();

    const double freq = static_cast<double>(SDL_GetPerformanceFrequency());
    const Uint64 start = SDL_GetPerformanceCounter();
    double last_report_s = 0;
    while (app.running && g_stop == 0) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            handle_event(app, event);
        }
        const double run_s = static_cast<double>(SDL_GetPerformanceCounter() - start) / freq;
        if (app.opts.seconds > 0 && run_s >= app.opts.seconds) {
            break;
        }
        for (auto& w : app.wins) {
            render_window(app, w, run_s, freq);
        }
        if (run_s - last_report_s >= kReportSeconds) {
            print_interval(app, run_s - last_report_s);
            last_report_s = run_s;
        }
    }
    std::puts(g_stop != 0 ? "stopping on signal" : "stopping");

    summarize_frames(app);
    summarize_input(app);

    for (auto& w : app.wins) {
        SDL_DestroyRenderer(w.renderer);
        SDL_DestroyWindow(w.window);
    }
    SDL_Quit();
    return print_summary();
}
