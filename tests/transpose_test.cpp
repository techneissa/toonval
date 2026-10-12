// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/transpose.hpp"

#include "toonval/keyboard.hpp"

#include <doctest/doctest.h>

using namespace toonval;

TEST_CASE("transpose: the amount is clamped to -12..+12") {
    CHECK(Transposition(5, kRange61).semitones() == 5);
    CHECK(Transposition(12, kRange61).semitones() == 12);
    CHECK(Transposition(-12, kRange61).semitones() == -12);
    CHECK(Transposition(13, kRange61).semitones() == 12);
    CHECK(Transposition(-40, kRange61).semitones() == -12);
    CHECK(Transposition(1000, kRange61).semitones() == 12);
}

TEST_CASE("transpose: zero semitones leaves every note inside the keyboard alone") {
    for (const KeyRange range : {kRange61, kRange88, kRange49}) {
        const Transposition t(0, range);
        for (int p = range.lowest; p <= range.highest; ++p) {
            const TransposedPitch m = t.map(p);
            CHECK(m.playable);
            CHECK(m.pitch == p);
            CHECK_FALSE(m.folded);
            CHECK(m.fold_octaves == 0);
        }
    }
}

TEST_CASE("transpose: the default shifts nothing and folds nothing anywhere in MIDI") {
    const Transposition t;
    for (int p = 0; p <= 127; ++p) {
        const TransposedPitch m = t.map(p);
        CHECK(m.playable);
        CHECK(m.pitch == p);
        CHECK_FALSE(m.folded);
    }
}

TEST_CASE("transpose: a shift that stays on the keyboard moves the note without a marker") {
    const Transposition up(7, kRange61);
    const TransposedPitch a = up.map(60); // C4 -> G4
    CHECK(a.pitch == 67);
    CHECK_FALSE(a.folded);
    const Transposition down(-12, kRange61);
    CHECK(down.map(60).pitch == 48);
    CHECK_FALSE(down.map(60).folded);
}

TEST_CASE("transpose: every shift at the top edge of a 61-key keyboard") {
    // The highest key is 96. Playing it +s lands s above the edge; the fold brings it back by one
    // octave (s <= 12), and the lowest key at -s does the same from below.
    for (int s = 1; s <= 12; ++s) {
        const TransposedPitch top = Transposition(s, kRange61).map(96);
        CHECK(top.playable);
        CHECK(top.folded);
        CHECK(top.fold_octaves == -1);
        CHECK(top.pitch == 96 + s - 12);

        const TransposedPitch bottom = Transposition(-s, kRange61).map(36);
        CHECK(bottom.playable);
        CHECK(bottom.folded);
        CHECK(bottom.fold_octaves == 1);
        CHECK(bottom.pitch == 36 - s + 12);
    }
}

TEST_CASE("transpose: every shift at the edges of an 88-key keyboard") {
    for (int s = 1; s <= 12; ++s) {
        const TransposedPitch top = Transposition(s, kRange88).map(108);
        CHECK(top.folded);
        CHECK(top.fold_octaves == -1);
        CHECK(top.pitch == 108 + s - 12);

        const TransposedPitch bottom = Transposition(-s, kRange88).map(21);
        CHECK(bottom.folded);
        CHECK(bottom.fold_octaves == 1);
        CHECK(bottom.pitch == 21 - s + 12);
    }
}

TEST_CASE("transpose: the key just inside the edge is not folded, the key just outside is") {
    const Transposition t(3, kRange61);
    CHECK_FALSE(t.map(93).folded); // 96, the highest key
    CHECK(t.map(93).pitch == 96);
    CHECK(t.map(94).folded); // 97 -> 85
    CHECK(t.map(94).pitch == 85);
    const Transposition d(-3, kRange61);
    CHECK_FALSE(d.map(39).folded); // 36, the lowest key
    CHECK(d.map(39).pitch == 36);
    CHECK(d.map(38).folded); // 35 -> 47
    CHECK(d.map(38).pitch == 47);
}

TEST_CASE("transpose: a song note outside the keyboard folds by as many octaves as it needs") {
    // A 61-key keyboard playing a piece written for 88 keys.
    const Transposition t(0, kRange61);
    const TransposedPitch low = t.map(21); // A0
    CHECK(low.pitch == 45);
    CHECK(low.folded);
    CHECK(low.fold_octaves == 2);
    const TransposedPitch high = t.map(108); // C8 -> C7, the top key
    CHECK(high.pitch == 96);
    CHECK(high.fold_octaves == -1);
    const TransposedPitch top = t.map(127); // G9 -> G6
    CHECK(top.pitch == 91);
    CHECK(top.folded);
    CHECK(top.fold_octaves == -3);
    // Never more octaves than needed: the result is the nearest one inside the range.
    const TransposedPitch just_over = Transposition(0, kRange61).map(97);
    CHECK(just_over.pitch == 85);
    CHECK(just_over.fold_octaves == -1);
}

TEST_CASE("transpose: the shift and the fold keep the pitch class") {
    for (int s = -12; s <= 12; ++s) {
        const Transposition t(s, kRange61);
        for (int p = 0; p <= 127; ++p) {
            const TransposedPitch m = t.map(p);
            REQUIRE(m.playable);
            CHECK(kRange61.contains(m.pitch));
            CHECK(((m.pitch - (p + s)) % 12) == 0);
            CHECK(m.pitch == p + s + 12 * m.fold_octaves);
            CHECK(m.folded == (m.fold_octaves != 0));
        }
    }
}

TEST_CASE("transpose: a keyboard narrower than an octave plays what fits and reports the rest") {
    // Five keys, C4 to G4 (60..67): a note folds into it only when its pitch class is there.
    const KeyRange tiny{60, 64};
    const Transposition t(0, tiny);
    CHECK(t.map(62).pitch == 62);
    CHECK_FALSE(t.map(62).folded);
    const TransposedPitch up = t.map(50); // D3 -> D4 is inside
    CHECK(up.playable);
    CHECK(up.pitch == 62);
    CHECK(up.fold_octaves == 1);
    const TransposedPitch missing = t.map(66); // F#4: no F# anywhere in 60..64
    CHECK_FALSE(missing.playable);
    CHECK(missing.pitch == -1);
    CHECK_FALSE(missing.folded);
    CHECK(missing.fold_octaves == 0);
    // The shift is applied before the test: +2 turns the D into an E, which is also in range.
    CHECK(Transposition(2, tiny).map(62).pitch == 64);
    CHECK_FALSE(Transposition(2, tiny).map(63).playable); // F, not in range
}

TEST_CASE("transpose: a range with no keys, or outside MIDI, plays nothing and guesses nothing") {
    const Transposition backwards(0, KeyRange{80, 60});
    CHECK_FALSE(backwards.map(70).playable);
    CHECK(backwards.map(70).pitch == -1);
    // A range that reaches past 127 never gives back a note number MIDI can't send.
    const Transposition wide(0, KeyRange{0, 200});
    CHECK(wide.map(127).pitch == 127);
    CHECK(wide.map(130).pitch == 118); // a song pitch past 127 folds into MIDI's own range
    CHECK(wide.map(130).folded);
    CHECK_FALSE(Transposition(0, KeyRange{-10, -2}).map(60).playable);
}

TEST_CASE("transpose: a pitch below MIDI's range still lands inside the keyboard") {
    const TransposedPitch m = Transposition(12, kRange88).map(-3); // -3 + 12 = 9, below A0
    CHECK(m.playable);
    CHECK(m.pitch == 21);
    CHECK(m.fold_octaves == 1);
}

TEST_CASE("transpose: the same shift and range compare equal") {
    CHECK(Transposition(3, kRange61) == Transposition(3, kRange61));
    CHECK_FALSE(Transposition(3, kRange61) == Transposition(4, kRange61));
    CHECK_FALSE(Transposition(3, kRange61) == Transposition(3, kRange88));
    CHECK(Transposition(50, kRange61) == Transposition(12, kRange61));
}

TEST_CASE("transpose: expected_pitch of a note is the mapped pitch; drums are never moved") {
    Note n;
    n.pitch = 94;
    const Transposition t(5, kRange61); // 99 -> 87
    CHECK(t.expected_pitch(n).pitch == 87);
    CHECK(t.expected_pitch(n).folded);
    Note drum;
    drum.pitch = 38;
    drum.drum = true;
    CHECK(t.expected_pitch(drum).pitch == 38);
    CHECK_FALSE(t.expected_pitch(drum).folded);
    CHECK(Transposition(0, KeyRange{60, 64}).expected_pitch(drum).playable);
}
