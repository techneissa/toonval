// SPDX-License-Identifier: GPL-3.0-or-later
// Deterministic piano-like test tones for the pitch detector's evaluation: stretched (inharmonic)
// partials with per-partial decay, two detuned strings beating, a hammer-strike spectral notch,
// attack noise, room noise and an interfering tone. Same options and seed, same samples, on every
// platform (own random generator, no std distributions).
#pragma once

#include <cstdint>
#include <vector>

namespace toonval {

struct PianoToneOptions {
    int note = 60;
    float velocity = 0.7f;     // 0..1: loudness and brightness
    float detune_cents = 0.0f; // whole-note offset from equal temperament
    float a4_hz = 440.0f;
    float inharmonicity_scale = 1.0f; // times the typical B for the note
    float hammer_noise = 1.0f;        // attack noise level, relative
    std::uint64_t seed = 1;
};

// Peak amplitude of a tone at this velocity: about -6 dBFS at 1, -36 dBFS at 0.
float piano_tone_amplitude(float velocity);

// Typical inharmonicity coefficient B the synthesiser uses for a note (independent of the
// detector's own table, so the detector never sees exactly its own model).
float synth_inharmonicity(int note);

// Adds a tone starting at out[start] and running to out[length - 1].
void add_piano_tone(float* out, int length, int start, const PianoToneOptions& options,
                    int sample_rate);

// Adds pinkish room noise of the given RMS.
void add_room_noise(float* out, int length, float rms, std::uint64_t seed);

struct SynthClipOptions {
    float lead_in_s = 0.15f;      // noise (and interferer) before the onset
    float note_s = 0.35f;         // audio after the onset
    float noise_rms = 1e-3f;      // room noise, about -60 dBFS
    bool interferer = true;       // a steady second tone, another note, from the clip's start
    float interferer_db = -20.0f; // its RMS against the note's RMS over its first 100 ms
    float max_detune_cents = 5.0f;
    float inharmonicity_spread = 0.4f; // B is scaled by 1 +/- this, at random
};

struct SynthClip {
    std::vector<float> samples;
    std::int64_t onset = 0;
    int note = 0;
    int interferer_note = -1;
    float detune_cents = 0.0f;
};

// One labelled clip. `seed` picks the detune, B, phases, noise and the interfering note.
SynthClip make_synth_clip(int note, float velocity, std::uint64_t seed,
                          const SynthClipOptions& options, int sample_rate);

// A chord clip: all `notes` struck together at the onset (no interferer).
SynthClip make_synth_chord_clip(const int* notes, int count, float velocity, std::uint64_t seed,
                                const SynthClipOptions& options, int sample_rate);

} // namespace toonval
