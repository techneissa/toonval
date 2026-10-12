// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/pointer_keys.hpp"

#include "toonval/key_state.hpp"
#include "toonval/pitch.hpp"

#include <cmath>
#include <random>
#include <vector>

#include <doctest/doctest.h>

using namespace toonval;

namespace {

using Events = std::vector<MidiEvent>;

constexpr RectF kArea{0.0f, 500.0f, 1200.0f, 200.0f}; // keys y 500..700, black keys to y 624

PointerGeometry whole(KeyRange range = kRange61) {
    KeyViewFollower f;
    f.snap(range, range);
    const KeyViewFrame fr = f.frame();
    return {KeyboardLayout(fr.layout_range), fr, kArea};
}

// x of the middle of a white key, or the boundary line a black key is centred on.
float centre_x(const PointerGeometry& g, int note) {
    const KeyRect r = g.view.to_window(g.layout.key(note), g.layout.white_key_count());
    return kArea.x + (r.x + r.width / 2.0f) * kArea.w;
}

int notes_on(const Events& e) {
    int n = 0;
    for (const MidiEvent& ev : e) {
        n += ev.type == MidiEventType::NoteOn ? 1 : 0;
    }
    return n;
}

} // namespace

TEST_CASE("a point on a white key hits that key") {
    const PointerGeometry g = whole();
    for (int note = 36; note <= 96; ++note) {
        if (is_black_key(note)) {
            continue;
        }
        const auto hit = hit_test_key(g, centre_x(g, note), 680.0f);
        REQUIRE(hit.has_value());
        CHECK(hit->note == note);
    }
}

TEST_CASE("in the upper part a black key is hit, below it the white key under it") {
    const PointerGeometry g = whole();
    const float x = centre_x(g, 37); // C#2
    CHECK(hit_test_key(g, x, 510.0f)->note == 37);
    CHECK(hit_test_key(g, x, 620.0f)->note == 37);
    // Just under the black key's tip the point is on the white key whose area it falls in.
    const auto below = hit_test_key(g, x, 630.0f);
    REQUIRE(below.has_value());
    CHECK_FALSE(is_black_key(below->note));
    // The boundary line between C and D is where C# is centred: with the point left of centre the
    // white key is C, right of it D.
    CHECK(hit_test_key(g, x - 1.0f, 630.0f)->note == 36);
    CHECK(hit_test_key(g, x + 1.0f, 630.0f)->note == 38);
}

TEST_CASE("the black key's edges and the white keys beside it") {
    const PointerGeometry g = whole();
    const float x = centre_x(g, 37);
    const float half = 0.5f * KeyboardLayout::kBlackWidthRatio * (kArea.w / 36.0f);
    CHECK(hit_test_key(g, x + half - 0.5f, 520.0f)->note == 37);
    CHECK(hit_test_key(g, x - half + 0.5f, 520.0f)->note == 37);
    CHECK(hit_test_key(g, x + half + 0.5f, 520.0f)->note == 38);
    CHECK(hit_test_key(g, x - half - 0.5f, 520.0f)->note == 36);
}

TEST_CASE("every note on the keyboard can be hit") {
    const PointerGeometry g = whole(kRange88);
    for (int note = 21; note <= 108; ++note) {
        const float y = is_black_key(note) ? 520.0f : 680.0f;
        const auto hit = hit_test_key(g, centre_x(g, note), y);
        REQUIRE(hit.has_value());
        CHECK(hit->note == note);
    }
}

TEST_CASE("points outside the key area hit nothing") {
    const PointerGeometry g = whole();
    CHECK_FALSE(hit_test_key(g, 600.0f, 499.0f).has_value());
    CHECK_FALSE(hit_test_key(g, 600.0f, 700.0f).has_value());
    CHECK_FALSE(hit_test_key(g, -1.0f, 600.0f).has_value());
    CHECK_FALSE(hit_test_key(g, 1200.0f, 600.0f).has_value());
    CHECK(hit_test_key(g, 0.0f, 500.0f).has_value());
    CHECK(hit_test_key(g, 1199.9f, 699.9f).has_value());
    const float nan = std::nanf("");
    CHECK_FALSE(hit_test_key(g, nan, 600.0f).has_value());
    CHECK_FALSE(hit_test_key(g, 600.0f, nan).has_value());
    PointerGeometry empty = g;
    empty.key_area = {};
    CHECK_FALSE(hit_test_key(empty, 600.0f, 600.0f).has_value());
}

TEST_CASE("velocity grows from the back of the key to the front") {
    const PointerGeometry g = whole();
    const float x = centre_x(g, 48);
    const int back = hit_test_key(g, x, 500.0f)->velocity;
    const int middle = hit_test_key(g, x, 600.0f)->velocity;
    const int front = hit_test_key(g, x, 699.9f)->velocity;
    CHECK(back == kPointerMinVelocity);
    CHECK(front >= 126);
    CHECK(front <= kPointerMaxVelocity);
    CHECK(back < middle);
    CHECK(middle < front);
    // Linear: the middle is halfway.
    CHECK(middle ==
          doctest::Approx((kPointerMinVelocity + kPointerMaxVelocity) / 2.0).epsilon(0.02));
}

TEST_CASE("velocity never decreases going down a white key and stays in 1..127") {
    const PointerGeometry g = whole();
    const float x = centre_x(g, 52);
    int previous = 0;
    for (float y = 500.0f; y < 700.0f; y += 0.5f) {
        const auto hit = hit_test_key(g, x, y);
        REQUIRE(hit.has_value());
        CHECK(hit->velocity >= previous);
        CHECK(hit->velocity >= 1);
        CHECK(hit->velocity <= 127);
        previous = hit->velocity;
    }
}

TEST_CASE("a black key's velocity runs over its own length") {
    const PointerGeometry g = whole();
    const float x = centre_x(g, 37);
    CHECK(hit_test_key(g, x, 500.0f)->velocity == kPointerMinVelocity);
    CHECK(hit_test_key(g, x, 623.0f)->velocity >= 125);
}

TEST_CASE("hit testing follows a panned and zoomed view") {
    KeyViewFollower f;
    f.snap(kRange88, KeyRange{48, 83});
    f.advance(kRange88, KeyRange{60, 95}, 0.05);
    const KeyViewFrame fr = f.frame();
    REQUIRE(fr.pan > 0.0f);
    const PointerGeometry g{KeyboardLayout(fr.layout_range), fr, kArea};
    for (int note = fr.layout_range.lowest + 2; note <= fr.layout_range.highest - 2; ++note) {
        if (is_black_key(note)) {
            continue;
        }
        const auto hit = hit_test_key(g, centre_x(g, note), 680.0f);
        REQUIRE(hit.has_value());
        CHECK(hit->note == note);
    }
}

TEST_CASE("pointer events carry the pointer source, channel 0 and the time") {
    const PointerGeometry g = whole();
    PointerKeys p;
    Events out;
    p.down(1, centre_x(g, 60), 650.0f, g, 777, out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].type == MidiEventType::NoteOn);
    CHECK(out[0].source == InputSource::Pointer);
    CHECK(out[0].is_virtual());
    CHECK(out[0].data1 == 60);
    CHECK(out[0].channel == 0);
    CHECK(out[0].time_ns == 777);
    out.clear();
    p.up(1, 888, out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].type == MidiEventType::NoteOff);
    CHECK(out[0].data1 == 60);
    CHECK(out[0].time_ns == 888);
    CHECK(out[0].source == InputSource::Pointer);
}

TEST_CASE("each finger holds one note and a chord plays all of them") {
    const PointerGeometry g = whole();
    PointerKeys p;
    Events out;
    p.down(1, centre_x(g, 60), 650.0f, g, 1, out);
    p.down(2, centre_x(g, 64), 650.0f, g, 2, out);
    p.down(3, centre_x(g, 67), 650.0f, g, 3, out);
    CHECK(notes_on(out) == 3);
    CHECK(p.active_pointers() == 3);
    CHECK(p.held_notes() == 3);
    CHECK(p.note_of(2) == 64);
    out.clear();
    p.up(2, 4, out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].data1 == 64);
    CHECK(p.held_notes() == 2);
    CHECK(p.note_of(1) == 60);
}

TEST_CASE("a press that lands on no key plays nothing and the pointer stays unused") {
    const PointerGeometry g = whole();
    PointerKeys p;
    Events out;
    p.down(1, 600.0f, 300.0f, g, 1, out); // in the note area
    CHECK(out.empty());
    CHECK(p.active_pointers() == 0);
    // Dragging that pointer onto the keys does not play: it never went down on a key.
    p.move(1, centre_x(g, 60), 650.0f, g, 2, out);
    CHECK(out.empty());
    p.up(1, 3, out);
    CHECK(out.empty());
}

TEST_CASE("moving onto a new key releases the old one and presses the new (glissando)") {
    const PointerGeometry g = whole();
    PointerKeys p;
    Events out;
    p.down(1, centre_x(g, 60), 650.0f, g, 1, out);
    out.clear();
    p.move(1, centre_x(g, 60) + 1.0f, 660.0f, g, 2, out); // same key
    CHECK(out.empty());
    p.move(1, centre_x(g, 62), 650.0f, g, 3, out);
    REQUIRE(out.size() == 2);
    CHECK(out[0].type == MidiEventType::NoteOff);
    CHECK(out[0].data1 == 60);
    CHECK(out[1].type == MidiEventType::NoteOn);
    CHECK(out[1].data1 == 62);
    CHECK(p.held_notes() == 1);
    CHECK(p.note_of(1) == 62);
}

TEST_CASE("dragging across keys plays each key passed, one at a time") {
    const PointerGeometry g = whole();
    PointerKeys p;
    Events out;
    KeyState state;
    const auto apply = [&] {
        for (const MidiEvent& e : out) {
            state.apply(e);
        }
        out.clear();
    };
    p.down(1, centre_x(g, 60), 650.0f, g, 1, out);
    apply();
    int max_held = 0;
    int pressed = 1;
    for (float x = centre_x(g, 60); x < centre_x(g, 72); x += 2.0f) {
        p.move(1, x, 650.0f, g, 2, out);
        pressed += notes_on(out);
        apply();
        max_held = std::max(max_held, state.held_count());
    }
    CHECK(max_held == 1);
    CHECK(pressed >= 8); // C D E F G A B on the way, black keys in the lower part are skipped
    p.up(1, 3, out);
    apply();
    CHECK(state.held_count() == 0);
}

TEST_CASE("a drag through the upper part plays the black keys it crosses") {
    const PointerGeometry g = whole();
    PointerKeys p;
    Events out;
    bool saw_black = false;
    p.down(1, centre_x(g, 60), 520.0f, g, 1, out);
    for (float x = centre_x(g, 60); x < centre_x(g, 72); x += 1.0f) {
        out.clear();
        p.move(1, x, 520.0f, g, 2, out);
        for (const MidiEvent& e : out) {
            saw_black = saw_black || (e.type == MidiEventType::NoteOn && is_black_key(e.data1));
        }
    }
    CHECK(saw_black);
}

TEST_CASE("leaving the key area releases the note, coming back presses it again") {
    const PointerGeometry g = whole();
    PointerKeys p;
    Events out;
    p.down(1, centre_x(g, 60), 650.0f, g, 1, out);
    out.clear();
    p.move(1, centre_x(g, 60), 300.0f, g, 2, out); // up into the note area
    REQUIRE(out.size() == 1);
    CHECK(out[0].type == MidiEventType::NoteOff);
    CHECK_FALSE(p.note_of(1).has_value());
    CHECK(p.active_pointers() == 1); // still down
    out.clear();
    p.move(1, centre_x(g, 60), 400.0f, g, 3, out);
    CHECK(out.empty());
    p.move(1, centre_x(g, 64), 650.0f, g, 4, out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].type == MidiEventType::NoteOn);
    CHECK(out[0].data1 == 64);
    out.clear();
    p.up(1, 5, out);
    CHECK(out.size() == 1);
    CHECK(p.active_pointers() == 0);
}

TEST_CASE("leaving off the side of the keyboard releases too") {
    const PointerGeometry g = whole();
    PointerKeys p;
    Events out;
    p.down(1, 5.0f, 650.0f, g, 1, out);
    out.clear();
    p.move(1, -50.0f, 650.0f, g, 2, out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].type == MidiEventType::NoteOff);
}

TEST_CASE("an up for a pointer that is not down does nothing") {
    PointerKeys p;
    Events out;
    p.up(9, 1, out);
    p.move(9, 1.0f, 1.0f, whole(), 1, out);
    CHECK(out.empty());
}

TEST_CASE("a second finger on the same key shares the note until both let go") {
    const PointerGeometry g = whole();
    PointerKeys p;
    Events out;
    p.down(1, centre_x(g, 60), 650.0f, g, 1, out);
    p.down(2, centre_x(g, 60) + 3.0f, 600.0f, g, 2, out);
    CHECK(notes_on(out) == 1); // one note-on
    out.clear();
    p.up(1, 3, out);
    CHECK(out.empty()); // finger 2 still holds it
    p.up(2, 4, out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].type == MidiEventType::NoteOff);
}

TEST_CASE("release_all lets go of every pointer's note exactly once") {
    const PointerGeometry g = whole();
    PointerKeys p;
    Events out;
    p.down(1, centre_x(g, 60), 650.0f, g, 1, out);
    p.down(2, centre_x(g, 64), 650.0f, g, 2, out);
    p.down(3, centre_x(g, 64), 650.0f, g, 3, out);
    out.clear();
    p.release_all(9, out);
    int offs = 0;
    for (const MidiEvent& e : out) {
        CHECK(e.type == MidiEventType::NoteOff);
        CHECK(e.time_ns == 9);
        ++offs;
    }
    CHECK(offs == 2);
    CHECK(p.active_pointers() == 0);
    CHECK(p.held_notes() == 0);
    out.clear();
    p.release_all(10, out);
    CHECK(out.empty());
    p.up(1, 11, out); // a late up
    CHECK(out.empty());
}

TEST_CASE("more pointers than slots: the extra ones play nothing and nothing sticks") {
    const PointerGeometry g = whole();
    PointerKeys p;
    Events out;
    for (std::uint64_t id = 0; id < 40; ++id) {
        p.down(id, centre_x(g, 36 + static_cast<int>(id % 20)), 650.0f, g, 1, out);
    }
    CHECK(p.active_pointers() <= static_cast<int>(PointerKeys::kMaxPointers));
    for (std::uint64_t id = 0; id < 40; ++id) {
        p.up(id, 2, out);
    }
    CHECK(p.held_notes() == 0);
    KeyState state;
    for (const MidiEvent& e : out) {
        state.apply(e);
    }
    CHECK(state.held_count() == 0);
}

TEST_CASE("property: random pointer storms always end with every key up") {
    std::mt19937 rng(42);
    const PointerGeometry g = whole(kRange88);
    for (int round = 0; round < 200; ++round) {
        PointerKeys p;
        Events out;
        KeyState state;
        for (int i = 0; i < 300; ++i) {
            const std::uint64_t id = rng() % 6;
            const float x = static_cast<float>(rng() % 1300) - 50.0f;
            const float y = 480.0f + static_cast<float>(rng() % 260);
            switch (rng() % 3) {
            case 0:
                p.down(id, x, y, g, i, out);
                break;
            case 1:
                p.move(id, x, y, g, i, out);
                break;
            default:
                p.up(id, i, out);
                break;
            }
            for (const MidiEvent& e : out) {
                state.apply(e);
            }
            // The state always agrees with the pointers.
            REQUIRE(state.held_count() == p.held_notes());
            out.clear();
        }
        p.release_all(999, out);
        for (const MidiEvent& e : out) {
            state.apply(e);
        }
        CHECK(state.held_count() == 0);
    }
}
