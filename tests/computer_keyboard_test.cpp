// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/computer_keyboard.hpp"

#include "toonval/key_state.hpp"
#include "toonval/pitch.hpp"

#include <utility>
#include <vector>

#include <doctest/doctest.h>

using namespace toonval;

namespace {

using Events = std::vector<MidiEvent>;

// (scancode, semitones above the base note, label)
struct Mapped {
    int code;
    int offset;
    const char* label;
};
constexpr Mapped kMap[] = {
    {scancode::kA, 0, "A"},  {scancode::kW, 1, "W"},          {scancode::kS, 2, "S"},
    {scancode::kE, 3, "E"},  {scancode::kD, 4, "D"},          {scancode::kF, 5, "F"},
    {scancode::kT, 6, "T"},  {scancode::kG, 7, "G"},          {scancode::kY, 8, "Y"},
    {scancode::kH, 9, "H"},  {scancode::kU, 10, "U"},         {scancode::kJ, 11, "J"},
    {scancode::kK, 12, "K"}, {scancode::kO, 13, "O"},         {scancode::kL, 14, "L"},
    {scancode::kP, 15, "P"}, {scancode::kSemicolon, 16, ";"}, {scancode::kApostrophe, 17, "'"},
};

Events press(ComputerKeyboard& k, int code, std::int64_t t = 1) {
    Events out;
    k.key(code, true, false, t, out);
    return out;
}
Events release(ComputerKeyboard& k, int code, std::int64_t t = 2) {
    Events out;
    k.key(code, false, false, t, out);
    return out;
}

bool is_on(const MidiEvent& e, int note, int vel) {
    return e.type == MidiEventType::NoteOn && e.data1 == note && e.data2 == vel;
}
bool is_off(const MidiEvent& e, int note) {
    return e.type == MidiEventType::NoteOff && e.data1 == note;
}

} // namespace

TEST_CASE("the middle letter row plays the white keys from middle C") {
    ComputerKeyboard k(kRange88);
    const std::pair<int, int> whites[] = {
        {scancode::kA, 60},         {scancode::kS, 62},         {scancode::kD, 64},
        {scancode::kF, 65},         {scancode::kG, 67},         {scancode::kH, 69},
        {scancode::kJ, 71},         {scancode::kK, 72},         {scancode::kL, 74},
        {scancode::kSemicolon, 76}, {scancode::kApostrophe, 77}};
    for (const auto& [code, note] : whites) {
        CAPTURE(code);
        const Events down = press(k, code);
        REQUIRE(down.size() == 1);
        CHECK(is_on(down[0], note, k.velocity()));
        CHECK_FALSE(is_black_key(note));
        const Events up = release(k, code);
        REQUIRE(up.size() == 1);
        CHECK(is_off(up[0], note));
    }
}

TEST_CASE("the row above plays the black keys between them") {
    ComputerKeyboard k(kRange88);
    const std::pair<int, int> blacks[] = {
        {scancode::kW, 61}, {scancode::kE, 63}, {scancode::kT, 66}, {scancode::kY, 68},
        {scancode::kU, 70}, {scancode::kO, 73}, {scancode::kP, 75}};
    for (const auto& [code, note] : blacks) {
        CAPTURE(code);
        const Events down = press(k, code);
        REQUIRE(down.size() == 1);
        CHECK(down[0].data1 == note);
        CHECK(is_black_key(note));
        release(k, code);
    }
}

TEST_CASE("every mapped key plays its offset from the base note and has a label") {
    ComputerKeyboard k(kRange88);
    for (const Mapped& m : kMap) {
        CAPTURE(m.code);
        CHECK(ComputerKeyboard::is_typing_key(m.code));
        CHECK(k.note_for(m.code) == 60 + m.offset);
        CHECK(k.label_for_note(60 + m.offset) == m.label);
    }
    CHECK(k.label_for_note(59).empty());
    CHECK(k.label_for_note(78).empty());
}

TEST_CASE("events carry the typing source, channel 0 and the given time") {
    ComputerKeyboard k(kRange88);
    const Events down = press(k, scancode::kA, 12345);
    REQUIRE(down.size() == 1);
    CHECK(down[0].source == InputSource::ComputerKeyboard);
    CHECK(down[0].is_virtual());
    CHECK(down[0].channel == 0);
    CHECK(down[0].time_ns == 12345);
    CHECK_FALSE(down[0].synthetic);
}

TEST_CASE("other keys are not typing keys and do nothing") {
    ComputerKeyboard k(kRange88);
    for (const int code : {0, 5 /* B */, 12 /* I */, 20 /* Q */, 41 /* Escape */, 44 /* Space */,
                           53 /* ` */, 79 /* right arrow */, 225 /* Shift */, 1000, -3}) {
        CAPTURE(code);
        CHECK_FALSE(ComputerKeyboard::is_typing_key(code));
        CHECK(press(k, code).empty());
        CHECK(release(k, code).empty());
        CHECK_FALSE(k.note_for(code).has_value());
    }
}

TEST_CASE("auto-repeat of a held key does nothing") {
    ComputerKeyboard k(kRange88);
    CHECK(press(k, scancode::kA).size() == 1);
    Events out;
    for (int i = 0; i < 5; ++i) {
        k.key(scancode::kA, true, true, 10 + i, out);
    }
    CHECK(out.empty());
    CHECK(k.held_count() == 1);
    CHECK(release(k, scancode::kA).size() == 1);
    // A repeat after the release (a late event) does not start a note either.
    k.key(scancode::kA, true, true, 20, out);
    CHECK(out.empty());
}

TEST_CASE("repeating octave, velocity and sustain keys changes nothing more") {
    ComputerKeyboard k(kRange88);
    Events out;
    k.key(scancode::kX, true, false, 1, out);
    k.key(scancode::kX, true, true, 2, out);
    k.key(scancode::kX, true, true, 3, out);
    k.key(scancode::kX, false, false, 4, out);
    CHECK(k.octave_shift() == 1);
    k.key(scancode::kV, true, false, 5, out);
    k.key(scancode::kV, true, true, 6, out);
    CHECK(k.velocity() == 96);
    k.key(scancode::kTab, true, false, 7, out);
    k.key(scancode::kTab, true, true, 8, out);
    CHECK(out.size() == 1); // only the pedal-down
}

TEST_CASE("a second press of a held key without a release is not a new note") {
    ComputerKeyboard k(kRange88);
    CHECK(press(k, scancode::kA).size() == 1);
    CHECK(press(k, scancode::kA).empty());
    CHECK(release(k, scancode::kA).size() == 1);
    CHECK(release(k, scancode::kA).empty()); // a stray second key-up
}

TEST_CASE("Z and X shift an octave and the next note is in the new octave") {
    ComputerKeyboard k(kRange88);
    Events out;
    k.key(scancode::kX, true, false, 1, out);
    k.key(scancode::kX, false, false, 2, out);
    CHECK(out.empty());
    CHECK(k.octave_shift() == 1);
    CHECK(k.base_note() == 72);
    CHECK(press(k, scancode::kA)[0].data1 == 72);
    release(k, scancode::kA);
    k.key(scancode::kZ, true, false, 3, out);
    k.key(scancode::kZ, true, false, 3, out); // not released: a second down is ignored
    k.key(scancode::kZ, false, false, 4, out);
    k.key(scancode::kZ, true, false, 5, out);
    k.key(scancode::kZ, false, false, 6, out);
    CHECK(k.octave_shift() == -1);
    CHECK(press(k, scancode::kA)[0].data1 == 48);
}

TEST_CASE("the octave shift stops where the whole mapped span would leave the keyboard") {
    struct Case {
        KeyRange range;
        int min_shift;
        int max_shift;
    };
    // base = 60 + 12 s must be >= lowest, and base + 17 <= highest.
    const Case cases[] = {
        {kRange49, -2, 0}, {kRange61, -2, 1}, {kRange88, -3, 2}, {kRange76, -2, 2}};
    for (const Case& c : cases) {
        CAPTURE(c.range.lowest);
        ComputerKeyboard k(c.range);
        CHECK(k.min_octave_shift() == c.min_shift);
        CHECK(k.max_octave_shift() == c.max_shift);
        Events out;
        for (int i = 0; i < 12; ++i) {
            k.key(scancode::kX, true, false, i, out);
            k.key(scancode::kX, false, false, i, out);
        }
        CHECK(k.octave_shift() == c.max_shift);
        CHECK(k.base_note() + 17 <= c.range.highest);
        CHECK(*k.note_for(scancode::kApostrophe) <= c.range.highest);
        for (int i = 0; i < 12; ++i) {
            k.key(scancode::kZ, true, false, i, out);
            k.key(scancode::kZ, false, false, i, out);
        }
        CHECK(k.octave_shift() == c.min_shift);
        CHECK(k.base_note() >= c.range.lowest);
        CHECK(*k.note_for(scancode::kA) >= c.range.lowest);
        CHECK(out.empty());
    }
}

TEST_CASE("every mapped note on every shift of every keyboard is on the keyboard") {
    for (const KeyRange range : {kRange49, kRange61, kRange76, kRange88}) {
        ComputerKeyboard k(range);
        for (int shift = k.min_octave_shift(); shift <= k.max_octave_shift(); ++shift) {
            ComputerKeyboard at(range);
            Events out;
            while (at.octave_shift() < shift) {
                at.key(scancode::kX, true, false, 0, out);
                at.key(scancode::kX, false, false, 0, out);
            }
            while (at.octave_shift() > shift) {
                at.key(scancode::kZ, true, false, 0, out);
                at.key(scancode::kZ, false, false, 0, out);
            }
            for (const Mapped& m : kMap) {
                const std::optional<int> n = at.note_for(m.code);
                REQUIRE(n.has_value());
                CHECK(range.contains(*n));
            }
        }
    }
}

TEST_CASE("the starting octave is the one nearest middle C that fits the keyboard") {
    CHECK(ComputerKeyboard(kRange88).octave_shift() == 0);
    CHECK(ComputerKeyboard(kRange61).octave_shift() == 0);
    CHECK(ComputerKeyboard(kRange49).octave_shift() == 0);
    // A keyboard that stops at C5: base 60 would end at 77, too high; 48 fits.
    CHECK(ComputerKeyboard(KeyRange{36, 72}).base_note() == 48);
    // A keyboard that starts at C5.
    CHECK(ComputerKeyboard(KeyRange{72, 108}).base_note() == 72);
}

TEST_CASE("a keyboard too narrow for the span plays what fits and drops the rest") {
    ComputerKeyboard k(KeyRange{60, 72}); // 13 keys
    CHECK(k.min_octave_shift() == k.max_octave_shift());
    CHECK(press(k, scancode::kA).size() == 1);
    release(k, scancode::kA);
    // ' would be 77: off the keyboard, so nothing plays and releasing it does nothing.
    CHECK(press(k, scancode::kApostrophe).empty());
    CHECK(release(k, scancode::kApostrophe).empty());
    CHECK_FALSE(k.note_for(scancode::kApostrophe).has_value());
}

TEST_CASE("a key held across an octave change releases the note it started") {
    ComputerKeyboard k(kRange88);
    const Events down = press(k, scancode::kA);
    REQUIRE(down.size() == 1);
    CHECK(down[0].data1 == 60);
    Events out;
    k.key(scancode::kX, true, false, 3, out); // octave up while A is held
    k.key(scancode::kX, false, false, 4, out);
    CHECK(out.empty()); // the held note was not touched
    const Events up = release(k, scancode::kA, 5);
    REQUIRE(up.size() == 1);
    CHECK(is_off(up[0], 60)); // not 72
    // A new press of A now plays the new octave, and its release matches.
    CHECK(press(k, scancode::kA, 6)[0].data1 == 72);
    CHECK(is_off(release(k, scancode::kA, 7)[0], 72));
}

TEST_CASE("held keys across several octave changes each release their own note") {
    ComputerKeyboard k(kRange88);
    CHECK(press(k, scancode::kA)[0].data1 == 60);
    Events out;
    k.key(scancode::kX, true, false, 1, out);
    k.key(scancode::kX, false, false, 1, out);
    CHECK(press(k, scancode::kS)[0].data1 == 74);
    k.key(scancode::kZ, true, false, 2, out);
    k.key(scancode::kZ, false, false, 2, out);
    k.key(scancode::kZ, true, false, 2, out);
    k.key(scancode::kZ, false, false, 2, out);
    CHECK(press(k, scancode::kD)[0].data1 == 52);
    CHECK(is_off(release(k, scancode::kS)[0], 74));
    CHECK(is_off(release(k, scancode::kA)[0], 60));
    CHECK(is_off(release(k, scancode::kD)[0], 52));
    CHECK(k.held_count() == 0);
}

TEST_CASE("octave change at the keyboard limit with a key held still releases it") {
    ComputerKeyboard k(kRange49);
    for (int i = 0; i < 3; ++i) {
        Events out;
        k.key(scancode::kZ, true, false, 0, out);
        k.key(scancode::kZ, false, false, 0, out);
    }
    CHECK(k.octave_shift() == -2);
    const int low = k.base_note();
    CHECK(press(k, scancode::kA)[0].data1 == low);
    Events out;
    k.key(scancode::kX, true, false, 1, out);
    k.key(scancode::kX, false, false, 1, out);
    CHECK(is_off(release(k, scancode::kA)[0], low));
}

TEST_CASE("two typing keys holding the same note release it only when both are up") {
    ComputerKeyboard k(kRange88);
    CHECK(press(k, scancode::kK)[0].data1 == 72); // K at shift 0
    Events out;
    k.key(scancode::kX, true, false, 1, out);
    k.key(scancode::kX, false, false, 1, out);
    CHECK(press(k, scancode::kA).empty()); // A at shift 1 is also 72: already sounding
    CHECK(release(k, scancode::kK).empty());
    const Events up = release(k, scancode::kA);
    REQUIRE(up.size() == 1);
    CHECK(is_off(up[0], 72));
}

TEST_CASE("C and V step the velocity along the ladder and stop at the ends") {
    ComputerKeyboard k(kRange88);
    CHECK(k.velocity() == 80);
    Events out;
    const auto tap = [&](int code) {
        k.key(code, true, false, 0, out);
        k.key(code, false, false, 0, out);
    };
    tap(scancode::kV);
    CHECK(k.velocity() == 96);
    tap(scancode::kV);
    tap(scancode::kV);
    CHECK(k.velocity() == 127);
    tap(scancode::kV);
    CHECK(k.velocity() == 127);
    tap(scancode::kC);
    CHECK(k.velocity() == 112);
    for (int i = 0; i < 10; ++i) {
        tap(scancode::kC);
    }
    CHECK(k.velocity() == 16);
    CHECK(out.empty());
    // The new velocity is in the next note-on, and a held note keeps the one it started with.
    const Events down = press(k, scancode::kA);
    CHECK(is_on(down[0], 60, 16));
}

TEST_CASE("every velocity on the ladder is a valid MIDI velocity and they rise") {
    int previous = 0;
    for (const int v : kVelocityLevels) {
        CHECK(v > previous);
        CHECK(v >= 1);
        CHECK(v <= 127);
        previous = v;
    }
    CHECK(kVelocityLevels[kDefaultVelocityLevel] == 80);
}

TEST_CASE("Tab holds the sustain pedal: CC64 127 down, 0 up") {
    ComputerKeyboard k(kRange88);
    const Events down = press(k, scancode::kTab, 5);
    REQUIRE(down.size() == 1);
    CHECK(down[0].type == MidiEventType::ControlChange);
    CHECK(down[0].data1 == kCcSustain);
    CHECK(down[0].data2 == 127);
    CHECK(down[0].source == InputSource::ComputerKeyboard);
    CHECK(k.sustain_down());
    const Events up = release(k, scancode::kTab, 6);
    REQUIRE(up.size() == 1);
    CHECK(up[0].data1 == kCcSustain);
    CHECK(up[0].data2 == 0);
    CHECK_FALSE(k.sustain_down());
    CHECK(release(k, scancode::kTab).empty()); // no double release
}

TEST_CASE("the sustain pedal never holds a key: the note-off still arrives") {
    ComputerKeyboard k(kRange88);
    KeyState state;
    Events all;
    k.key(scancode::kTab, true, false, 1, all);
    k.key(scancode::kA, true, false, 2, all);
    k.key(scancode::kA, false, false, 3, all);
    for (const MidiEvent& e : all) {
        state.apply(e);
    }
    CHECK(state.sustain_down());
    CHECK_FALSE(state.is_down(60));
}

TEST_CASE("release_all lets go of every held note and the pedal") {
    ComputerKeyboard k(kRange88);
    press(k, scancode::kA);
    press(k, scancode::kD);
    press(k, scancode::kTab);
    Events out;
    k.release_all(99, out);
    int offs = 0;
    bool pedal_up = false;
    for (const MidiEvent& e : out) {
        CHECK(e.time_ns == 99);
        if (e.type == MidiEventType::NoteOff) {
            ++offs;
        }
        if (e.type == MidiEventType::ControlChange && e.data1 == kCcSustain && e.data2 == 0) {
            pedal_up = true;
        }
    }
    CHECK(offs == 2);
    CHECK(pedal_up);
    CHECK(k.held_count() == 0);
    CHECK_FALSE(k.sustain_down());
    // Nothing left to do the second time, and the late key-ups do nothing.
    Events again;
    k.release_all(100, again);
    CHECK(again.empty());
    CHECK(release(k, scancode::kA).empty());
}

TEST_CASE("typing events drive KeyState exactly like MIDI: down then up") {
    ComputerKeyboard k(kRange88);
    KeyState state;
    for (const MidiEvent& e : press(k, scancode::kG, 100)) {
        state.apply(e);
    }
    CHECK(state.is_down(67));
    CHECK(state.press_time_ns(67) == 100);
    CHECK(state.press_velocity(67) == 80);
    for (const MidiEvent& e : release(k, scancode::kG, 200)) {
        state.apply(e);
    }
    CHECK_FALSE(state.is_down(67));
}

TEST_CASE("changing the keyboard clamps the octave and keeps held notes releasable") {
    ComputerKeyboard k(kRange88);
    Events out;
    for (int i = 0; i < 2; ++i) {
        k.key(scancode::kX, true, false, 0, out);
        k.key(scancode::kX, false, false, 0, out);
    }
    CHECK(k.octave_shift() == 2);
    CHECK(press(k, scancode::kA)[0].data1 == 84);
    k.set_keyboard(kRange49);
    CHECK(k.octave_shift() == 0);
    CHECK(is_off(release(k, scancode::kA)[0], 84));
    CHECK(k.base_note() + 17 <= kRange49.highest);
}
