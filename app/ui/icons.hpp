// SPDX-License-Identifier: GPL-3.0-or-later
// Lucide icon codepoints (lucide-font 1.55.0, codepoints.json) as UTF-8, for text drawn with a
// font the icon font is merged into.
#pragma once

namespace toonval::app::ui::icon {

// Not a font icon: glyph() draws a C marker (a bold C over a short line) for this pointer.
inline constexpr const char* kCMarker = "\x01";

inline constexpr const char* kSettings = "\xEE\x85\x94";    // settings
inline constexpr const char* kNames = "\xEE\x8F\x99";       // case-sensitive
inline constexpr const char* kStyle = "\xEE\x90\x92";       // sparkles
inline constexpr const char* kPalette = "\xEE\x87\x9D";     // palette
inline constexpr const char* kPresets = "\xEE\x81\xA0";     // bookmark
inline constexpr const char* kEffects = "\xEE\x8D\x97";     // wand-sparkles
inline constexpr const char* kView = "\xEE\x84\x9D";        // monitor
inline constexpr const char* kPractice = "\xEE\x86\x80";    // target
inline constexpr const char* kBack = "\xEE\x81\xAE";        // chevron-left
inline constexpr const char* kChevron = "\xEE\x81\xAF";     // chevron-right
inline constexpr const char* kClose = "\xEE\x86\xB2";       // x
inline constexpr const char* kCheck = "\xEE\x81\xAC";       // check
inline constexpr const char* kInfo = "\xEE\x83\xB9";        // info
inline constexpr const char* kReset = "\xEE\x85\x88";       // rotate-ccw
inline constexpr const char* kSave = "\xEE\x85\x8D";        // save
inline constexpr const char* kRename = "\xEE\x87\xB9";      // pencil
inline constexpr const char* kClear = "\xEE\x86\x8E";       // trash-2
inline constexpr const char* kLoad = "\xEE\x82\xB2";        // download
inline constexpr const char* kContrast = "\xEE\x82\x9D";    // contrast
inline constexpr const char* kFitKeys = "\xEE\x87\x86";     // move-horizontal
inline constexpr const char* kFitSong = "\xEE\x84\xA2";     // music
inline constexpr const char* kKeyboard = "\xEE\x95\xA0";    // keyboard-music
inline constexpr const char* kPiano = "\xEE\x95\xA1";       // piano
inline constexpr const char* kHand = "\xEE\x87\x97";        // hand
inline constexpr const char* kTimer = "\xEE\x87\xA0";       // timer
inline constexpr const char* kGauge = "\xEE\x86\xBF";       // gauge
inline constexpr const char* kHome = "\xEE\x83\xB5";        // house
inline constexpr const char* kGrid = "\xEE\x83\xBF";        // layout-grid
inline constexpr const char* kFlash = "\xEE\x86\xB4";       // zap
inline constexpr const char* kGlow = "\xEE\x85\xB8";        // sun
inline constexpr const char* kSparks = "\xEE\x91\xBE";      // sparkle
inline constexpr const char* kTrail = "\xEE\x8A\x83";       // waves
inline constexpr const char* kBackground = "\xEE\x83\xB6";  // image
inline constexpr const char* kChord = "\xEE\x8E\xA4";       // captions
inline constexpr const char* kHeight = "\xEE\x8D\xBD";      // arrow-up-down
inline constexpr const char* kSeconds = "\xEE\x8A\x96";     // hourglass
inline constexpr const char* kOctave = "\xEE\x83\xAF";      // hash
inline constexpr const char* kLanguages = "\xEE\x98\x85";   // letter-text
inline constexpr const char* kMarker = "\xEE\x84\x91";      // map-pin
inline constexpr const char* kReduced = "\xEE\x8A\x99";     // sun-dim
inline constexpr const char* kDrop = "\xEE\x82\xB4";        // droplet
inline constexpr const char* kLayers = "\xEE\x94\xA9";      // layers
inline constexpr const char* kPlus = "\xEE\x84\xBD";        // plus
inline constexpr const char* kMinus = "\xEE\x84\x9C";       // minus
inline constexpr const char* kSize = "\xEE\x85\x8B";        // ruler
inline constexpr const char* kBlend = "\xEE\x96\x9C";       // blend
inline constexpr const char* kReplay = "\xEE\x85\x9F";      // skip-back
inline constexpr const char* kRewind = "\xEE\x85\x87";      // rewind
inline constexpr const char* kForward = "\xEE\x82\xBD";     // fast-forward
inline constexpr const char* kPlay = "\xEE\x84\xBC";        // play
inline constexpr const char* kPause = "\xEE\x84\xAE";       // pause
inline constexpr const char* kMute = "\xEE\x86\xAC";        // volume-x
inline constexpr const char* kSoundOther = "\xEE\x86\xAA";  // volume-1
inline constexpr const char* kSoundBoth = "\xEE\x86\xAB";   // volume-2
inline constexpr const char* kCountdown = "\xEE\x88\xB6";   // timer-reset
inline constexpr const char* kSkipBack = "\xEE\x85\x88";    // rotate-ccw
inline constexpr const char* kSkipForward = "\xEE\x85\x89"; // rotate-cw
inline constexpr const char* kBar = "\xEE\x90\xB5";         // panel-top
inline constexpr const char* kTyping = "\xEE\x8A\x84";      // keyboard

} // namespace toonval::app::ui::icon
