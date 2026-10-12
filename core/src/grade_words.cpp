// SPDX-License-Identifier: GPL-3.0-or-later
#include "toonval/grade_words.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace toonval {
namespace {

bool finite_rect(const RectF& r) {
    return std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.w) && std::isfinite(r.h);
}

// True when `a` and `b` are closer than `gap` on both axes.
bool too_close(const RectF& a, const RectF& b, float gap) {
    return a.x < b.right() + gap && b.x < a.right() + gap && a.y < b.bottom() + gap &&
           b.y < a.bottom() + gap;
}

// The edge of the area rows start from, and whether they grow up or down from it.
struct Rows {
    float edge = 0.0f;
    float step = 0.0f; // row pitch, signed: negative grows upward from the bottom edge
    bool valid = false;
};

Rows rows_of(const GradeLayoutParams& p) {
    Rows r;
    const RectF& area = p.note_area;
    if (!finite_rect(area) || area.empty() || !std::isfinite(p.word_h) || !(p.word_h > 0.0f)) {
        return r;
    }
    const float pitch = p.word_h + kGradeWordGapPx;
    if (p.position == GradePosition::Top) {
        float top = area.y;
        if (finite_rect(p.chord_label) && !p.chord_label.empty()) {
            top = std::max(top, p.chord_label.bottom());
        }
        r.edge = top + kGradeWordGapPx;
        r.step = pitch;
    } else {
        r.edge = area.bottom() - kGradeWordGapPx; // the lower edge of row 0
        r.step = -pitch;
    }
    r.valid = true;
    return r;
}

// Row `row` as a rect `w` wide centred on `centre_x` and moved to stay inside the area; empty
// when it does not fit.
RectF row_rect(const GradeLayoutParams& p, const Rows& rows, int row, float centre_x, float w) {
    const RectF& area = p.note_area;
    const float y = rows.step < 0.0f ? rows.edge - p.word_h + static_cast<float>(row) * rows.step
                                     : rows.edge + static_cast<float>(row) * rows.step;
    if (!(w > 0.0f) || w > area.w || y < area.y || y + p.word_h > area.bottom()) {
        return {};
    }
    const float x = std::clamp(centre_x - 0.5f * w, area.x, area.right() - w);
    return RectF{x, y, w, p.word_h};
}

} // namespace

RectF grade_band(const GradeLayoutParams& params) {
    const Rows rows = rows_of(params);
    if (!rows.valid) {
        return {};
    }
    RectF band;
    bool any = false;
    for (int row = 0; row < kGradeWordRows; ++row) {
        const RectF r = row_rect(params, rows, row, params.note_area.x, params.note_area.w);
        if (r.empty()) {
            break;
        }
        if (!any) {
            band = r;
            any = true;
        } else {
            const float x0 = std::min(band.x, r.x);
            const float y0 = std::min(band.y, r.y);
            const float x1 = std::max(band.right(), r.right());
            const float y1 = std::max(band.bottom(), r.bottom());
            band = RectF{x0, y0, x1 - x0, y1 - y0};
        }
    }
    return band;
}

float grade_word_alpha(float age) {
    if (!(age >= 0.0f) || age > 1.0f) {
        return 0.0f;
    }
    const float fade_in = std::min(1.0f, age / kGradeFadeIn);
    return fade_in * (1.0f - age * age);
}

std::size_t place_grade_words(const GradeLayoutParams& params, std::span<const RectF> avoid,
                              std::span<const GradeWordIn> words, std::span<GradeWordOut> out) {
    const std::size_t n = std::min(words.size(), out.size());
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = GradeWordOut{};
    }
    const Rows rows = rows_of(params);
    if (!rows.valid || n == 0) {
        return 0;
    }

    // Newest first (smallest age); a stable insertion sort of indices in a fixed array.
    std::array<std::size_t, kMaxGradeWords> order{};
    std::size_t count = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (!std::isfinite(words[i].age) || !std::isfinite(words[i].centre_x) ||
            !std::isfinite(words[i].width)) {
            continue;
        }
        std::size_t at = count < kMaxGradeWords ? count : kMaxGradeWords;
        if (count == kMaxGradeWords) {
            // Full: the new word replaces the oldest only if it is newer.
            const std::size_t oldest = order[count - 1];
            if (words[i].age >= words[oldest].age) {
                continue;
            }
            at = count - 1;
        } else {
            ++count;
        }
        while (at > 0 && words[order[at - 1]].age > words[i].age) {
            order[at] = order[at - 1];
            --at;
        }
        order[at] = i;
    }

    std::array<RectF, kMaxGradeWords> placed{};
    std::size_t placed_count = 0;
    for (std::size_t k = 0; k < count; ++k) {
        const std::size_t i = order[k];
        for (int row = 0; row < kGradeWordRows; ++row) {
            const RectF r = row_rect(params, rows, row, words[i].centre_x, words[i].width);
            if (r.empty()) {
                break; // this row is outside the area, and so is every one beyond it
            }
            bool free = true;
            for (std::size_t j = 0; j < placed_count && free; ++j) {
                free = !too_close(r, placed[j], kGradeWordGapPx);
            }
            for (std::size_t j = 0; j < avoid.size() && free; ++j) {
                free = !finite_rect(avoid[j]) || avoid[j].empty() ||
                       !too_close(r, avoid[j], kGradeWordGapPx);
            }
            if (free && finite_rect(params.chord_label) && !params.chord_label.empty()) {
                free = !too_close(r, params.chord_label, 0.0f);
            }
            if (free) {
                out[i] = GradeWordOut{true, r, row};
                placed[placed_count++] = r;
                break;
            }
        }
    }
    return placed_count;
}

} // namespace toonval
