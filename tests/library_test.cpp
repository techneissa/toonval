// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/library.hpp"

#include "progress_fixtures.hpp"
#include "smf_builder.hpp"

#include "toonval/content_hash.hpp"
#include "toonval/midi_file.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

using namespace toonval;
using toonval::test::Bytes;
using toonval::test::Track;

namespace {

// Pitches of a one-track file, one note every `gap` ticks, each `gap` long.
Bytes melody_file(const std::vector<int>& pitches, int gap = 480,
                  const char* track_name = nullptr) {
    Track t;
    if (track_name != nullptr) {
        t.name(0, track_name);
    }
    for (int p : pitches) {
        t.on(0, 0, p).off(static_cast<std::uint32_t>(gap), 0, p);
    }
    t.end();
    return toonval::test::smf({t});
}

SongEntry entry_of(const Bytes& bytes, std::string_view file_name = "song.mid") {
    LibraryLoad r = load_library_entry(bytes, file_name);
    REQUIRE(r.ok);
    return r.entry;
}

Bytes read_test_file(const char* name) {
    std::ifstream in(std::string(TOONVAL_TEST_DATA_DIR) + "/" + name, std::ios::binary);
    REQUIRE(in.good());
    return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

// ---- Text

TEST_CASE("midi text: valid UTF-8 is kept") {
    CHECK(utf8_from_midi_text("Für Elise") == "Für Elise");
    CHECK(utf8_from_midi_text("") == "");
    CHECK(utf8_from_midi_text("\xE3\x82\xA2") == "\xE3\x82\xA2"); // ア
}

TEST_CASE("midi text: Latin-1 and Windows-1252 bytes become UTF-8") {
    CHECK(utf8_from_midi_text("F\xFCr Elise") == "Für Elise");
    CHECK(utf8_from_midi_text("Caf\xE9") == "Café");
    CHECK(utf8_from_midi_text("It\x92s") == "It\xE2\x80\x99s"); // 0x92 is a right quote in 1252
    CHECK(utf8_from_midi_text("a\x81z") == "a\xEF\xBF\xBDz");   // 0x81 is undefined: U+FFFD
}

TEST_CASE("midi text: Shift-JIS kana and half-width katakana are decoded, kanji are not") {
    // "さくら" in Shift-JIS: 82 B3 82 AD 82 E7
    CHECK(utf8_from_midi_text("\x82\xB3\x82\xAD\x82\xE7") == "さくら");
    // "ピアノ" in Shift-JIS katakana: 83 73 83 41 83 6D
    CHECK(utf8_from_midi_text("\x83\x73\x83\x41\x83\x6D") == "ピアノ");
    // Half-width katakana "ﾋﾟｱﾉ": CB DF B1 C9
    CHECK(utf8_from_midi_text("\xCB\xDF\xB1\xC9") == "ﾋﾟｱﾉ");
    // Kana mixed with ASCII and a kanji (92 86 = 中, not in the built-in table): U+FFFD for the
    // kanji
    CHECK(utf8_from_midi_text("Song \x82\xB3\x92\x86") == "Song さ\xEF\xBF\xBD");
}

TEST_CASE("midi text: the result is always valid UTF-8, whatever the bytes") {
    std::string all;
    for (int i = 0; i < 256; ++i) {
        all.push_back(static_cast<char>(i));
    }
    for (int start = 0; start < 256; start += 7) {
        std::string slice = all.substr(static_cast<std::size_t>(start), 40);
        CAPTURE(start);
        CHECK(is_valid_utf8(utf8_from_midi_text(slice)));
    }
    CHECK(is_valid_utf8(utf8_from_midi_text(all)));
    CHECK(is_valid_utf8(utf8_from_midi_text("\xE3\x82"))); // cut-off UTF-8
}

TEST_CASE("is_valid_utf8 refuses overlong forms, surrogates and cut-off sequences") {
    CHECK(is_valid_utf8("plain"));
    CHECK(is_valid_utf8("\xC3\xBC"));
    CHECK_FALSE(is_valid_utf8("\xC0\xAF"));         // overlong '/'
    CHECK_FALSE(is_valid_utf8("\xED\xA0\x80"));     // surrogate
    CHECK_FALSE(is_valid_utf8("\xF4\x90\x80\x80")); // above U+10FFFF
    CHECK_FALSE(is_valid_utf8("\xE3\x82"));         // truncated
    CHECK_FALSE(is_valid_utf8("\xFC"));
}

TEST_CASE("title from a file name: no folder, no extension, underscores become spaces") {
    CHECK(title_from_file_name("Fur_Elise.mid") == "Fur Elise");
    CHECK(title_from_file_name("/usb/Songs/Moonlight__Sonata.MIDI") == "Moonlight Sonata");
    CHECK(title_from_file_name("C:\\Music\\Gymnopedie 1.mid") == "Gymnopedie 1");
    CHECK(title_from_file_name("  spaced   out .kar") == "spaced out");
    CHECK(title_from_file_name("Op. 27 No.2") == "Op. 27 No.2"); // not an extension we know
    CHECK(title_from_file_name("song.musicxml") == "song");
    CHECK(title_from_file_name("song.mxl") == "song");
    CHECK(title_from_file_name("no_extension") == "no extension");
    CHECK(title_from_file_name(".mid") == "");
    CHECK(title_from_file_name("") == "");
}

TEST_CASE("title from a file name: control characters go and the length is capped") {
    CHECK(title_from_file_name("a\tb\nc.mid") == "a b c");
    std::string long_name(1000, 'x');
    std::string t = title_from_file_name(long_name + ".mid");
    CHECK(t.size() == kMaxRunTitleBytes);
    // Cut at a character boundary, never inside one.
    std::string wide;
    for (int i = 0; i < 300; ++i) {
        wide += "é";
    }
    std::string cut = title_from_file_name(wide);
    CHECK(is_valid_utf8(cut));
    CHECK(cut.size() <= kMaxRunTitleBytes);
    CHECK(cut.size() >= kMaxRunTitleBytes - 1);
}

TEST_CASE("title: the file name wins, the sequence name fills in for a generic file name") {
    CHECK(choose_title("Fur_Elise.mid", "control track") == "Fur Elise");
    CHECK(choose_title("song1.mid", "Moonlight Sonata") == "song1"); // song1 is not generic
    CHECK(choose_title("untitled.mid", "Moonlight Sonata") == "Moonlight Sonata");
    CHECK(choose_title("0001.mid", "Moonlight Sonata") == "Moonlight Sonata");
    CHECK(choose_title("midi.mid", "Prelude in C") == "Prelude in C");
    CHECK(choose_title("", "Prelude in C") == "Prelude in C");
}

TEST_CASE("title: when nothing is usable it is 'Untitled', and mojibake never wins") {
    CHECK(choose_title("", "") == "Untitled");
    CHECK(choose_title("untitled.mid", "control track") == "untitled");
    CHECK(choose_title("untitled.mid", "") == "untitled");
    // A name of only replacement characters is not a title.
    CHECK(choose_title("\xEF\xBF\xBD\xEF\xBF\xBD.mid", "Real Name") == "Real Name");
    // Shift-JIS file name (bytes), decoded to kana.
    CHECK(choose_title("\x82\xB3\x82\xAD\x82\xE7.mid", "") == "さくら");
}

TEST_CASE("sequence name: the first track's name event") {
    Bytes file =
        toonval::test::smf({Track().name(0, "Prelude in C").end(),
                            Track().name(0, "Right Hand").on(0, 0, 60).off(480, 0, 60).end()});
    CHECK(midi_sequence_name(file) == "Prelude in C");
    // Format 0: the one track's name.
    Bytes single = melody_file({60, 62}, 480, "Waltz");
    CHECK(midi_sequence_name(single) == "Waltz");
    Bytes none = melody_file({60, 62});
    CHECK(midi_sequence_name(none).empty());
}

TEST_CASE("sequence name: never reads outside a truncated or hostile file") {
    Bytes file = toonval::test::smf({Track().name(0, "Long Name Here").end()});
    for (std::size_t cut = 0; cut <= file.size(); ++cut) {
        Bytes part(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(cut));
        std::string name = midi_sequence_name(part);
        CHECK(name.size() <= std::string("Long Name Here").size());
    }
    Bytes junk(300, 0xFF);
    CHECK(midi_sequence_name(junk).empty());
    CHECK(midi_sequence_name(Bytes{}).empty());
    // A name event whose length runs past the end of the data.
    Bytes bad = toonval::test::header(0, 1, 480);
    Bytes body = {0x00, 0xFF, 0x03, 0x7F, 'a', 'b'};
    toonval::test::put_chunk(bad, "MTrk", body);
    CHECK(midi_sequence_name(bad).empty());
}

TEST_CASE("search: case and accents are ignored") {
    CHECK(fold_for_search("Für Elise") == "fur elise");
    CHECK(fold_for_search("ÉTUDE Op.10") == "etude op.10");
    CHECK(fold_for_search("Dvořák — Largo") == "dvorak — largo");
    CHECK(fold_for_search("Łódź") == "lodz");
    CHECK(fold_for_search("Straße") == "strasse");
    CHECK(fold_for_search("Ærø Œuvre") == "aero oeuvre");
    CHECK(fold_for_search("Cre\xCC\x80me") == "creme"); // e + combining grave
    CHECK(fold_for_search("ＡＢＣ１２３") == "abc123"); // full-width
    CHECK(fold_for_search("Привет") == "привет");
    CHECK(fold_for_search("ΑΒΓ") == "αβγ");
    CHECK(fold_for_search("") == "");
}

TEST_CASE("search: every word of the query must appear in the title") {
    std::string key = fold_for_search("Für Elise (Beethoven)");
    CHECK(matches_search(key, ""));
    CHECK(matches_search(key, "   "));
    CHECK(matches_search(key, "elise"));
    CHECK(matches_search(key, "FUR"));
    CHECK(matches_search(key, "für elise"));
    CHECK(matches_search(key, "beet fur"));
    CHECK(matches_search(key, "Elíse"));
    CHECK_FALSE(matches_search(key, "elise mozart"));
    CHECK_FALSE(matches_search(key, "xyz"));
}

// ---- Entries

TEST_CASE("entry: hash, duration, notes per hand and pitch range") {
    Track right;
    right.name(0, "Right Hand").on(0, 0, 72).off(480, 0, 72).on(0, 0, 76).off(480, 0, 76).end();
    Track left;
    left.name(0, "Left Hand").on(0, 1, 40).off(960, 1, 40).end();
    Bytes file = toonval::test::smf({Track().bpm(0, 120).end(), right, left});
    SongEntry e = entry_of(file, "Two_Hands.mid");
    CHECK(e.hash == content_hash(file.data(), file.size()));
    CHECK(e.title == "Two Hands");
    CHECK(e.title_key == "two hands");
    CHECK(e.duration_s == doctest::Approx(1.0));
    CHECK(e.note_count == 3);
    CHECK(e.right_notes == 2);
    CHECK(e.left_notes == 1);
    CHECK(e.unknown_notes == 0);
    CHECK(e.hands_known());
    CHECK(e.lowest_pitch == 40);
    CHECK(e.highest_pitch == 76);
    CHECK(e.bar_count >= 1);
}

TEST_CASE("entry: a one-track song has unknown hands, drums are not counted") {
    Track t;
    t.on(0, 0, 60).off(240, 0, 60).on(0, 9, 36).off(240, 9, 36).on(0, 0, 62).off(240, 0, 62).end();
    SongEntry e = entry_of(toonval::test::smf({t}));
    CHECK(e.note_count == 2);
    CHECK(e.unknown_notes == 2);
    CHECK_FALSE(e.hands_known());
    CHECK(e.lowest_pitch == 60);
    CHECK(e.highest_pitch == 62);
}

TEST_CASE("entry: a file with no playable notes has no pitch range") {
    Track t;
    t.bpm(0, 120).end();
    SongEntry e = entry_of(toonval::test::smf({t}));
    CHECK(e.note_count == 0);
    CHECK(e.lowest_pitch == -1);
    CHECK(e.highest_pitch == -1);
    CHECK(e.difficulty == 1);
    CHECK(fit_to_keyboard(e, kRange61).fit == KeyFit::Fits);
}

TEST_CASE("entry: a file that is not MIDI is reported, never thrown") {
    Bytes junk = {1, 2, 3, 4};
    LibraryLoad r = load_library_entry(junk, "junk.mid");
    CHECK_FALSE(r.ok);
    CHECK(r.error == MidiFileError::NotMidi);
}

TEST_CASE("entry: the title uses the file name, then the sequence name") {
    Bytes named = toonval::test::smf(
        {Track().name(0, "Prelude in C").end(), Track().on(0, 0, 60).off(480, 0, 60).end()});
    CHECK(entry_of(named, "bwv846.mid").title == "bwv846");
    CHECK(entry_of(named, "untitled.mid").title == "Prelude in C");
    CHECK(entry_of(named, "").title == "Prelude in C");
}

// ---- Keyboard fit

TEST_CASE("fit: a song inside the keyboard fits with no shift") {
    SongEntry e = entry_of(melody_file({48, 60, 72, 84}));
    KeyboardFit f = fit_to_keyboard(e, kRange61); // 36..96
    CHECK(f.fit == KeyFit::Fits);
    CHECK(f.semitones == 0);
    CHECK(f.fits());
}

TEST_CASE("fit: a song a little too low or too high needs the smallest shift that fits") {
    {
        SongEntry e = entry_of(melody_file({33, 60})); // 33 is 3 under 36
        KeyboardFit f = fit_to_keyboard(e, kRange61);
        CHECK(f.fit == KeyFit::FitsWithShift);
        CHECK(f.semitones == 3);
        CHECK(f.fits());
    }
    {
        SongEntry e = entry_of(melody_file({60, 100})); // 100 is 4 over 96
        KeyboardFit f = fit_to_keyboard(e, kRange61);
        CHECK(f.fit == KeyFit::FitsWithShift);
        CHECK(f.semitones == -4);
    }
    {
        // 61-key keyboard, song 30..96: shift +6 fits exactly (36..102 no; 30..96 needs +6
        // -> 36..102)
        SongEntry e = entry_of(melody_file({30, 90})); // 60 wide, keyboard is 60 wide: 36..96
        KeyboardFit f = fit_to_keyboard(e, kRange61);
        CHECK(f.fit == KeyFit::FitsWithShift);
        CHECK(f.semitones == 6);
    }
}

TEST_CASE("fit: a song wider than the keyboard, or needing more than an octave, is too wide") {
    SongEntry wide = entry_of(melody_file({21, 108}));
    CHECK(fit_to_keyboard(wide, kRange61).fit == KeyFit::TooWide);
    CHECK_FALSE(fit_to_keyboard(wide, kRange61).fits());
    CHECK(fit_to_keyboard(wide, kRange88).fit == KeyFit::Fits);
    // Fits the keyboard's width but sits 14 semitones away: more than the +/-12 shift allows.
    SongEntry far = entry_of(melody_file({22, 30})); // 22..30
    KeyboardFit f = fit_to_keyboard(far, KeyRange{44, 96});
    CHECK(f.fit == KeyFit::TooWide);
    CHECK(f.semitones == 0);
}

TEST_CASE("fit: the shift limit is exactly an octave") {
    SongEntry low = entry_of(melody_file({36, 40}));
    KeyboardFit f = fit_to_keyboard(low, KeyRange{48, 72});
    CHECK(f.fit == KeyFit::FitsWithShift);
    CHECK(f.semitones == 12);
    SongEntry too_low = entry_of(melody_file({35, 40}));
    CHECK(fit_to_keyboard(too_low, KeyRange{48, 72}).fit == KeyFit::TooWide);
}

// ---- Difficulty

TEST_CASE("difficulty: levels are 1..5 and never fall when any factor rises") {
    DifficultyFactors base;
    base.onsets_per_second = 0.5;
    base.notes_per_onset = 1.0;
    base.mean_chord_span = 0.0;
    base.pitch_range = 5;
    int previous_level = difficulty_level(base);
    CHECK(previous_level == 1);

    auto check_monotonic = [&](auto setter, double from, double to, double step) {
        int last = 0;
        for (double v = from; v <= to; v += step) {
            DifficultyFactors f = base;
            setter(f, v);
            int level = difficulty_level(f);
            CAPTURE(v);
            CHECK(level >= 1);
            CHECK(level <= 5);
            CHECK(level >= last);
            last = level;
        }
        return last;
    };
    int a = check_monotonic([](DifficultyFactors& f, double v) { f.onsets_per_second = v; }, 0, 20,
                            0.25);
    int b =
        check_monotonic([](DifficultyFactors& f, double v) { f.notes_per_onset = v; }, 1, 8, 0.25);
    int c =
        check_monotonic([](DifficultyFactors& f, double v) { f.mean_chord_span = v; }, 0, 40, 0.5);
    int d = check_monotonic([](DifficultyFactors& f, double v) { f.pitch_range = v; }, 0, 100, 1);
    CHECK(a >= 3); // speed alone can carry a song to the middle of the scale or beyond
    CHECK(b >= 1);
    CHECK(c >= 1);
    CHECK(d >= 1);

    DifficultyFactors hardest;
    hardest.onsets_per_second = 12;
    hardest.notes_per_onset = 4;
    hardest.mean_chord_span = 16;
    hardest.pitch_range = 80;
    CHECK(difficulty_level(hardest) == 5);
    DifficultyFactors silly;
    silly.onsets_per_second = 1e9;
    silly.notes_per_onset = 1e9;
    silly.mean_chord_span = 1e9;
    silly.pitch_range = 1e9;
    CHECK(difficulty_level(silly) == 5);
    DifficultyFactors negative;
    negative.onsets_per_second = -3;
    negative.notes_per_onset = -3;
    negative.mean_chord_span = -3;
    negative.pitch_range = -3;
    CHECK(difficulty_level(negative) == 1);
}

TEST_CASE("difficulty factors: a slow single-note tune, a fast one, and one with chords") {
    // 8 quarter notes at 120 BPM, one voice: 2 onsets a second, no chords.
    std::vector<int> pitches = {60, 62, 64, 65, 67, 69, 71, 72};
    SongEntry slow = entry_of(melody_file(pitches, 480));
    CHECK(slow.factors.onsets_per_second == doctest::Approx(2.0).epsilon(0.1));
    CHECK(slow.factors.notes_per_onset == doctest::Approx(1.0));
    CHECK(slow.factors.mean_chord_span == doctest::Approx(0.0));
    CHECK(slow.factors.pitch_range == doctest::Approx(12.0));
    CHECK(slow.difficulty <= 2);

    // The same notes four times as fast.
    SongEntry fast = entry_of(melody_file(pitches, 120));
    CHECK(fast.factors.onsets_per_second > 3.0 * slow.factors.onsets_per_second);
    CHECK(fast.difficulty >= slow.difficulty);

    // Triads in one hand: three notes per onset, spanning a fifth.
    Track t;
    for (int i = 0; i < 8; ++i) {
        int root = 60 + i;
        t.on(0, 0, root).on(0, 0, root + 4).on(0, 0, root + 7);
        t.off(480, 0, root).off(0, 0, root + 4).off(0, 0, root + 7);
    }
    t.end();
    SongEntry chords = entry_of(toonval::test::smf({t}));
    CHECK(chords.factors.notes_per_onset == doctest::Approx(3.0));
    CHECK(chords.factors.mean_chord_span == doctest::Approx(7.0));
    CHECK(chords.difficulty >= slow.difficulty);
}

TEST_CASE("difficulty factors: two hands playing together count per hand, not as one chord") {
    Track right;
    right.name(0, "Right");
    Track left;
    left.name(0, "Left");
    for (int i = 0; i < 8; ++i) {
        right.on(0, 0, 72 + i).off(480, 0, 72 + i);
        left.on(0, 1, 40 + i).off(480, 1, 40 + i);
    }
    SongEntry e =
        entry_of(toonval::test::smf({Track().bpm(0, 120).end(), right.end(), left.end()}));
    CHECK(e.hands_known());
    CHECK(e.factors.notes_per_onset == doctest::Approx(1.0));
    CHECK(e.factors.mean_chord_span == doctest::Approx(0.0));
    CHECK(e.factors.pitch_range == doctest::Approx(39.0));
}

TEST_CASE("difficulty: the real files, for the record") {
    SongEntry bach = entry_of(read_test_file("bach-bwv846-prelude.mid"), "bach-bwv846-prelude.mid");
    SongEntry satie = entry_of(read_test_file("satie-gymnopedie-1.mid"), "satie-gymnopedie-1.mid");
    MESSAGE("bach: " << bach.difficulty << " (" << bach.factors.onsets_per_second << " onsets/s, "
                     << bach.factors.notes_per_onset << " notes/onset, span "
                     << bach.factors.mean_chord_span << ", range " << bach.factors.pitch_range
                     << ")");
    MESSAGE("satie: " << satie.difficulty << " (" << satie.factors.onsets_per_second
                      << " onsets/s, " << satie.factors.notes_per_onset << " notes/onset, span "
                      << satie.factors.mean_chord_span << ", range " << satie.factors.pitch_range
                      << ")");
    CHECK(bach.difficulty >= 1);
    CHECK(bach.difficulty <= 5);
    CHECK(satie.difficulty >= 1);
    CHECK(satie.difficulty <= 5);
    // The prelude's constant sixteenth notes are busier than the Gymnopedie's slow waltz.
    CHECK(bach.factors.onsets_per_second > satie.factors.onsets_per_second);
    CHECK(bach.difficulty >= satie.difficulty);
    CHECK(bach.title == "bach-bwv846-prelude");
    CHECK(bach.hands_known());
    CHECK(bach.lowest_pitch == 36);
    CHECK(bach.highest_pitch == 81);
    CHECK(fit_to_keyboard(bach, kRange61).fit == KeyFit::Fits);
    KeyboardFit satie_on_49 = fit_to_keyboard(satie, kRange49); // 33..81 on 36..84
    CHECK(satie_on_49.fit == KeyFit::FitsWithShift);
    CHECK(satie_on_49.semitones == 3);
}

// ---- History

TEST_CASE("history: an entry with no runs has no best and was never played") {
    SongEntry e = entry_of(melody_file({60, 62}));
    attach_history(e, {});
    CHECK_FALSE(e.has_best);
    CHECK(e.run_count == 0);
    CHECK(e.last_run_index == -1);
}

TEST_CASE("history: the best score is the highest accuracy, ties go to the higher speed") {
    SongEntry e = entry_of(melody_file({60, 62}));
    using toonval::testing::make;
    using toonval::testing::RunSpec;
    auto run = [&](std::uint64_t song, int graded, int missed, int speed, std::int32_t day) {
        RunSpec s;
        s.song = song;
        s.graded = graded;
        s.missed = missed;
        s.speed = speed;
        s.day = day;
        return make(s);
    };
    std::vector<RunRecord> runs = {
        run(e.hash, 40, 10, 100, 10), // 75%
        run(999, 40, 0, 100, 11),     // another song, ignored
        run(e.hash, 40, 4, 100, 12),  // 90%
        run(e.hash, 40, 4, 120, 13),  // 90% at a higher speed: the best
        run(e.hash, 40, 8, 100, 14),  // 80%
        run(999, 40, 0, 100, 15),
    };
    attach_history(e, runs);
    REQUIRE(e.has_best);
    CHECK(e.best.accuracy == doctest::Approx(0.9));
    CHECK(e.best.speed_percent == 120);
    CHECK(e.run_count == 4);
    CHECK(e.last_run_index == 4);
    CHECK(e.last_run_day == 14);
}

TEST_CASE("history: runs too short to count are played but not a best") {
    SongEntry e = entry_of(melody_file({60, 62}));
    toonval::testing::RunSpec s;
    s.song = e.hash;
    s.graded = 3; // fewer than kMinGradedForBest
    s.missed = 0;
    std::vector<RunRecord> runs = {toonval::testing::make(s)};
    attach_history(e, runs);
    CHECK_FALSE(e.has_best);
    CHECK(e.run_count == 1);
    CHECK(e.last_run_index == 0);
}

TEST_CASE("history: attach_history replaces what an earlier call set") {
    SongEntry e = entry_of(melody_file({60, 62}));
    toonval::testing::RunSpec s;
    s.song = e.hash;
    std::vector<RunRecord> runs = {toonval::testing::make(s)};
    attach_history(e, runs);
    REQUIRE(e.run_count == 1);
    attach_history(e, {});
    CHECK(e.run_count == 0);
    CHECK_FALSE(e.has_best);
    CHECK(e.last_run_index == -1);
}

// ---- Browsing

namespace {

struct Lib {
    std::vector<SongEntry> entries;
};

SongEntry make_entry(std::string title, int difficulty, int lowest, int highest,
                     int last_run_index = -1) {
    SongEntry e;
    e.title = title;
    e.title_key = fold_for_search(title);
    e.difficulty = difficulty;
    e.lowest_pitch = lowest;
    e.highest_pitch = highest;
    e.note_count = 10;
    e.hash = static_cast<std::uint64_t>(std::hash<std::string>{}(title));
    e.run_count = last_run_index >= 0 ? 1 : 0;
    e.last_run_index = last_run_index;
    return e;
}

std::vector<std::string> titles(const std::vector<SongEntry>& entries,
                                const std::vector<std::size_t>& order) {
    std::vector<std::string> out;
    for (std::size_t i : order) {
        out.push_back(entries[i].title);
    }
    return out;
}

} // namespace

TEST_CASE("browse: sort by title ignores case and accents, ties fall back to the hash") {
    std::vector<SongEntry> entries = {
        make_entry("zebra", 1, 60, 70), make_entry("Étude", 1, 60, 70),
        make_entry("apple", 1, 60, 70), make_entry("Éclair", 1, 60, 70)};
    LibrarySort sort;
    sort.key = SortKey::Title;
    auto order = browse_library(entries, {}, sort);
    CHECK(titles(entries, order) == std::vector<std::string>{"apple", "Éclair", "Étude", "zebra"});
    sort.descending = true;
    order = browse_library(entries, {}, sort);
    CHECK(titles(entries, order) == std::vector<std::string>{"zebra", "Étude", "Éclair", "apple"});
}

TEST_CASE("browse: sort by difficulty, easy first, equal levels by title") {
    std::vector<SongEntry> entries = {make_entry("c", 3, 60, 70), make_entry("a", 3, 60, 70),
                                      make_entry("b", 1, 60, 70), make_entry("d", 5, 60, 70)};
    LibrarySort sort;
    sort.key = SortKey::Difficulty;
    CHECK(titles(entries, browse_library(entries, {}, sort)) ==
          std::vector<std::string>{"b", "a", "c", "d"});
    sort.descending = true;
    CHECK(titles(entries, browse_library(entries, {}, sort)) ==
          std::vector<std::string>{"d", "a", "c", "b"});
}

TEST_CASE("browse: recently played puts the latest run first and never-played songs last") {
    std::vector<SongEntry> entries = {
        make_entry("never", 1, 60, 70), make_entry("old", 1, 60, 70, 2),
        make_entry("new", 1, 60, 70, 40), make_entry("mid", 1, 60, 70, 10),
        make_entry("aaa never", 1, 60, 70)};
    LibrarySort sort;
    sort.key = SortKey::RecentlyPlayed;
    sort.descending = true; // the natural order for this key
    CHECK(titles(entries, browse_library(entries, {}, sort)) ==
          std::vector<std::string>{"new", "mid", "old", "aaa never", "never"});
    sort.descending = false; // oldest first, never-played still last
    CHECK(titles(entries, browse_library(entries, {}, sort)) ==
          std::vector<std::string>{"old", "mid", "new", "aaa never", "never"});
}

TEST_CASE("browse: sort by length") {
    std::vector<SongEntry> entries = {make_entry("long", 1, 60, 70), make_entry("short", 1, 60, 70),
                                      make_entry("mid", 1, 60, 70)};
    entries[0].duration_s = 300;
    entries[1].duration_s = 30;
    entries[2].duration_s = 120;
    LibrarySort sort;
    sort.key = SortKey::Length;
    CHECK(titles(entries, browse_library(entries, {}, sort)) ==
          std::vector<std::string>{"short", "mid", "long"});
}

TEST_CASE("browse: filter by difficulty range") {
    std::vector<SongEntry> entries = {make_entry("a", 1, 60, 70), make_entry("b", 2, 60, 70),
                                      make_entry("c", 3, 60, 70), make_entry("d", 4, 60, 70),
                                      make_entry("e", 5, 60, 70)};
    LibraryFilter f;
    f.min_difficulty = 2;
    f.max_difficulty = 4;
    CHECK(titles(entries, browse_library(entries, f, {})) ==
          std::vector<std::string>{"b", "c", "d"});
    f.min_difficulty = 4;
    f.max_difficulty = 2; // an empty range matches nothing, it does not swap
    CHECK(browse_library(entries, f, {}).empty());
}

TEST_CASE("browse: filter by what fits my keyboard") {
    std::vector<SongEntry> entries = {make_entry("fits", 1, 40, 90), make_entry("shift", 1, 33, 90),
                                      make_entry("wide", 1, 21, 108),
                                      make_entry("empty", 1, -1, -1)};
    entries[3].note_count = 0;
    LibraryFilter f;
    f.range = kRange61; // 36..96
    f.keyboard = KeyboardFilter::Any;
    CHECK(browse_library(entries, f, {}).size() == 4);
    f.keyboard = KeyboardFilter::FitsAsIs;
    CHECK(titles(entries, browse_library(entries, f, {})) ==
          std::vector<std::string>{"empty", "fits"});
    f.keyboard = KeyboardFilter::FitsWithShift;
    CHECK(titles(entries, browse_library(entries, f, {})) ==
          std::vector<std::string>{"empty", "fits", "shift"});
}

TEST_CASE("browse: filter by played or not") {
    std::vector<SongEntry> entries = {make_entry("played", 1, 60, 70, 3),
                                      make_entry("fresh", 1, 60, 70)};
    LibraryFilter f;
    f.played = PlayedFilter::PlayedOnly;
    CHECK(titles(entries, browse_library(entries, f, {})) == std::vector<std::string>{"played"});
    f.played = PlayedFilter::UnplayedOnly;
    CHECK(titles(entries, browse_library(entries, f, {})) == std::vector<std::string>{"fresh"});
}

TEST_CASE("browse: text search, filters and sort work together") {
    std::vector<SongEntry> entries = {
        make_entry("Für Elise", 3, 40, 90), make_entry("Elise Waltz", 2, 40, 90),
        make_entry("Moonlight Sonata", 4, 40, 90), make_entry("Elise Etude", 5, 21, 108)};
    LibraryFilter f;
    f.text = "ELISE";
    f.keyboard = KeyboardFilter::FitsAsIs;
    f.range = kRange61;
    LibrarySort sort;
    sort.key = SortKey::Difficulty;
    CHECK(titles(entries, browse_library(entries, f, sort)) ==
          std::vector<std::string>{"Elise Waltz", "Für Elise"});
}

TEST_CASE("browse: the result holds indices into the list that was passed") {
    std::vector<SongEntry> entries = {make_entry("b", 1, 60, 70), make_entry("a", 1, 60, 70)};
    LibrarySort sort;
    auto order = browse_library(entries, {}, sort);
    REQUIRE(order.size() == 2);
    CHECK(order[0] == 1);
    CHECK(order[1] == 0);
}
