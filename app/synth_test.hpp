// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace toonval::app {

struct AudioOptions;

// --synth-test: plays a short scale, a pedalled arpeggio and a loud chord on the built-in piano,
// with no window, so a person can hear it. Prints the device it got and, with
// --audio-latency-log, the press-to-sound latency. Returns the process exit code.
int run_synth_test(const AudioOptions& options);

} // namespace toonval::app
