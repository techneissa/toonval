// SPDX-License-Identifier: GPL-3.0-or-later
// The settings: a centred sheet over a dimmed screen, the pages down the left (icons with names,
// or icons with captions when narrow) and the page on the right, in two columns when wide. The
// Quick page with the most used settings comes first, so most visits need one tap.
#pragma once

#include "bar.hpp"
#include "kit.hpp"
#include "pages.hpp"

namespace toonval::app::ui {

class SettingsSheet {
  public:
    // Shows `p`; a sub-page opens under its rail page.
    void open_at(Page p);
    // Shows the rail page last chosen (Quick at first).
    void reopen() { open_at(last_); }
    // Draws the sheet `open` (0..1, eased) of the way in. Returns true when it asks to close: the
    // close button or a tap on the dimmed screen.
    bool draw(Kit& k, UiModel& model, const Frame& f, float open);

  private:
    // Page changes fade and lift the new page; a sub-page pushes in from the right.
    struct Nav {
        Page page = Page::Quick;
        float t = 1.0f;
        float dir = 0.0f; // +1 push, -1 pop, 0 a switch between rail pages

        void go(Page p, float d) {
            if (p == page) {
                return;
            }
            page = p;
            t = 0.0f;
            dir = d;
        }
        void update(float dt) { t = approach(t, 1.0f, dt, tok::kMove); }
        float offset(float distance) const { return (1.0f - ease_out(t)) * distance * dir; }
        float alpha() const { return 0.25f + 0.75f * ease_out(t); }
    };

    Nav nav_;
    Page last_ = Page::Quick;
};

} // namespace toonval::app::ui
