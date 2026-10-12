// SPDX-License-Identifier: GPL-3.0-or-later
// Opens a virtual MIDI output port named "midi-demo" and plays a short test pattern into it, so the
// app can be checked without a piano. Run the app first; it picks the port up through hot-plug.
//
//   midi-demo [settle-seconds]          the test pattern
//   midi-demo --blast [count] [settle]  floods the app's input queue to check overflow handling
#include <libremidi/libremidi.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace {

constexpr int kNoteOn = 0x90;
constexpr int kNoteOff = 0x80;
constexpr int kControlChange = 0xB0;
constexpr int kSustain = 64;

void wait_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

class Player {
  public:
    explicit Player(libremidi::midi_out& out) : out_(out) {}

    void send(int status, int data1, int data2, const char* what) {
        std::printf("  %-34s %02X %3d %3d\n", what, status, data1, data2);
        out_.send_message(static_cast<unsigned char>(status), static_cast<unsigned char>(data1),
                          static_cast<unsigned char>(data2));
    }

  private:
    libremidi::midi_out& out_;
};

// Sends a burst far larger than the app's per-port queue in one CoreMIDI packet, so the app's MIDI
// thread pushes it all before the frame loop can drain anything. Holds C4, then plays `count`
// presses and velocity-0 releases of D4 with running status, then releases C4. The release of C4
// is dropped by the full queue, so without the resync C4 would stay lit in the app.
//
// libremidi sends more than 3 bytes only as SysEx, so the burst starts with F0: the note-on
// status right after it ends the SysEx, and the rest is plain channel data. A debugging aid only.
int blast(libremidi::midi_out& out, int count) {
    std::vector<unsigned char> burst{0xF0, kNoteOn, 60, 100};
    for (int i = 0; i < count; ++i) {
        burst.insert(burst.end(), {62, 90, 62, 0});
    }
    burst.insert(burst.end(), {kNoteOff, 60, 0});
    std::printf("Blast: C4 down, %d D4 presses and releases, C4 up, %zu bytes in one packet.\n",
                count, burst.size());
    if (burst.size() > 65535) {
        std::fprintf(stderr, "count too large for one packet (max about 16000)\n");
        return 1;
    }
    if (out.send_message(burst.data(), burst.size()) != stdx::error{}) {
        std::fprintf(stderr, "send failed\n");
        return 1;
    }
    wait_ms(1000);
    std::printf("Done. The app should show every key up, a dropped count, and one All Notes Off "
                "marked synthetic.\n");
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    bool blast_mode = false;
    int count = 6000;
    int arg = 1;
    if (argc > 1 && std::strcmp(argv[1], "--blast") == 0) {
        blast_mode = true;
        arg = 2;
        if (argc > 2) {
            count = std::atoi(argv[2]);
            arg = 3;
        }
    }
    // Seconds to wait after opening the port, so the app has time to notice it.
    int settle_s = 2;
    if (argc > arg) {
        settle_s = std::atoi(argv[arg]);
    }

    libremidi::midi_out out;
    if (out.open_virtual_port("midi-demo") != stdx::error{}) {
        std::fprintf(stderr, "could not open a virtual MIDI output port\n");
        return 1;
    }
    std::printf("Virtual port \"midi-demo\" open. Playing in %d s.\n", settle_s);
    wait_ms(settle_s * 1000);
    if (blast_mode) {
        return blast(out, count);
    }

    Player p(out);
    std::printf("Notes C4 D4 E4, each with a real note-off:\n");
    for (int note : {60, 62, 64}) {
        p.send(kNoteOn, note, 96, "note-on");
        wait_ms(300);
        p.send(kNoteOff, note, 64, "note-off");
        wait_ms(150);
    }

    std::printf("C major chord, released with velocity-0 note-ons:\n");
    for (int note : {48, 52, 55}) {
        p.send(kNoteOn, note, 90, "note-on");
    }
    wait_ms(700);
    for (int note : {48, 52, 55}) {
        p.send(kNoteOn, note, 0, "note-on velocity 0 (note-off)");
    }
    wait_ms(300);

    std::printf("Sustain pedal held over G4 (the key still goes up):\n");
    p.send(kControlChange, kSustain, 127, "sustain down");
    p.send(kNoteOn, 67, 80, "note-on");
    wait_ms(400);
    p.send(kNoteOff, 67, 0, "note-off");
    wait_ms(400);
    p.send(kControlChange, kSustain, 0, "sustain up");
    wait_ms(300);

    std::printf("C0, below every piano, to show out-of-range handling:\n");
    p.send(kNoteOn, 12, 80, "note-on C0");
    wait_ms(300);
    p.send(kNoteOff, 12, 0, "note-off C0");
    wait_ms(300);

    std::printf("Held D5 left down; closing the port releases it in the app.\n");
    p.send(kNoteOn, 74, 80, "note-on (no note-off)");
    wait_ms(1000);
    std::printf("Done.\n");
    return 0;
}
