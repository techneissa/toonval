// SPDX-License-Identifier: GPL-3.0-or-later
#include "dev_script.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace toonval::app {

bool DevScript::parse(const char* option, const char* value) {
    using Kind = Step::Kind;
    long frame = 0;
    int consumed = 0;
    if (std::strcmp(option, "--info") == 0) {
        if (std::sscanf(value, "%ld%n", &frame, &consumed) != 1 || value[consumed] != '\0' ||
            frame < 0) {
            return false;
        }
        steps_.push_back({Kind::Info, frame, 0, 0, {}});
        return true;
    }
    if (std::strcmp(option, "--quit-frame") == 0) {
        return std::sscanf(value, "%ld%n", &quit_frame_, &consumed) == 1 && value[consumed] == '\0';
    }
    if (std::sscanf(value, "%ld:%n", &frame, &consumed) != 1 || consumed == 0 || frame < 0) {
        return false;
    }
    const char* rest = value + consumed;
    if (std::strcmp(option, "--tap") == 0) {
        float x = 0;
        float y = 0;
        int n = 0;
        if (std::sscanf(rest, "%f,%f%n", &x, &y, &n) != 2 || rest[n] != '\0') {
            return false;
        }
        steps_.push_back({Kind::Move, frame, x, y, {}});
        steps_.push_back({Kind::Down, frame + 1, x, y, {}});
        steps_.push_back({Kind::Up, frame + 2, x, y, {}});
        return true;
    }
    if (std::strcmp(option, "--drag") == 0) {
        float x = 0;
        float y = 0;
        float x2 = 0;
        float y2 = 0;
        int n = 0;
        if (std::sscanf(rest, "%f,%f:%f,%f%n", &x, &y, &x2, &y2, &n) != 4 || rest[n] != '\0') {
            return false;
        }
        constexpr int kSteps = 8;
        steps_.push_back({Kind::Move, frame, x, y, {}});
        steps_.push_back({Kind::Down, frame + 1, x, y, {}});
        for (int i = 1; i <= kSteps; ++i) {
            const float t = static_cast<float>(i) / kSteps;
            steps_.push_back({Kind::Move, frame + 1 + i, x + (x2 - x) * t, y + (y2 - y) * t, {}});
        }
        steps_.push_back({Kind::Up, frame + 2 + kSteps, x2, y2, {}});
        return true;
    }
    if (std::strcmp(option, "--move") == 0) {
        float x = 0;
        float y = 0;
        int n = 0;
        if (std::sscanf(rest, "%f,%f%n", &x, &y, &n) != 2 || rest[n] != '\0') {
            return false;
        }
        steps_.push_back({Kind::Move, frame, x, y, {}});
        return true;
    }
    if (std::strcmp(option, "--key") == 0) {
        const char* colon = std::strchr(rest, ':');
        const std::string name = colon != nullptr ? std::string(rest, colon) : std::string(rest);
        long hold = 3;
        if (colon != nullptr && (std::sscanf(colon + 1, "%ld%n", &hold, &consumed) != 1 ||
                                 colon[1 + consumed] != '\0' || hold < 1)) {
            return false;
        }
        if (SDL_GetScancodeFromName(name.c_str()) == SDL_SCANCODE_UNKNOWN) {
            return false;
        }
        steps_.push_back({Kind::KeyDown, frame, 0, 0, name});
        steps_.push_back({Kind::KeyUp, frame + hold, 0, 0, name});
        return true;
    }
    if (std::strcmp(option, "--press") == 0) {
        float x = 0;
        float y = 0;
        long hold = 3;
        int n = 0;
        const int got = std::sscanf(rest, "%f,%f%n", &x, &y, &n);
        if (got != 2) {
            return false;
        }
        if (rest[n] == ':' && (std::sscanf(rest + n + 1, "%ld%n", &hold, &consumed) != 1 ||
                               rest[n + 1 + consumed] != '\0' || hold < 1)) {
            return false;
        }
        if (rest[n] != '\0' && rest[n] != ':') {
            return false;
        }
        steps_.push_back({Kind::Move, frame, x, y, {}});
        steps_.push_back({Kind::Down, frame + 1, x, y, {}});
        steps_.push_back({Kind::Up, frame + 1 + hold, x, y, {}});
        return true;
    }
    if (std::strcmp(option, "--resize") == 0) {
        float w = 0;
        float h = 0;
        int n = 0;
        if (std::sscanf(rest, "%fx%f%n", &w, &h, &n) != 2 || rest[n] != '\0' || w < 1 || h < 1) {
            return false;
        }
        steps_.push_back({Kind::Resize, frame, w, h, {}});
        return true;
    }
    if (std::strcmp(option, "--type") == 0) {
        steps_.push_back({Kind::Text, frame, 0, 0, rest});
        return true;
    }
    if (std::strcmp(option, "--shot") == 0) {
        if (rest[0] == '\0') {
            return false;
        }
        steps_.push_back({Kind::Shot, frame, 0, 0, rest});
        return true;
    }
    return false;
}

void DevScript::before_events(SDL_Window* window, long frame) {
    for (const Step& s : steps_) {
        if (s.frame != frame) {
            continue;
        }
        SDL_Event e;
        SDL_zero(e);
        switch (s.kind) {
        case Step::Kind::Move:
            e.type = SDL_EVENT_MOUSE_MOTION;
            e.motion.windowID = SDL_GetWindowID(window);
            e.motion.x = s.x;
            e.motion.y = s.y;
            break;
        case Step::Kind::Down:
        case Step::Kind::Up: {
            // Move there first in the same frame: a real pointer motion that arrived since the
            // scripted move would otherwise put the press somewhere else.
            SDL_Event move;
            SDL_zero(move);
            move.type = SDL_EVENT_MOUSE_MOTION;
            move.motion.windowID = SDL_GetWindowID(window);
            move.motion.x = s.x;
            move.motion.y = s.y;
            SDL_PushEvent(&move);
        }
            e.type = s.kind == Step::Kind::Down ? SDL_EVENT_MOUSE_BUTTON_DOWN
                                                : SDL_EVENT_MOUSE_BUTTON_UP;
            e.button.windowID = SDL_GetWindowID(window);
            e.button.button = SDL_BUTTON_LEFT;
            e.button.down = s.kind == Step::Kind::Down;
            e.button.clicks = 1;
            e.button.x = s.x;
            e.button.y = s.y;
            break;
        case Step::Kind::Text:
            e.type = SDL_EVENT_TEXT_INPUT;
            e.text.windowID = SDL_GetWindowID(window);
            e.text.text = s.text.c_str(); // lives as long as this script
            break;
        case Step::Kind::KeyDown:
        case Step::Kind::KeyUp: {
            const SDL_Scancode sc = SDL_GetScancodeFromName(s.text.c_str());
            e.type = s.kind == Step::Kind::KeyDown ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            e.key.windowID = SDL_GetWindowID(window);
            e.key.scancode = sc;
            e.key.key = SDL_GetKeyFromScancode(sc, SDL_KMOD_NONE, false);
            e.key.down = s.kind == Step::Kind::KeyDown;
            e.key.repeat = false;
            break;
        }
        case Step::Kind::Resize:
            SDL_SetWindowSize(window, static_cast<int>(s.x), static_cast<int>(s.y));
            continue;
        case Step::Kind::Info: {
            int x = 0;
            int y = 0;
            int w = 0;
            int h = 0;
            int pw = 0;
            int ph = 0;
            SDL_GetWindowPosition(window, &x, &y);
            SDL_GetWindowSize(window, &w, &h);
            SDL_GetWindowSizeInPixels(window, &pw, &ph);
            std::printf("window at %d,%d size %dx%d points, %dx%d pixels\n", x, y, w, h, pw, ph);
            std::fflush(stdout);
            continue;
        }
        case Step::Kind::Shot:
            continue;
        }
        e.common.timestamp = SDL_GetTicksNS();
        SDL_PushEvent(&e);
    }
}

bool DevScript::after_draw(SDL_Renderer* renderer, long frame) {
    for (const Step& s : steps_) {
        if (s.kind != Step::Kind::Shot || s.frame != frame) {
            continue;
        }
        SDL_Surface* surface = SDL_RenderReadPixels(renderer, nullptr);
        if (surface == nullptr || !SDL_SavePNG(surface, s.text.c_str())) {
            std::fprintf(stderr, "screenshot %s failed: %s\n", s.text.c_str(), SDL_GetError());
        } else {
            std::printf("screenshot %s (%dx%d)\n", s.text.c_str(), surface->w, surface->h);
        }
        SDL_DestroySurface(surface);
    }
    return quit_frame_ >= 0 && frame >= quit_frame_;
}

} // namespace toonval::app
