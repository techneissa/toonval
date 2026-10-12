// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/key_state.hpp"

#include <doctest/doctest.h>

using namespace toonval;

namespace {
MidiEvent note_on(int note, int velocity, std::int64_t t = 0, int channel = 0) {
    return {t, MidiEventType::NoteOn, static_cast<std::uint8_t>(channel),
            static_cast<std::uint8_t>(note), static_cast<std::uint8_t>(velocity)};
}
MidiEvent note_off(int note, int velocity = 0, std::int64_t t = 0, int channel = 0) {
    return {t, MidiEventType::NoteOff, static_cast<std::uint8_t>(channel),
            static_cast<std::uint8_t>(note), static_cast<std::uint8_t>(velocity)};
}
MidiEvent cc(int controller, int value, std::int64_t t = 0, int channel = 0) {
    return {t, MidiEventType::ControlChange, static_cast<std::uint8_t>(channel),
            static_cast<std::uint8_t>(controller), static_cast<std::uint8_t>(value)};
}
} // namespace

TEST_CASE("a new KeyState has nothing held") {
    KeyState keys;
    CHECK(keys.held_count() == 0);
    CHECK(keys.held_notes().empty());
    CHECK_FALSE(keys.is_down(60));
    CHECK_FALSE(keys.sustain_down());
    CHECK_FALSE(keys.press_time_ns(60).has_value());
}

TEST_CASE("note-on presses a key and note-off releases it") {
    KeyState keys;
    KeyTransition down = keys.apply(note_on(60, 90, 1000));
    CHECK(down.type == KeyTransitionType::Pressed);
    CHECK(down.note == 60);
    CHECK(down.velocity == 90);
    CHECK(keys.is_down(60));
    CHECK(keys.held_count() == 1);

    KeyTransition up = keys.apply(note_off(60, 33, 2000));
    CHECK(up.type == KeyTransitionType::Released);
    CHECK(up.note == 60);
    CHECK(up.velocity == 33);
    CHECK_FALSE(keys.is_down(60));
    CHECK(keys.held_count() == 0);
}

TEST_CASE("velocity-0 note-on releases the key") {
    KeyState keys;
    keys.apply(note_on(60, 90));
    KeyTransition up = keys.apply(note_on(60, 0));
    CHECK(up.type == KeyTransitionType::Released);
    CHECK(up.note == 60);
    CHECK_FALSE(keys.is_down(60));
}

TEST_CASE("velocity-0 note-on for a key that is up is a no-op") {
    KeyState keys;
    CHECK(keys.apply(note_on(60, 0)).type == KeyTransitionType::None);
    CHECK_FALSE(keys.is_down(60));
}

TEST_CASE("held notes come back in ascending order") {
    KeyState keys;
    keys.apply(note_on(67, 80));
    keys.apply(note_on(60, 80));
    keys.apply(note_on(64, 80));
    keys.apply(note_on(0, 80));
    keys.apply(note_on(127, 80));
    CHECK(keys.held_notes() == std::vector<std::uint8_t>{0, 60, 64, 67, 127});
    keys.apply(note_off(64));
    CHECK(keys.held_notes() == std::vector<std::uint8_t>{0, 60, 67, 127});
}

TEST_CASE("press time and velocity of a held key come from its note-on") {
    KeyState keys;
    keys.apply(note_on(60, 77, 5'000'000));
    keys.apply(note_on(64, 50, 9'000'000));
    CHECK(keys.press_time_ns(60) == 5'000'000);
    CHECK(keys.press_time_ns(64) == 9'000'000);
    CHECK(keys.press_velocity(60) == 77);
    keys.apply(note_off(60));
    CHECK_FALSE(keys.press_time_ns(60).has_value());
    CHECK_FALSE(keys.press_velocity(60).has_value());
    CHECK(keys.press_time_ns(64) == 9'000'000);
}

TEST_CASE("a repeated note-on for a held key is not a second press") {
    KeyState keys;
    keys.apply(note_on(60, 90, 1000));
    KeyTransition again = keys.apply(note_on(60, 120, 2000));
    CHECK(again.type == KeyTransitionType::None);
    CHECK(keys.held_count() == 1);
    CHECK(keys.press_time_ns(60) == 1000);
    CHECK(keys.press_velocity(60) == 90);
    // One note-off still releases it.
    CHECK(keys.apply(note_off(60)).type == KeyTransitionType::Released);
    CHECK_FALSE(keys.is_down(60));
}

TEST_CASE("a note-off for a key that is not down is a no-op") {
    KeyState keys;
    keys.apply(note_on(64, 80));
    CHECK(keys.apply(note_off(60)).type == KeyTransitionType::None);
    CHECK(keys.held_notes() == std::vector<std::uint8_t>{64});
    keys.apply(note_off(64));
    CHECK(keys.apply(note_off(64)).type == KeyTransitionType::None);
    CHECK(keys.held_count() == 0);
}

TEST_CASE("sustain pedal never makes or keeps a key held") {
    KeyState keys;
    CHECK(keys.apply(cc(64, 127)).type == KeyTransitionType::None);
    CHECK(keys.sustain_down());
    CHECK(keys.held_count() == 0);
    CHECK(keys.held_notes().empty());

    keys.apply(note_on(60, 90));
    keys.apply(note_off(60));
    CHECK_FALSE(keys.is_down(60)); // pedal down does not keep it held
    CHECK(keys.sustain_down());

    keys.apply(note_on(62, 90));
    CHECK(keys.apply(cc(64, 0)).type == KeyTransitionType::None);
    CHECK_FALSE(keys.sustain_down());
    CHECK(keys.is_down(62)); // pedal up does not release a key that is still down
}

TEST_CASE("sustain pedal is down at value 64 and above") {
    KeyState keys;
    keys.apply(cc(64, 63));
    CHECK_FALSE(keys.sustain_down());
    keys.apply(cc(64, 64));
    CHECK(keys.sustain_down());
    keys.apply(cc(64, 127));
    CHECK(keys.sustain_down());
    keys.apply(cc(64, 1));
    CHECK_FALSE(keys.sustain_down());
}

TEST_CASE("CC123 All Notes Off releases every held key and reports it") {
    KeyState keys;
    keys.apply(note_on(60, 90));
    keys.apply(note_on(64, 90));
    keys.apply(note_on(67, 90));
    keys.apply(cc(64, 127));
    KeyTransition t = keys.apply(cc(123, 0));
    CHECK(t.type == KeyTransitionType::AllReleased);
    CHECK(t.released_count == 3);
    CHECK(keys.held_count() == 0);
    CHECK(keys.held_notes().empty());
    CHECK_FALSE(keys.is_down(64));
    CHECK_FALSE(keys.press_time_ns(60).has_value());
    CHECK(keys.sustain_down()); // the pedal is a separate switch

    // A press after the panic works normally.
    CHECK(keys.apply(note_on(64, 80)).type == KeyTransitionType::Pressed);
}

TEST_CASE("CC120 All Sound Off releases every held key and reports it") {
    KeyState keys;
    keys.apply(note_on(60, 90));
    keys.apply(note_on(72, 90));
    KeyTransition t = keys.apply(cc(120, 0));
    CHECK(t.type == KeyTransitionType::AllReleased);
    CHECK(t.released_count == 2);
    CHECK(keys.held_count() == 0);
}

TEST_CASE("All Notes Off with nothing held reports no change") {
    KeyState keys;
    CHECK(keys.apply(cc(123, 0)).type == KeyTransitionType::None);
    CHECK(keys.apply(cc(120, 0)).type == KeyTransitionType::None);
}

TEST_CASE("clear releases every key and lifts the pedal") {
    KeyState keys;
    keys.apply(note_on(60, 90));
    keys.apply(cc(64, 127));
    KeyTransition t = keys.clear();
    CHECK(t.type == KeyTransitionType::AllReleased);
    CHECK(t.released_count == 1);
    CHECK(keys.held_count() == 0);
    CHECK_FALSE(keys.sustain_down());
    CHECK(keys.clear().type == KeyTransitionType::None);
}

TEST_CASE("other controllers never change key state") {
    KeyState keys;
    keys.apply(note_on(60, 90));
    for (int controller : {0, 1, 7, 10, 11, 64 + 1, 66, 67, 91, 119, 121, 122, 124, 127}) {
        CHECK(keys.apply(cc(controller, 127)).type == KeyTransitionType::None);
    }
    CHECK(keys.held_notes() == std::vector<std::uint8_t>{60});
    CHECK_FALSE(keys.sustain_down());
}

TEST_CASE("notes outside 0..127 never change state") {
    KeyState keys;
    CHECK(keys.apply(note_on(128, 90)).type == KeyTransitionType::None);
    CHECK(keys.apply(note_on(255, 90)).type == KeyTransitionType::None);
    CHECK(keys.held_count() == 0);
    keys.apply(note_on(60, 90));
    CHECK(keys.apply(note_off(200)).type == KeyTransitionType::None);
    CHECK(keys.held_notes() == std::vector<std::uint8_t>{60});
    CHECK_FALSE(keys.is_down(-1));
    CHECK_FALSE(keys.is_down(128));
    CHECK_FALSE(keys.press_time_ns(128).has_value());
    CHECK_FALSE(keys.press_time_ns(-1).has_value());
}

TEST_CASE("non-note events never change state") {
    KeyState keys;
    keys.apply(note_on(60, 90));
    MidiEvent other{100, MidiEventType::Other, 0, 60, 0};
    CHECK(keys.apply(other).type == KeyTransitionType::None);
    CHECK(keys.held_notes() == std::vector<std::uint8_t>{60});
    // An Other event shaped like a note-off must not release.
    MidiEvent fake{100, MidiEventType::Other, 0, 60, 90};
    CHECK(keys.apply(fake).type == KeyTransitionType::None);
    CHECK(keys.is_down(60));
}

TEST_CASE("key state works for any channel") {
    KeyState keys;
    CHECK(keys.apply(note_on(60, 90, 0, 9)).type == KeyTransitionType::Pressed);
    CHECK(keys.apply(note_on(62, 90, 0, 15)).type == KeyTransitionType::Pressed);
    CHECK(keys.held_notes() == std::vector<std::uint8_t>{60, 62});
    CHECK(keys.apply(note_off(60, 0, 0, 3)).type == KeyTransitionType::Released);
    CHECK(keys.apply(cc(64, 127, 0, 7)).type == KeyTransitionType::None);
    CHECK(keys.sustain_down());
    CHECK(keys.apply(cc(123, 0, 0, 12)).type == KeyTransitionType::AllReleased);
    CHECK(keys.held_count() == 0);
}

TEST_CASE("keys outside any keyboard range are still tracked") {
    KeyState keys;
    keys.apply(note_on(0, 90));
    keys.apply(note_on(127, 90));
    CHECK(keys.is_down(0));
    CHECK(keys.is_down(127));
    CHECK(keys.held_notes() == std::vector<std::uint8_t>{0, 127});
}

TEST_CASE("decoded raw bytes drive key state end to end") {
    KeyState keys;
    const std::uint8_t on[] = {0x90, 60, 100};
    const std::uint8_t off_as_zero[] = {0x90, 60, 0};
    CHECK(keys.apply(decode_midi(on, 3, 10)).type == KeyTransitionType::Pressed);
    CHECK(keys.press_time_ns(60) == 10);
    CHECK(keys.apply(decode_midi(off_as_zero, 3, 20)).type == KeyTransitionType::Released);
    const std::uint8_t clock[] = {0xF8};
    CHECK(keys.apply(decode_midi(clock, 1, 30)).type == KeyTransitionType::None);
}

TEST_CASE("held_notes into a buffer replaces its contents with the held notes ascending") {
    KeyState keys;
    keys.apply(note_on(67, 80));
    keys.apply(note_on(60, 80));
    std::vector<std::uint8_t> out{1, 2, 3};
    keys.held_notes(out);
    CHECK(out == std::vector<std::uint8_t>{60, 67});
    keys.clear();
    keys.held_notes(out);
    CHECK(out.empty());
}
