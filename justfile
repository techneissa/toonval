set shell := ["bash", "-euo", "pipefail", "-c"]

build_dir := "build"

# List recipes
default:
    @just --list

# Configure (first run fetches dependencies) and build everything
build:
    @scripts/ui-run.sh cmake -S . -B {{build_dir}} -G Ninja -DCMAKE_BUILD_TYPE=Debug
    @scripts/ui-run.sh cmake --build {{build_dir}}

# Build and launch the app
run: build
    {{build_dir}}/app/toonval

# Build and run the unit tests
test: build
    @scripts/ui-run.sh {{build_dir}}/tests/toonval_tests

# Launch the app and print note-on to frame latency every 5 s
latency-log: build
    {{build_dir}}/app/toonval --latency-log

# Play a scale, a pedalled arpeggio and a chord on the built-in piano, no window (args pass through)
synth-test *args: build
    {{build_dir}}/app/toonval --synth-test {{args}}

# Extract the piano presets from FluidR3_GM into build/toonval-piano.sf2 (downloads 148 MB once)
piano-sf2 *args:
    python3 -I tools/extract_piano_sf2.py --out {{build_dir}}/toonval-piano.sf2 {{args}}

# Release build, then frame times of all six note styles at 1080p with 200 notes (args pass through)
bench *args:
    @scripts/ui-run.sh cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
    @scripts/ui-run.sh cmake --build build-release --target toonval
    build-release/app/toonval --bench-all {{args}}

# Release build, then the pitch detector over the synthetic piano set (args pass through: --help)
pitch-eval *args:
    @scripts/ui-run.sh cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
    @scripts/ui-run.sh cmake --build build-release --target pitch-eval
    build-release/tools/pitch-eval {{args}}

# List connected MIDI ports and matching keyboard profiles
midi-ports: build
    {{build_dir}}/tools/midi-ports

# Play a test pattern into a virtual MIDI port (run the app first)
midi-demo *args: build
    {{build_dir}}/tools/midi-demo {{args}}

# Print live MIDI events and key changes without a window
midi-log: build
    {{build_dir}}/app/toonval --midi-log

# Display, renderer and touch spike; args pass through (just display-spike --list)
display-spike *args: build
    {{build_dir}}/tools/display-spike {{args}}

# Raspberry Pi OS only: install packages, build in Release, install binaries and systemd units
pi-setup *args:
    @[ "$(uname -s)" = Linux ] || { echo "pi-setup only runs on Linux (Raspberry Pi OS); this is $(uname -s). Run it on the Pi." >&2; exit 1; }
    sudo deploy/pi/setup.sh {{args}}

# Format all C++ sources with clang-format
format:
    @find core app tools tests -name '*.cpp' -o -name '*.hpp' | xargs clang-format -i

# Delete the build directory
clean:
    rm -rf {{build_dir}}
