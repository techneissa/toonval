// SPDX-License-Identifier: GPL-3.0-or-later
#include "virtual_input.hpp"

#include "midi_input.hpp"

#include <imgui.h>

#include <algorithm>

namespace toonval::app {

namespace {

// The mouse is one pointer; fingers use SDL's finger ids, which never reach this value.
constexpr PointerKeys::PointerId kMousePointer = ~PointerKeys::PointerId{0};
constexpr std::int64_t kMaxEventAgeNs = 1'000'000'000;

// SDL scancodes are USB HID usage ids; the core's key table relies on it.
static_assert(scancode::kA == SDL_SCANCODE_A && scancode::kC == SDL_SCANCODE_C &&
                  scancode::kD == SDL_SCANCODE_D && scancode::kE == SDL_SCANCODE_E &&
                  scancode::kF == SDL_SCANCODE_F && scancode::kG == SDL_SCANCODE_G &&
                  scancode::kH == SDL_SCANCODE_H && scancode::kJ == SDL_SCANCODE_J &&
                  scancode::kK == SDL_SCANCODE_K && scancode::kL == SDL_SCANCODE_L &&
                  scancode::kO == SDL_SCANCODE_O && scancode::kP == SDL_SCANCODE_P &&
                  scancode::kS == SDL_SCANCODE_S && scancode::kT == SDL_SCANCODE_T &&
                  scancode::kU == SDL_SCANCODE_U && scancode::kV == SDL_SCANCODE_V &&
                  scancode::kW == SDL_SCANCODE_W && scancode::kX == SDL_SCANCODE_X &&
                  scancode::kY == SDL_SCANCODE_Y && scancode::kZ == SDL_SCANCODE_Z &&
                  scancode::kTab == SDL_SCANCODE_TAB &&
                  scancode::kSemicolon == SDL_SCANCODE_SEMICOLON &&
                  scancode::kApostrophe == SDL_SCANCODE_APOSTROPHE,
              "core scancode constants must match SDL's");

} // namespace

std::int64_t event_time_ns(std::uint64_t sdl_timestamp_ns) {
    const std::int64_t now = midi_clock_ns();
    const auto age =
        static_cast<std::int64_t>(SDL_GetTicksNS()) - static_cast<std::int64_t>(sdl_timestamp_ns);
    return now - (age < 0 || age > kMaxEventAgeNs ? 0 : age);
}

VirtualInput::VirtualInput() = default;

void VirtualInput::set_geometry(const KeyboardLayout& layout, const KeyViewFrame& view,
                                const RectF& key_area, float panel_x) {
    geometry_.emplace(PointerGeometry{layout, view, key_area});
    panel_x_ = panel_x;
}

float VirtualInput::pixel_scale(SDL_Window* window) {
    int w = 0;
    int h = 0;
    int pw = 0;
    int ph = 0;
    SDL_GetWindowSize(window, &w, &h);
    SDL_GetWindowSizeInPixels(window, &pw, &ph);
    return w > 0 ? static_cast<float>(pw) / static_cast<float>(w) : 1.0f;
}

bool VirtualInput::handle(const SDL_Event& event, SDL_Window* window, bool ui_wants_keys,
                          std::vector<MidiEvent>& out) {
    const ImGuiIO& io = ImGui::GetIO();
    const auto under_panel = [&](float x) { return panel_x_ >= 0.0f && x >= panel_x_; };
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const int code = static_cast<int>(event.key.scancode);
        if (!ComputerKeyboard::is_typing_key(code)) {
            return false;
        }
        const bool down = event.key.down;
        const bool chord_shortcut =
            (event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) != 0;
        if (down && (ui_wants_keys || io.WantCaptureKeyboard || chord_shortcut)) {
            return false;
        }
        typing_.key(code, down, event.key.repeat, event_time_ns(event.key.timestamp), out);
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (event.button.button != SDL_BUTTON_LEFT || event.button.which == SDL_TOUCH_MOUSEID) {
            return false; // a finger's own events play; SDL's copy of it as a mouse does not
        }
        const std::int64_t t = event_time_ns(event.button.timestamp);
        if (!event.button.down) {
            pointers_.up(kMousePointer, t, out);
            return false;
        }
        const float scale = pixel_scale(window);
        const float x = event.button.x * scale;
        if (!geometry_ || io.WantCaptureMouse || under_panel(x)) {
            return false;
        }
        const std::size_t before = out.size();
        pointers_.down(kMousePointer, x, event.button.y * scale, *geometry_, t, out);
        return pointers_.active_pointers() > 0 || out.size() != before;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        if (event.motion.which == SDL_TOUCH_MOUSEID || !geometry_) {
            return false;
        }
        const float scale = pixel_scale(window);
        pointers_.move(kMousePointer, event.motion.x * scale, event.motion.y * scale, *geometry_,
                       event_time_ns(event.motion.timestamp), out);
        return false;
    }
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_UP: {
        // Only a touch screen plays; a trackpad's fingers are not on the keys.
        if (SDL_GetTouchDeviceType(event.tfinger.touchID) != SDL_TOUCH_DEVICE_DIRECT) {
            return false;
        }
        const std::int64_t t = event_time_ns(event.tfinger.timestamp);
        const PointerKeys::PointerId id = event.tfinger.fingerID;
        if (event.type == SDL_EVENT_FINGER_UP) {
            pointers_.up(id, t, out);
            return false;
        }
        if (!geometry_) {
            return false;
        }
        int pw = 0;
        int ph = 0;
        SDL_GetWindowSizeInPixels(window, &pw, &ph);
        const float x = event.tfinger.x * static_cast<float>(pw);
        const float y = event.tfinger.y * static_cast<float>(ph);
        if (event.type == SDL_EVENT_FINGER_DOWN) {
            if (!under_panel(x)) {
                pointers_.down(id, x, y, *geometry_, t, out);
            }
        } else {
            pointers_.move(id, x, y, *geometry_, t, out);
        }
        return true;
    }
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        release_all(out);
        return false;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        pointers_.up(kMousePointer, event_time_ns(event.window.timestamp), out);
        return false;
    default:
        return false;
    }
}

void VirtualInput::release_all(std::vector<MidiEvent>& out) {
    const std::int64_t now = midi_clock_ns();
    typing_.release_all(now, out);
    pointers_.release_all(now, out);
}

} // namespace toonval::app
