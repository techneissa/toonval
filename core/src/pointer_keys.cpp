// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/pointer_keys.hpp"

#include "toonval/pitch.hpp"

#include <algorithm>
#include <cmath>

namespace toonval {
namespace {

int velocity_at(float t) {
    t = std::clamp(std::isfinite(t) ? t : 0.0f, 0.0f, 1.0f);
    const float v = static_cast<float>(kPointerMinVelocity) +
                    static_cast<float>(kPointerMaxVelocity - kPointerMinVelocity) * t;
    return std::clamp(static_cast<int>(std::lround(v)), kPointerMinVelocity, kPointerMaxVelocity);
}

} // namespace

std::optional<KeyHit> hit_test_key(const PointerGeometry& g, float x, float y) {
    const RectF& area = g.key_area;
    if (area.empty() || !std::isfinite(x) || !std::isfinite(y) || x < area.x || y < area.y ||
        x >= area.right() || y >= area.bottom()) {
        return std::nullopt;
    }
    const float u = (x - area.x) / area.w; // 0..1 across the window
    const int whites = g.layout.white_key_count();
    const float black_length = area.h * kBlackKeyLengthShare;
    const KeyRange& range = g.layout.range();
    const auto covers = [&](int note) {
        const KeyRect r = g.view.to_window(g.layout.key(note), whites);
        return r.width > 0.0f && u >= r.x && u < r.x + r.width;
    };
    if (y < area.y + black_length) {
        for (int note = range.lowest; note <= range.highest; ++note) {
            if (is_black_key(note) && covers(note)) {
                return KeyHit{note,
                              static_cast<std::uint8_t>(velocity_at((y - area.y) / black_length))};
            }
        }
    }
    for (int note = range.lowest; note <= range.highest; ++note) {
        if (!is_black_key(note) && covers(note)) {
            return KeyHit{note, static_cast<std::uint8_t>(velocity_at((y - area.y) / area.h))};
        }
    }
    return std::nullopt;
}

PointerKeys::Pointer* PointerKeys::find(PointerId id) {
    for (Pointer& p : pointers_) {
        if (p.used && p.id == id) {
            return &p;
        }
    }
    return nullptr;
}

const PointerKeys::Pointer* PointerKeys::find(PointerId id) const {
    return const_cast<PointerKeys*>(this)->find(id);
}

void PointerKeys::press(int note, int velocity, std::int64_t time_ns, std::vector<MidiEvent>& out) {
    if (shared_[static_cast<std::size_t>(note)]++ != 0) {
        return;
    }
    MidiEvent e;
    e.time_ns = time_ns;
    e.type = MidiEventType::NoteOn;
    e.data1 = static_cast<std::uint8_t>(note);
    e.data2 = static_cast<std::uint8_t>(velocity);
    e.source = InputSource::Pointer;
    out.push_back(e);
}

void PointerKeys::release(int note, std::int64_t time_ns, std::vector<MidiEvent>& out) {
    if (note < 0 || --shared_[static_cast<std::size_t>(note)] != 0) {
        return;
    }
    MidiEvent e;
    e.time_ns = time_ns;
    e.type = MidiEventType::NoteOff;
    e.data1 = static_cast<std::uint8_t>(note);
    e.source = InputSource::Pointer;
    out.push_back(e);
}

void PointerKeys::down(PointerId id, float x, float y, const PointerGeometry& g,
                       std::int64_t time_ns, std::vector<MidiEvent>& out) {
    if (find(id) != nullptr) {
        move(id, x, y, g, time_ns, out); // a second down for a pointer already down
        return;
    }
    const std::optional<KeyHit> hit = hit_test_key(g, x, y);
    if (!hit) {
        return;
    }
    for (Pointer& p : pointers_) {
        if (!p.used) {
            p = {true, id, hit->note};
            press(hit->note, hit->velocity, time_ns, out);
            return;
        }
    }
    // More pointers than slots: the extra one plays nothing.
}

void PointerKeys::move(PointerId id, float x, float y, const PointerGeometry& g,
                       std::int64_t time_ns, std::vector<MidiEvent>& out) {
    Pointer* p = find(id);
    if (p == nullptr) {
        return;
    }
    const std::optional<KeyHit> hit = hit_test_key(g, x, y);
    const int now = hit ? hit->note : -1;
    if (now == p->note) {
        return;
    }
    const int old = p->note;
    p->note = now;
    release(old, time_ns, out);
    if (hit) {
        press(hit->note, hit->velocity, time_ns, out);
    }
}

void PointerKeys::up(PointerId id, std::int64_t time_ns, std::vector<MidiEvent>& out) {
    Pointer* p = find(id);
    if (p == nullptr) {
        return;
    }
    const int note = p->note;
    *p = {};
    release(note, time_ns, out);
}

void PointerKeys::release_all(std::int64_t time_ns, std::vector<MidiEvent>& out) {
    for (Pointer& p : pointers_) {
        if (p.used) {
            const int note = p.note;
            p = {};
            release(note, time_ns, out);
        }
    }
}

std::optional<int> PointerKeys::note_of(PointerId id) const {
    const Pointer* p = find(id);
    if (p == nullptr || p->note < 0) {
        return std::nullopt;
    }
    return p->note;
}

int PointerKeys::active_pointers() const {
    int n = 0;
    for (const Pointer& p : pointers_) {
        n += p.used ? 1 : 0;
    }
    return n;
}

int PointerKeys::held_notes() const {
    int n = 0;
    for (const std::uint8_t c : shared_) {
        n += c > 0 ? 1 : 0;
    }
    return n;
}

} // namespace toonval
