// SPDX-License-Identifier: GPL-3.0-or-later
// Builds Standard MIDI File bytes for tests, so every test file is readable next to its checks.
#pragma once

#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <vector>

namespace toonval::test {

using Bytes = std::vector<std::uint8_t>;

inline void put_vlq(Bytes& out, std::uint32_t value) {
    std::uint8_t buf[5];
    int n = 0;
    buf[n++] = static_cast<std::uint8_t>(value & 0x7F);
    while ((value >>= 7) != 0) {
        buf[n++] = static_cast<std::uint8_t>(0x80 | (value & 0x7F));
    }
    while (n > 0) {
        out.push_back(buf[--n]);
    }
}

inline void put_be(Bytes& out, std::uint32_t value, int bytes) {
    for (int i = bytes - 1; i >= 0; --i) {
        out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }
}

// Events of one MTrk chunk. Every call takes the delta time in ticks since the previous event.
class Track {
  public:
    // Any bytes after the delta, written as given: use it for running status.
    Track& raw(std::uint32_t delta, std::initializer_list<std::uint8_t> bytes) {
        put_vlq(data_, delta);
        data_.insert(data_.end(), bytes);
        return *this;
    }
    Track& on(std::uint32_t delta, int channel, int pitch, int velocity = 100) {
        return raw(delta, {u8(0x90 | channel), u8(pitch), u8(velocity)});
    }
    Track& off(std::uint32_t delta, int channel, int pitch, int velocity = 64) {
        return raw(delta, {u8(0x80 | channel), u8(pitch), u8(velocity)});
    }
    Track& cc(std::uint32_t delta, int channel, int controller, int value) {
        return raw(delta, {u8(0xB0 | channel), u8(controller), u8(value)});
    }
    Track& program(std::uint32_t delta, int channel, int program) {
        return raw(delta, {u8(0xC0 | channel), u8(program)});
    }
    Track& meta(std::uint32_t delta, int type, const Bytes& payload) {
        put_vlq(data_, delta);
        data_.push_back(0xFF);
        data_.push_back(u8(type));
        put_vlq(data_, static_cast<std::uint32_t>(payload.size()));
        data_.insert(data_.end(), payload.begin(), payload.end());
        return *this;
    }
    Track& tempo(std::uint32_t delta, std::uint32_t us_per_quarter) {
        Bytes p;
        put_be(p, us_per_quarter, 3);
        return meta(delta, 0x51, p);
    }
    Track& bpm(std::uint32_t delta, int beats_per_minute) {
        return tempo(delta, static_cast<std::uint32_t>(60'000'000 / beats_per_minute));
    }
    // Denominator as written in music (4 for x/4); stored as its power of two.
    Track& time_sig(std::uint32_t delta, int numerator, int denominator) {
        std::uint8_t power = 0;
        while ((1 << power) < denominator) {
            ++power;
        }
        return meta(delta, 0x58, {u8(numerator), power, 24, 8});
    }
    Track& name(std::uint32_t delta, std::string_view text) {
        return meta(delta, 0x03, Bytes(text.begin(), text.end()));
    }
    Track& instrument(std::uint32_t delta, std::string_view text) {
        return meta(delta, 0x04, Bytes(text.begin(), text.end()));
    }
    Track& channel_prefix(std::uint32_t delta, int channel) {
        return meta(delta, 0x20, {u8(channel)});
    }
    Track& end(std::uint32_t delta = 0) { return meta(delta, 0x2F, {}); }

    const Bytes& data() const { return data_; }

  private:
    static std::uint8_t u8(int v) { return static_cast<std::uint8_t>(v); }
    Bytes data_;
};

inline void put_chunk(Bytes& out, std::string_view id, const Bytes& body) {
    out.insert(out.end(), id.begin(), id.end());
    put_be(out, static_cast<std::uint32_t>(body.size()), 4);
    out.insert(out.end(), body.begin(), body.end());
}

inline Bytes header(int format, int tracks, int division) {
    Bytes out;
    Bytes body;
    put_be(body, static_cast<std::uint32_t>(format), 2);
    put_be(body, static_cast<std::uint32_t>(tracks), 2);
    put_be(body, static_cast<std::uint32_t>(division), 2);
    put_chunk(out, "MThd", body);
    return out;
}

// A whole file: format 0 for one track, format 1 otherwise, `ppq` ticks per quarter note.
inline Bytes smf(std::initializer_list<Track> tracks, int ppq = 480) {
    Bytes out = header(tracks.size() == 1 ? 0 : 1, static_cast<int>(tracks.size()), ppq);
    for (const Track& t : tracks) {
        put_chunk(out, "MTrk", t.data());
    }
    return out;
}

} // namespace toonval::test
