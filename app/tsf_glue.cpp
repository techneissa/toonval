// SPDX-License-Identifier: GPL-3.0-or-later
// TinySoundFont's implementation, compiled once here without the project's warning flags.
#define TSF_IMPLEMENTATION
#include "tsf_glue.hpp"

namespace toonval::app {
namespace {

// The voice that the steal order picks, or null when no voice is playing.
tsf_voice* pick_victim(tsf* f) {
    tsf_voice* best[3] = {nullptr, nullptr, nullptr}; // releasing, pedal-held, held
    for (tsf_voice* v = f->voices; v != f->voices + f->voiceNum; ++v) {
        if (v->playingPreset == -1) {
            continue;
        }
        const int rank = v->ampenv.segment >= TSF_SEGMENT_RELEASE ? 0 : (v->heldSustain ? 1 : 2);
        if (best[rank] == nullptr || v->playIndex < best[rank]->playIndex) {
            best[rank] = v;
        }
    }
    for (tsf_voice* v : best) {
        if (v != nullptr) {
            return v;
        }
    }
    return nullptr;
}

} // namespace

int tsf_glue_make_room(tsf* f, int preset_index, int key, float velocity) {
    if (preset_index < 0 || preset_index >= f->presetNum) {
        return 0;
    }
    const int midi_velocity = static_cast<int>(velocity * 127);
    const tsf_preset& preset = f->presets[preset_index];
    int needed = 0;
    for (int i = 0; i < preset.regionNum; ++i) {
        const tsf_region& r = preset.regions[i];
        if (key >= r.lokey && key <= r.hikey && midi_velocity >= r.lovel &&
            midi_velocity <= r.hivel) {
            ++needed;
        }
    }
    int free = 0;
    for (tsf_voice* v = f->voices; v != f->voices + f->voiceNum; ++v) {
        free += v->playingPreset == -1 ? 1 : 0;
    }
    int stolen = 0;
    while (free < needed) {
        tsf_voice* victim = pick_victim(f);
        if (victim == nullptr) {
            break;
        }
        // A note can use several voices (a stereo pair): they share a play index, end them all.
        const unsigned int play_index = victim->playIndex;
        for (tsf_voice* v = f->voices; v != f->voices + f->voiceNum; ++v) {
            if (v->playingPreset != -1 && v->playIndex == play_index) {
                v->playingPreset = -1;
                ++free;
                ++stolen;
            }
        }
    }
    return stolen;
}

int tsf_glue_voices_for_key(const tsf* f, int key) {
    int count = 0;
    for (const tsf_voice* v = f->voices; v != f->voices + f->voiceNum; ++v) {
        count += v->playingPreset != -1 && v->playingKey == key ? 1 : 0;
    }
    return count;
}

} // namespace toonval::app
