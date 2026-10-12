// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/note_window.hpp"

#include "toonval/keyboard.hpp"
#include "toonval/transpose.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <random>
#include <vector>

using namespace toonval;

namespace {

Note make_note(double start_s, double duration_s, Hand hand = Hand::Right, bool drum = false) {
    Note n;
    n.start_s = start_s;
    n.duration_s = duration_s;
    n.hand = hand;
    n.drum = drum;
    return n;
}

Song song_of(std::vector<Note> notes) {
    Song s;
    s.notes = std::move(notes);
    return s;
}

std::vector<std::uint32_t> indices(const std::vector<VisibleNote>& v) {
    std::vector<std::uint32_t> out;
    for (const VisibleNote& n : v) {
        out.push_back(n.index);
    }
    return out;
}

NoteWindowOptions window(double ahead, double behind = 0.0) {
    NoteWindowOptions o;
    o.ahead_s = ahead;
    o.behind_s = behind;
    return o;
}

} // namespace

TEST_CASE("note window: y is 0 at the hit line and 1 at the top, for the start and the end") {
    NoteWindow w(song_of({make_note(12.0, 1.0)}));
    std::vector<VisibleNote> out;
    w.query(10.0, window(4.0), out); // sees 10..14
    REQUIRE(out.size() == 1);
    CHECK(out[0].index == 0);
    CHECK(out[0].bottom == doctest::Approx(0.5)); // starts 2 s ahead of 4
    CHECK(out[0].top == doctest::Approx(0.75));
    w.query(12.0, window(4.0), out); // the note is at the hit line
    REQUIRE(out.size() == 1);
    CHECK(out[0].bottom == doctest::Approx(0.0));
    CHECK(out[0].top == doctest::Approx(0.25));
    w.query(12.5, window(4.0), out); // sounding: its start is below the line
    REQUIRE(out.size() == 1);
    CHECK(out[0].bottom == doctest::Approx(-0.125));
    CHECK(out[0].top == doctest::Approx(0.125));
}

TEST_CASE("note window: a note is shown from the top edge until it has left the bottom") {
    NoteWindow w(song_of({make_note(10.0, 1.0)}));
    std::vector<VisibleNote> out;
    w.query(5.9, window(4.0, 0.5), out); // sees 5.4..9.9: not yet
    CHECK(out.empty());
    w.query(6.0, window(4.0, 0.5), out); // sees up to 10.0, exclusive
    CHECK(out.empty());
    w.query(6.1, window(4.0, 0.5), out);
    CHECK(out.size() == 1);
    w.query(11.4, window(4.0, 0.5), out); // ends at 11.0, window starts at 10.9
    CHECK(out.size() == 1);
    w.query(11.5, window(4.0, 0.5), out); // window starts at 11.0: ended exactly there
    CHECK(out.empty());
    w.query(11.6, window(4.0, 0.5), out);
    CHECK(out.empty());
}

TEST_CASE("note window: the behind span shows notes just past the line, with y below 0") {
    NoteWindow w(song_of({make_note(1.0, 0.5)}));
    std::vector<VisibleNote> out;
    w.query(1.6, window(2.0, 0.25), out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].top == doctest::Approx(0.0 - 0.1 / 2.0).epsilon(1e-9));
    CHECK(out[0].top < 0.0);
    w.query(2.0, window(2.0, 0.25), out);
    CHECK(out.empty());
}

TEST_CASE("note window: a long note that began long ago is still found") {
    std::vector<Note> notes;
    notes.push_back(make_note(0.0, 1000.0)); // a drone note under the whole song
    for (int i = 0; i < 100; ++i) {
        notes.push_back(make_note(i * 1.0, 0.5));
    }
    NoteWindow w(song_of(notes));
    std::vector<VisibleNote> out;
    w.query(50.0, window(2.0), out);
    REQUIRE(!out.empty());
    CHECK(out[0].index == 0);
    CHECK(out[0].bottom < 0.0);
    CHECK(out[0].top > 1.0); // not clipped
}

TEST_CASE("note window: results come in order of start, with ties in song order") {
    NoteWindow w(song_of(
        {make_note(1.0, 1.0), make_note(1.0, 2.0), make_note(0.5, 3.0), make_note(2.0, 1.0)}));
    // Not sorted on purpose: the index sorts by start itself.
    std::vector<VisibleNote> out;
    w.query(1.0, window(3.0), out);
    CHECK(indices(out) == std::vector<std::uint32_t>{2, 0, 1, 3});
}

TEST_CASE("note window: hands and drums are filtered") {
    NoteWindow w(
        song_of({make_note(1.0, 1.0, Hand::Right), make_note(1.0, 1.0, Hand::Left),
                 make_note(1.0, 1.0, Hand::Unknown), make_note(1.0, 1.0, Hand::Unknown, true)}));
    std::vector<VisibleNote> out;
    NoteWindowOptions o = window(2.0);
    w.query(0.5, o, out);
    CHECK(indices(out) == std::vector<std::uint32_t>{0, 1, 2}); // drums are off by default
    o.hand = HandFilter::Right;
    w.query(0.5, o, out);
    CHECK(indices(out) == std::vector<std::uint32_t>{0});
    o.hand = HandFilter::Left;
    w.query(0.5, o, out);
    CHECK(indices(out) == std::vector<std::uint32_t>{1});
    o.hand = HandFilter::Both;
    o.include_drums = true;
    w.query(0.5, o, out);
    CHECK(indices(out) == std::vector<std::uint32_t>{0, 1, 2, 3});
    CHECK(NoteWindowOptions{}.include_drums == false);
}

TEST_CASE("note window: an empty song, an empty window and a bad span give nothing") {
    NoteWindow empty{Song{}};
    std::vector<VisibleNote> out{VisibleNote{}};
    empty.query(0.0, window(4.0), out);
    CHECK(out.empty());

    NoteWindow w(song_of({make_note(1.0, 1.0)}));
    out.push_back({});
    w.query(1.0, window(0.0), out);
    CHECK(out.empty());
    w.query(1.0, window(-1.0), out);
    CHECK(out.empty());
    w.query(100.0, window(4.0), out);
    CHECK(out.empty());
    w.query(-100.0, window(4.0), out);
    CHECK(out.empty());
}

TEST_CASE("note window: agrees with a scan of every note, for random songs and positions") {
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> start(0.0, 200.0);
    std::uniform_real_distribution<double> short_len(0.02, 1.5);
    std::uniform_real_distribution<double> long_len(5.0, 150.0);
    std::uniform_int_distribution<int> kind(0, 9);
    std::vector<Note> notes;
    for (int i = 0; i < 3000; ++i) {
        Hand h = static_cast<Hand>(kind(rng) % 3);
        notes.push_back(make_note(start(rng), kind(rng) == 0 ? long_len(rng) : short_len(rng), h,
                                  kind(rng) == 0));
    }
    NoteWindow w(song_of(notes));
    std::uniform_real_distribution<double> where(-5.0, 210.0);
    std::uniform_real_distribution<double> span(0.5, 8.0);
    std::vector<VisibleNote> out;
    for (int q = 0; q < 400; ++q) {
        NoteWindowOptions o = window(span(rng), span(rng) / 8);
        o.hand = static_cast<HandFilter>(q % 3);
        o.include_drums = q % 5 == 0;
        double pos = where(rng);
        w.query(pos, o, out);

        std::vector<std::uint32_t> expected;
        for (std::uint32_t i = 0; i < notes.size(); ++i) {
            const Note& n = notes[i];
            bool hand_ok = o.hand == HandFilter::Both ||
                           n.hand == (o.hand == HandFilter::Left ? Hand::Left : Hand::Right);
            bool seen = n.start_s < pos + o.ahead_s && n.start_s + n.duration_s > pos - o.behind_s;
            if (hand_ok && seen && (o.include_drums || !n.drum)) {
                expected.push_back(i);
            }
        }
        auto got = indices(out);
        std::sort(got.begin(), got.end());
        CAPTURE(q);
        CHECK(got == expected);
        for (const VisibleNote& v : out) {
            const Note& n = notes[v.index];
            CHECK(v.bottom == doctest::Approx((n.start_s - pos) / o.ahead_s));
            CHECK(v.top == doctest::Approx((n.start_s + n.duration_s - pos) / o.ahead_s));
        }
        CHECK(
            std::is_sorted(out.begin(), out.end(), [&](const VisibleNote& a, const VisibleNote& b) {
                return notes[a.index].start_s < notes[b.index].start_s;
            }));
    }
}

TEST_CASE("note window: a query costs about the notes on screen, not the notes in the song") {
    // 50,000 notes, ten per second, plus one note under the whole song and a few long ones, the
    // worst case for an index that only sorts by start.
    constexpr int kNotes = 50'000;
    std::vector<Note> notes;
    notes.push_back(make_note(0.0, 6000.0));
    for (int i = 0; i < kNotes; ++i) {
        notes.push_back(make_note(i * 0.1, i % 50 == 0 ? 30.0 : 0.08));
    }
    NoteWindow w(song_of(notes));
    REQUIRE(w.size() == kNotes + 1);

    std::vector<VisibleNote> out;
    std::size_t worst = 0;
    for (double pos : {0.0, 10.0, 1234.5, 2500.0, 4990.0, 6000.0}) {
        std::size_t examined = w.query(pos, window(4.0, 0.25), out);
        CAPTURE(pos);
        CHECK(examined < 4 * out.size() + 200);
        worst = std::max(worst, examined);
    }
    CHECK(worst < 600); // the song has 50,001 notes

    // The same answer a full scan gives at one position.
    std::vector<std::uint32_t> expected;
    const double pos = 2500.0;
    for (std::uint32_t i = 0; i < notes.size(); ++i) {
        if (notes[i].start_s < pos + 4.0 && notes[i].start_s + notes[i].duration_s > pos - 0.25) {
            expected.push_back(i);
        }
    }
    w.query(pos, window(4.0, 0.25), out);
    auto got = indices(out);
    std::sort(got.begin(), got.end());
    CHECK(got == expected);
}

TEST_CASE("note window: the index does not keep a reference to the song") {
    NoteWindow w;
    {
        Song s = song_of({make_note(1.0, 1.0)});
        w.build(s);
    }
    std::vector<VisibleNote> out;
    w.query(1.0, window(2.0), out);
    CHECK(out.size() == 1);
}

// ---- Transposition ------------------------------------------------------------------------------

namespace {

Note pitched(double start_s, int pitch, bool drum = false) {
    Note n = make_note(start_s, 0.5, Hand::Right, drum);
    n.pitch = static_cast<std::uint8_t>(pitch);
    return n;
}

} // namespace

TEST_CASE("note window: without a transposition the pitch is the song's") {
    NoteWindow w(song_of({pitched(1.0, 60), pitched(1.5, 10), pitched(2.0, 120)}));
    std::vector<VisibleNote> out;
    w.query(1.0, window(4.0), out);
    REQUIRE(out.size() == 3);
    for (const VisibleNote& n : out) {
        CHECK(n.playable);
        CHECK_FALSE(n.folded);
        CHECK(n.fold_octaves == 0);
    }
    CHECK(out[0].pitch == 60);
    CHECK(out[1].pitch == 10);
    CHECK(out[2].pitch == 120);
}

TEST_CASE("note window: shows the transposed pitch and marks the folded notes, keeping the index") {
    // Notes out of start order, so the index must stay the Song::notes index.
    NoteWindow w(song_of({pitched(3.0, 94), pitched(1.0, 60), pitched(2.0, 50)}));
    NoteWindowOptions o = window(8.0);
    o.transposition = Transposition(5, kRange61); // 94 -> 99 -> 87 folded; 60 -> 65; 50 -> 55
    std::vector<VisibleNote> out;
    w.query(0.0, o, out);
    REQUIRE(out.size() == 3);
    CHECK(out[0].index == 1);
    CHECK(out[0].pitch == 65);
    CHECK_FALSE(out[0].folded);
    CHECK(out[1].index == 2);
    CHECK(out[1].pitch == 55);
    CHECK(out[2].index == 0);
    CHECK(out[2].pitch == 87);
    CHECK(out[2].folded);
    CHECK(out[2].fold_octaves == -1);
    CHECK(out[2].playable);
}

TEST_CASE("note window: a note the keyboard can't play stays in the list, marked unplayable") {
    NoteWindow w(song_of({pitched(1.0, 66)}));
    NoteWindowOptions o = window(4.0);
    o.transposition = Transposition(0, KeyRange{60, 64});
    std::vector<VisibleNote> out;
    w.query(0.0, o, out);
    REQUIRE(out.size() == 1);
    CHECK_FALSE(out[0].playable);
    CHECK(out[0].pitch == -1);
}

TEST_CASE("note window: drum notes keep their instrument number whatever the transposition") {
    NoteWindow w(song_of({pitched(1.0, 38, true)}));
    NoteWindowOptions o = window(4.0);
    o.include_drums = true;
    o.transposition = Transposition(7, kRange61);
    std::vector<VisibleNote> out;
    w.query(0.0, o, out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].pitch == 38);
    CHECK_FALSE(out[0].folded);
}
