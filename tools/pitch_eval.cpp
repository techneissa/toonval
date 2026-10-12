// SPDX-License-Identifier: GPL-3.0-or-later
// pitch-eval: runs the pitch detector over the synthetic piano set, or over a folder of labelled
// WAV files, and prints wrong-note, octave-error, not-sure and missed rates and latency per
// register. Also measures CPU per block.
//
// Labelled WAV folder: every NAME.wav needs a NAME.txt next to it with one note per line,
// "onset_seconds offset_seconds midi_note" (the MAPS format; a non-numeric header line is
// skipped). Open detection is monophonic, so use isolated notes (e.g. MAPS ISOL/NO).
#include "toonval/pitch_eval.hpp"
#include "toonval/pitch.hpp"
#include "toonval/pitch_detect.hpp"
#include "toonval/pitch_synth.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace toonval;

namespace {

void usage() {
    std::puts("usage: pitch-eval [options]\n"
              "  --method yin|mpm|both   detector method (default both)\n"
              "  --velocities a,b,...    velocities 0..1 (default 0.25,0.5,0.75,1)\n"
              "  --variants N            random variants per note and velocity (default 2)\n"
              "  --seed S                base seed (default 1)\n"
              "  --step N                every Nth note from A0 (default 1)\n"
              "  --noise-db D            room noise level in dBFS RMS (default -60)\n"
              "  --interferer-db D       interfering tone level vs the note (default -20)\n"
              "  --no-interferer         no interfering tone\n"
              "  --confirm N             confirmation frames (default 3)\n"
              "  --min-confidence C      per-frame confidence needed to confirm (default 0.8)\n"
              "  --octave-periods F      periods before the octave-below check (default 4)\n"
              "  --block N               samples per process() call (default 256)\n"
              "  --expected              also evaluate expected-note mode\n"
              "  --melodies N            also evaluate N random 24-note melodies (legato, notes "
              "ringing)\n"
              "  --chords N              also evaluate N random triads in expected-chord mode\n"
              "  --wav DIR               evaluate labelled WAV files instead of the synthetic set\n"
              "  --failures              list every clip that was not correct, with its events\n"
              "  --bench                 measure CPU time per block (use a Release build)\n");
}

std::vector<float> parse_list(const char* text) {
    std::vector<float> out;
    std::stringstream ss(text);
    std::string item;
    while (std::getline(ss, item, ',')) {
        out.push_back(std::strtof(item.c_str(), nullptr));
    }
    return out;
}

void print_report(const char* title, const EvalReport& r) {
    std::printf("\n%s\n", title);
    std::printf("%-7s %6s %8s %8s %8s %8s %8s %9s %9s %9s\n", "reg", "notes", "correct", "wrong",
                "octave", "notsure", "missed", "lat p50", "lat p95", "lat max");
    auto row = [](const char* name, const RegisterStats& s) {
        std::printf("%-7s %6d %7.2f%% %7.2f%% %7.2f%% %7.2f%% %7.2f%% %7.1fms %7.1fms %7.1fms\n",
                    name, s.notes, 100.0f * rate(s.correct, s.notes),
                    100.0f * rate(s.wrong, s.notes), 100.0f * rate(s.octave, s.notes),
                    100.0f * rate(s.not_sure, s.notes), 100.0f * rate(s.missed, s.notes),
                    percentile(s.latency_ms, 50), percentile(s.latency_ms, 95),
                    percentile(s.latency_ms, 100));
    };
    for (int i = 0; i < kRegisterCount; ++i) {
        row(register_name(i), r.registers[static_cast<size_t>(i)]);
    }
    row("all", r.total);
    // Targets: wrong notes < 0.5 %, octave errors < 0.2 %, decisions within 40 ms from C4 up.
    std::vector<float> c4_up;
    for (int i = register_of(60); i < kRegisterCount; ++i) {
        const auto& l = r.registers[static_cast<size_t>(i)].latency_ms;
        c4_up.insert(c4_up.end(), l.begin(), l.end());
    }
    std::printf("targets: wrong %.2f%% (<0.5%%)  octave %.2f%% (<0.2%%)  C4+ max latency "
                "%.1f ms (<40 ms)\n",
                100.0f * rate(r.total.wrong, r.total.notes),
                100.0f * rate(r.total.octave, r.total.notes), percentile(c4_up, 100));
}

void print_verdicts(const char* name, const VerdictStats& s) {
    std::printf("%-9s %6d %7.2f%% %7.2f%% %7.2f%% %7.2f%% %7.1fms %7.1fms\n", name, s.queries,
                100.0f * rate(s.correct, s.queries), 100.0f * rate(s.wrong, s.queries),
                100.0f * rate(s.false_accept, s.queries), 100.0f * rate(s.not_sure, s.queries),
                percentile(s.latency_ms, 50), percentile(s.latency_ms, 95));
}

// --- WAV reading (tool only; core never touches files) ---

struct Wav {
    int rate = 0;
    std::vector<float> mono;
};

std::uint32_t le32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
           static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}
std::uint16_t le16(const unsigned char* p) { return static_cast<std::uint16_t>(p[0] | p[1] << 8); }

// PCM 16/24/32-bit integer or 32-bit float, any channel count (averaged to mono).
bool read_wav(const std::filesystem::path& path, Wav& wav, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    if (data.size() < 12 || std::memcmp(data.data(), "RIFF", 4) != 0 ||
        std::memcmp(data.data() + 8, "WAVE", 4) != 0) {
        error = "not a RIFF/WAVE file";
        return false;
    }
    int format = 0;
    int channels = 0;
    int bits = 0;
    size_t pos = 12;
    while (pos + 8 <= data.size()) {
        const std::uint32_t size = le32(data.data() + pos + 4);
        const unsigned char* body = data.data() + pos + 8;
        if (pos + 8 + size > data.size()) {
            break;
        }
        if (std::memcmp(data.data() + pos, "fmt ", 4) == 0 && size >= 16) {
            format = le16(body);
            channels = le16(body + 2);
            wav.rate = static_cast<int>(le32(body + 4));
            bits = le16(body + 14);
            if (format == 0xFFFE && size >= 26) {
                format = le16(body + 24); // WAVE_FORMAT_EXTENSIBLE sub-format
            }
        } else if (std::memcmp(data.data() + pos, "data", 4) == 0) {
            if (channels <= 0 || bits <= 0) {
                error = "data before fmt";
                return false;
            }
            const int bytes = bits / 8;
            const size_t frames = size / static_cast<size_t>(bytes * channels);
            wav.mono.assign(frames, 0.0f);
            for (size_t f = 0; f < frames; ++f) {
                float sum = 0.0f;
                for (int c = 0; c < channels; ++c) {
                    const unsigned char* s =
                        body + (f * static_cast<size_t>(channels) + static_cast<size_t>(c)) *
                                   static_cast<size_t>(bytes);
                    float v = 0.0f;
                    if (format == 3 && bits == 32) {
                        std::uint32_t u = le32(s);
                        std::memcpy(&v, &u, 4);
                    } else if (format == 1 && bits == 16) {
                        v = static_cast<float>(static_cast<std::int16_t>(le16(s))) / 32768.0f;
                    } else if (format == 1 && bits == 24) {
                        const std::int32_t x =
                            static_cast<std::int32_t>(static_cast<std::uint32_t>(s[0]) << 8 |
                                                      static_cast<std::uint32_t>(s[1]) << 16 |
                                                      static_cast<std::uint32_t>(s[2]) << 24);
                        v = static_cast<float>(x) / 2147483648.0f;
                    } else if (format == 1 && bits == 32) {
                        v = static_cast<float>(static_cast<std::int32_t>(le32(s))) / 2147483648.0f;
                    } else {
                        error = "unsupported sample format";
                        return false;
                    }
                    sum += v;
                }
                wav.mono[f] = sum / static_cast<float>(channels);
            }
            return true;
        }
        pos += 8 + size + (size & 1u);
    }
    error = "no data chunk";
    return false;
}

// Linear resampling to the detector's rate. Good enough for evaluation; the app captures at 48 kHz.
std::vector<float> resample(const std::vector<float>& in, int from, int to) {
    if (from == to || in.empty()) {
        return in;
    }
    const double ratio = static_cast<double>(from) / to;
    const auto n = static_cast<size_t>(static_cast<double>(in.size()) / ratio);
    std::vector<float> out(n);
    for (size_t i = 0; i < n; ++i) {
        const double x = static_cast<double>(i) * ratio;
        const auto k = static_cast<size_t>(x);
        const auto frac = static_cast<float>(x - static_cast<double>(k));
        const float a = in[std::min(k, in.size() - 1)];
        const float b = in[std::min(k + 1, in.size() - 1)];
        out[i] = a + (b - a) * frac;
    }
    return out;
}

struct Label {
    double onset = 0.0;
    double offset = 0.0;
    int note = 0;
};

std::vector<Label> read_labels(const std::filesystem::path& path) {
    std::vector<Label> labels;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        Label l;
        if (ss >> l.onset >> l.offset >> l.note) {
            labels.push_back(l);
        }
    }
    std::sort(labels.begin(), labels.end(),
              [](const Label& a, const Label& b) { return a.onset < b.onset; });
    return labels;
}

int evaluate_wav_folder(const std::string& dir, const PitchDetectorConfig& cfg, int block) {
    EvalReport report;
    int files = 0;
    std::vector<std::filesystem::path> paths;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".wav") {
            paths.push_back(entry.path());
        }
    }
    std::sort(paths.begin(), paths.end());
    for (const auto& path : paths) {
        auto label_path = path;
        label_path.replace_extension(".txt");
        if (!std::filesystem::exists(label_path)) {
            continue;
        }
        Wav wav;
        std::string error;
        if (!read_wav(path, wav, error)) {
            std::fprintf(stderr, "skip %s: %s\n", path.string().c_str(), error.c_str());
            continue;
        }
        const auto labels = read_labels(label_path);
        const auto audio = resample(wav.mono, wav.rate, cfg.sample_rate);
        PitchDetector detector(cfg);
        const auto events =
            run_detector(detector, audio.data(), static_cast<int>(audio.size()), block);
        for (size_t i = 0; i < labels.size(); ++i) {
            const auto onset = static_cast<std::int64_t>(labels[i].onset * cfg.sample_rate);
            // Up to 20 ms before the next labelled onset (which may be detected a little early).
            const auto end =
                i + 1 < labels.size()
                    ? static_cast<std::int64_t>(labels[i + 1].onset * cfg.sample_rate) -
                          cfg.sample_rate / 50
                    : static_cast<std::int64_t>(audio.size());
            report.add(labels[i].note,
                       score_note(labels[i].note, onset, end, events.data(),
                                  static_cast<int>(events.size()), cfg.sample_rate),
                       cfg.sample_rate);
        }
        ++files;
    }
    std::printf("%d labelled WAV files from %s\n", files, dir.c_str());
    print_report(cfg.method == PitchMethod::Yin ? "YIN" : "MPM", report);
    return files > 0 ? 0 : 1;
}

// --- CPU benchmark ---

void bench(PitchDetectorConfig cfg, int block) {
    const int rate_hz = cfg.sample_rate;
    // A scale over the keyboard with the interferer and noise, about 20 s.
    SynthClipOptions clip_options;
    clip_options.note_s = 0.25f;
    clip_options.lead_in_s = 0.0f;
    std::vector<float> audio;
    for (int note = 21; note <= 108; ++note) {
        const auto clip =
            make_synth_clip(note, 0.75f, static_cast<std::uint64_t>(note), clip_options, rate_hz);
        audio.insert(audio.end(), clip.samples.begin(), clip.samples.end());
    }
    // Then 2 s of loud noise: the gate stays open and acquisition never confirms, so every hop
    // runs the largest analysis. This is the worst case.
    std::vector<float> noise(static_cast<size_t>(2 * rate_hz), 0.0f);
    add_room_noise(noise.data(), static_cast<int>(noise.size()), 0.05f, 3);
    audio.insert(audio.end(), noise.begin(), noise.end());

    for (auto method : {PitchMethod::Mpm, PitchMethod::Yin}) {
        cfg.method = method;
        PitchDetector detector(cfg);
        PitchEvent events[16];
        std::vector<double> us;
        us.reserve(audio.size() / static_cast<size_t>(block) + 1);
        for (size_t i = 0; i + static_cast<size_t>(block) <= audio.size();
             i += static_cast<size_t>(block)) {
            const auto t0 = std::chrono::steady_clock::now();
            detector.process(audio.data() + i, block, events, 16);
            const auto t1 = std::chrono::steady_clock::now();
            us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
        }
        std::vector<double> sorted = us;
        std::sort(sorted.begin(), sorted.end());
        double sum = 0.0;
        for (double v : us) {
            sum += v;
        }
        const double block_us = 1e6 * block / rate_hz;
        const double mean = sum / static_cast<double>(us.size());
        std::printf("%s: %zu blocks of %d: mean %.1f us (%.1f%% of real time), p99 %.1f us, max "
                    "%.1f us (block lasts %.0f us)\n",
                    method == PitchMethod::Yin ? "YIN" : "MPM", us.size(), block, mean,
                    100.0 * mean / block_us, sorted[sorted.size() * 99 / 100], sorted.back(),
                    block_us);
    }
}

} // namespace

int main(int argc, char** argv) {
    SynthEvalOptions options;
    options.variants = 2;
    bool yin = true;
    bool mpm = true;
    bool expected = false;
    int chords = 0;
    int melodies = 0;
    bool do_bench = false;
    bool failures = false;
    std::string wav_dir;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&]() -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", arg.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--method") {
            const std::string m = value();
            yin = m == "yin" || m == "both";
            mpm = m == "mpm" || m == "both";
        } else if (arg == "--velocities") {
            options.velocities = parse_list(value());
        } else if (arg == "--variants") {
            options.variants = std::atoi(value());
        } else if (arg == "--seed") {
            options.seed = std::strtoull(value(), nullptr, 10);
        } else if (arg == "--step") {
            options.note_step = std::max(1, std::atoi(value()));
        } else if (arg == "--noise-db") {
            options.clip.noise_rms = std::pow(10.0f, std::strtof(value(), nullptr) / 20.0f);
        } else if (arg == "--interferer-db") {
            options.clip.interferer_db = std::strtof(value(), nullptr);
        } else if (arg == "--no-interferer") {
            options.clip.interferer = false;
        } else if (arg == "--confirm") {
            options.detector.confirm_frames = std::atoi(value());
        } else if (arg == "--min-confidence") {
            options.detector.min_confidence = std::strtof(value(), nullptr);
        } else if (arg == "--octave-periods") {
            options.detector.octave_check_periods = std::strtof(value(), nullptr);
        } else if (arg == "--block") {
            options.block = std::max(1, std::atoi(value()));
        } else if (arg == "--expected") {
            expected = true;
        } else if (arg == "--melodies") {
            melodies = std::atoi(value());
        } else if (arg == "--chords") {
            chords = std::atoi(value());
        } else if (arg == "--wav") {
            wav_dir = value();
        } else if (arg == "--failures") {
            failures = true;
        } else if (arg == "--bench") {
            do_bench = true;
        } else {
            usage();
            return arg == "--help" || arg == "-h" ? 0 : 2;
        }
    }

    if (do_bench) {
        bench(options.detector, options.block);
        return 0;
    }
    if (!wav_dir.empty()) {
        int status = 0;
        for (auto method : {PitchMethod::Mpm, PitchMethod::Yin}) {
            if ((method == PitchMethod::Mpm && mpm) || (method == PitchMethod::Yin && yin)) {
                options.detector.method = method;
                status |= evaluate_wav_folder(wav_dir, options.detector, options.block);
            }
        }
        return status;
    }

    std::printf("synthetic set: notes %d-%d step %d, %zu velocities x %d variants, noise %.0f "
                "dBFS, interferer %s\n",
                options.low_note, options.high_note, options.note_step, options.velocities.size(),
                options.variants, 20.0f * std::log10(options.clip.noise_rms),
                options.clip.interferer ? "on" : "off");
    for (auto method : {PitchMethod::Mpm, PitchMethod::Yin}) {
        if ((method == PitchMethod::Mpm && !mpm) || (method == PitchMethod::Yin && !yin)) {
            continue;
        }
        options.detector.method = method;
        std::vector<NoteRecord> records;
        options.records = failures ? &records : nullptr;
        print_report(method == PitchMethod::Yin ? "YIN, open detection" : "MPM, open detection",
                     evaluate_synthetic(options));
        options.records = nullptr;
        static constexpr const char* kOutcome[] = {"correct", "WRONG", "OCTAVE", "notsure",
                                                   "missed"};
        for (const auto& r : records) {
            // Correct notes are listed only when middle C and up took over 40 ms.
            const bool slow = r.score.outcome == NoteOutcome::Correct && r.note >= kMiddleC &&
                              r.score.latency_samples > 40 * 48;
            if (r.score.outcome == NoteOutcome::Correct && !slow) {
                continue;
            }
            std::printf("  %-4s v%.2f #%d %-8s interferer %-4s:", note_name(r.note).c_str(),
                        static_cast<double>(r.velocity), r.variant,
                        slow ? "SLOW" : kOutcome[static_cast<int>(r.score.outcome)],
                        note_name(r.interferer_note).c_str());
            for (const auto& e : r.events) {
                static constexpr const char* kType[] = {"on", "off", "unsure", "verdict"};
                std::printf(" %s:%s@%+.1f/%+.1fms", kType[static_cast<int>(e.type)],
                            e.note >= 0 ? note_name(e.note).c_str() : "-",
                            1000.0 * static_cast<double>(e.onset_sample - r.onset) / 48000.0,
                            1000.0 * static_cast<double>(e.sample - r.onset) / 48000.0);
            }
            std::printf("\n");
        }
    }
    for (auto method : {PitchMethod::Mpm, PitchMethod::Yin}) {
        if (melodies <= 0 || (method == PitchMethod::Mpm && !mpm) ||
            (method == PitchMethod::Yin && !yin)) {
            continue;
        }
        options.detector.method = method;
        std::vector<NoteRecord> records;
        options.records = failures ? &records : nullptr;
        print_report(method == PitchMethod::Yin ? "YIN, melodies" : "MPM, melodies",
                     evaluate_melody_synthetic(options, melodies, 24));
        options.records = nullptr;
        for (const auto& r : records) {
            std::printf("  melody %d at %.3f s: %-4s %-8s v%.2f:", r.variant,
                        static_cast<double>(r.onset) / 48000.0, note_name(r.note).c_str(),
                        r.score.outcome == NoteOutcome::Correct ? "ok" : "FAIL",
                        static_cast<double>(r.velocity));
            for (const auto& e : r.events) {
                static constexpr const char* kType[] = {"on", "off", "unsure", "verdict"};
                std::printf(" %s:%s@%+.1f/%+.1fms", kType[static_cast<int>(e.type)],
                            e.note >= 0 ? note_name(e.note).c_str() : "-",
                            1000.0 * static_cast<double>(e.onset_sample - r.onset) / 48000.0,
                            1000.0 * static_cast<double>(e.sample - r.onset) / 48000.0);
            }
            std::printf("\n");
        }
    }
    if (expected) {
        const auto r = evaluate_expected_synthetic(options);
        std::printf("\nexpected-note mode\n%-9s %6s %8s %8s %8s %8s %9s %9s\n", "asked", "queries",
                    "correct", "wrong", "falseacc", "notsure", "lat p50", "lat p95");
        print_verdicts("played", r.expected);
        print_verdicts("neighbor", r.neighbor);
        print_verdicts("octave", r.octave);
        for (int i = 0; i < kRegisterCount; ++i) {
            print_verdicts(register_name(i), r.registers[static_cast<size_t>(i)]);
        }
    }
    if (chords > 0) {
        const auto r = evaluate_chord_synthetic(options, chords);
        std::printf("\nexpected-chord mode: %d triads, %d notes asked\n", chords, r.notes);
        std::printf("correct %.2f%%  wrong %.2f%%  not sure %.2f%%  left-out notes caught %d/%d, "
                    "reported present %d  latency p50 %.1f ms p95 %.1f ms\n",
                    100.0f * rate(r.correct, r.notes), 100.0f * rate(r.wrong, r.notes),
                    100.0f * rate(r.not_sure, r.notes), r.missing_caught, r.missing_notes,
                    r.false_present, percentile(r.latency_ms, 50), percentile(r.latency_ms, 95));
    }
    return 0;
}
