// SPDX-License-Identifier: GPL-3.0-or-later
// Developer-only scripted input for checking the UI without a person at the screen: taps, drags,
// typed text, PNG screenshots of the rendered frame and an exit, each on a given frame number.
// Events are pushed into SDL's queue, so the UI sees them exactly like a touch. Coordinates are
// window points. Nothing here runs unless one of the options is given.
//
//   --tap FRAME:X,Y            press and release at X,Y (three frames)
//   --drag FRAME:X,Y:X2,Y2     press at X,Y, move to X2,Y2 over eight frames, release
//   --move FRAME:X,Y           move the mouse to X,Y without pressing (a hover)
//   --type FRAME:TEXT          type TEXT into the focused text field
//   --shot FRAME:PATH          save the frame as a PNG
//   --key FRAME:NAME[:N]       press the key with this SDL name (A, Semicolon, Tab, ...) and
//                              release it N frames later (default 3)
//   --press FRAME:X,Y[:N]      press the left mouse button at X,Y and release it N frames later
//   --resize FRAME:WxH         resize the window to W x H points
//   --info FRAME               print the window's position, size and pixel size
//   --quit-frame FRAME         exit after this frame
#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>
#include <vector>

namespace toonval::app {

class DevScript {
  public:
    // Takes one of the options above and its value; false if it is not one of them or is badly
    // written.
    bool parse(const char* option, const char* value);
    bool active() const { return !steps_.empty() || quit_frame_ >= 0; }

    // Pushes the events due on `frame`. Call before polling events.
    void before_events(SDL_Window* window, long frame);
    // Saves screenshots due on `frame`; call after drawing, before presenting. True when the
    // script is over and the app should exit.
    bool after_draw(SDL_Renderer* renderer, long frame);

  private:
    struct Step {
        enum class Kind {
            Move,
            Down,
            Up,
            Text,
            Shot,
            Resize,
            Info,
            KeyDown,
            KeyUp
        } kind = Kind::Move;
        long frame = 0;
        float x = 0.0f;
        float y = 0.0f;
        std::string text; // typed text or the PNG path
    };
    std::vector<Step> steps_;
    long quit_frame_ = -1;
};

} // namespace toonval::app
