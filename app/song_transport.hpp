// SPDX-License-Identifier: GPL-3.0-or-later
// Binds the UI's SongPort (app/ui/song_port.hpp) to the SongController: the bar's buttons, the
// song overview and the resume countdown act on it and show its state. The skip steps and the
// countdown length come from the controller's options (AppSettings).
#pragma once

#include "song_controller.hpp"
#include "ui/song_port.hpp"

#include <cstdint>
#include <vector>

namespace toonval::app {

class SongTransport final : public ui::SongPort {
  public:
    // `songs` must outlive this object.
    explicit SongTransport(SongController& songs);

    // Refreshes view() from the controller. Once per frame, after SongController::update.
    void update(std::int64_t now_ns);

    const ui::SongView& view() const override { return view_; }
    void toggle_play(std::int64_t now_ns) override;
    void replay(std::int64_t now_ns) override;
    void skip_back(std::int64_t now_ns) override;
    void skip_forward(std::int64_t now_ns) override;
    void seek(double song_s, std::int64_t now_ns) override;
    void set_sound(ui::SongSound sound, std::int64_t now_ns) override;

  private:
    SongController& songs_;
    std::vector<double> bar_starts_;
    const LoadedSong* bars_for_ = nullptr; // the song bar_starts_ was built from
    std::uint64_t bars_hash_ = 0;
    // The controller counts whole seconds; the ring empties smoothly from when it started.
    std::int64_t countdown_start_ns_ = 0;
    int countdown_total_s_ = 0;
    ui::SongView view_;
};

} // namespace toonval::app
