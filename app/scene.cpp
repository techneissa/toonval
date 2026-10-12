// SPDX-License-Identifier: GPL-3.0-or-later
#include "scene.hpp"

#include "toonval/grade_words.hpp"
#include "toonval/pitch.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <string>
#include <tuple>

namespace toonval::app {

namespace {

const SDL_FColor kWhite = rgba(255, 255, 255);
const SDL_FColor kBlack = rgba(0, 0, 0);

// More cached note names than this means the sizes changed a lot (a resize drag): start again.
constexpr std::size_t kMaxCachedNames = 256;
// Most edge arrows drawn in one frame.
constexpr std::size_t kMaxEdgeMarks = 64;

float smoothstep(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

SDL_Rect pixel_rect(const RectF& r) {
    return {static_cast<int>(r.x), static_cast<int>(r.y), static_cast<int>(r.w),
            static_cast<int>(r.h)};
}

int note_index(int note) { return std::clamp(note, 0, 127); }

} // namespace

Scene::Scene(SDL_Renderer* renderer, const LookSettings& settings)
    : renderer_(renderer), settings_(settings.clamped()), text_(renderer), textures_(renderer) {
    block_names_.reserve(static_cast<std::size_t>(kMaxBlockLabelsPerFrame));
    seen_press_.fill(kNeverPressed);
    rebuild_names();
    update_recipe();
}

void Scene::set_settings(const LookSettings& settings) {
    settings_ = settings.clamped();
    geometry_valid_ = false;
    rebuild_names();
    update_recipe();
}

void Scene::set_effects_level(EffectsLevel level) {
    if (level != level_) {
        level_ = level;
        update_recipe();
    }
}

void Scene::update_recipe() {
    budget_ = effects_budget(level_);
    recipe_ = degraded(style_recipe(settings_), budget_);
}

std::int64_t Scene::trail_span_ns() const {
    return static_cast<std::int64_t>(static_cast<double>(settings_.visible_seconds) * 1e9);
}

// The colours and names depend only on the settings, so they are made once, not per frame.
void Scene::rebuild_names() {
    for (int note = 0; note < 128; ++note) {
        const auto i = static_cast<std::size_t>(note);
        const RGBA c = note_color(settings_, note, 100, kFreePlayHand);
        fill_[i] = to_fcolor(c);
        label_colour_[i] = to_fcolor(style_label_color(settings_, note, 100, kFreePlayHand));
        key_label_colour_[i] =
            to_fcolor(style_key_label_color(settings_, note, 100, kFreePlayHand));
        names_[i] = note_label(note, settings_);
        short_names_[i] = note_short_label(note, settings_);
        c_names_[i] = c_marker_label(note, settings_);
    }
    name_cache_.clear();
}

SDL_FColor Scene::fill_for(int note, int velocity) const {
    if (settings_.color_scheme == ColorScheme::ByVelocity) {
        return to_fcolor(
            note_color(settings_, note, static_cast<std::uint8_t>(velocity & 127), kFreePlayHand));
    }
    return fill_[static_cast<std::size_t>(note_index(note))];
}

SDL_FColor Scene::key_label_colour_for(int note, int velocity) const {
    if (settings_.color_scheme == ColorScheme::ByVelocity) {
        return to_fcolor(style_key_label_color(
            settings_, note, static_cast<std::uint8_t>(velocity & 127), kFreePlayHand));
    }
    return key_label_colour_[static_cast<std::size_t>(note_index(note))];
}

SDL_FColor Scene::label_colour_for(int note, int velocity) const {
    if (settings_.color_scheme == ColorScheme::ByVelocity) {
        return to_fcolor(style_label_color(
            settings_, note, static_cast<std::uint8_t>(velocity & 127), kFreePlayHand));
    }
    return label_colour_[static_cast<std::size_t>(note_index(note))];
}

void Scene::update_geometry(const KeyboardLayout& layout, const KeyViewFrame& view, float width,
                            float height) {
    const KeyRange& range = layout.range();
    if (geometry_valid_ && width == width_ && height == height_ && range.lowest == range_.lowest &&
        range.highest == range_.highest && view.pan == pan_ && view.span_whites == span_) {
        return;
    }
    // A new size, keyboard range or look starts the name cache again; a pan or zoom alone only
    // moves the keys.
    const bool restart = !geometry_valid_ || width != width_ || height != height_ ||
                         range.lowest != range_.lowest || range.highest != range_.highest;
    geometry_valid_ = true;
    width_ = width;
    height_ = height;
    range_ = range;
    pan_ = view.pan;
    span_ = view.span_whites;
    unit_ = std::max(height / 1080.0f, 0.25f);
    white_width_ = width / std::max(view.span_whites, 1.0f);
    // Pixel density of the display, assuming 96 dpi at density 1: sets the 15 mm touch target and
    // the physical text sizes. SDL does not report the panel's real size.
    px_per_mm_ = kDefaultPxPerMm;
    if (SDL_Window* window = SDL_GetRenderWindow(renderer_); window != nullptr) {
        const float density = SDL_GetWindowPixelDensity(window);
        if (density > 0.0f) {
            px_per_mm_ = density * kDefaultPxPerMm;
        }
    }
    // The top row is always reserved (the UI draws the status, gear and bar in it).
    layout_ = compute_layout(width, height, px_per_mm_, settings_);
    min_name_px_ = min_name_font_px(px_per_mm_);
    for (int note = kMidiNoteMin; note <= kMidiNoteMax; ++note) {
        const KeyRect k = view.to_window(layout.key(note), layout.white_key_count());
        keys_[static_cast<std::size_t>(note)] = {k.x * width, k.width * width, k.black,
                                                 range.contains(note) && k.width > 0.0f};
    }
    if (restart) {
        name_cache_.clear();
    }

    // One font size for all white keys and one for all black keys, so names do not jump between
    // sizes along the keyboard: each fits the longest name of its kind.
    white_name_px_ = 0.0f;
    black_name_px_ = 0.0f;
    c_marker_px_ = 0.0f;
    if (layout_.c_markers_visible) {
        // The same size for every C: it fits the longest marker ("C-1") on a white key.
        int chars = 0;
        for (int note = range.lowest; note <= range.highest; ++note) {
            const auto i = static_cast<std::size_t>(note);
            if (keys_[i].in_range) {
                chars = std::max(chars, static_cast<int>(c_names_[i].size()));
            }
        }
        const float gap = std::max(1.0f, 1.6f * unit_);
        c_marker_px_ = fit_label_font_px(layout_.c_marker_font_px, white_width_ - gap, chars);
    }
    if (layout_.key_labels_visible) {
        int white_chars = 0;
        int black_chars = 0;
        float black_width = 0.0f;
        for (int note = range.lowest; note <= range.highest; ++note) {
            const auto i = static_cast<std::size_t>(note);
            if (!keys_[i].in_range) {
                continue;
            }
            const int chars = static_cast<int>(names_[i].size());
            if (keys_[i].black) {
                black_chars = std::max(black_chars, chars);
                black_width = keys_[i].width;
            } else {
                white_chars = std::max(white_chars, chars);
            }
        }
        const float gap = std::max(1.0f, 1.6f * unit_);
        white_name_px_ =
            fit_label_font_px(layout_.key_label_font_px, white_width_ - gap, white_chars);
        black_name_px_ = fit_label_font_px(layout_.key_label_font_px, black_width, black_chars);
    }
}

void Scene::draw(const FrameInput& in) {
    int w = 0;
    int h = 0;
    SDL_GetCurrentRenderOutputSize(renderer_, &w, &h);
    update_geometry(in.layout, in.view, static_cast<float>(w), static_cast<float>(h));
    if (name_cache_.size() > kMaxCachedNames) {
        name_cache_.clear(); // between frames, so no texture in use is destroyed
    }

    if (!has_epoch_) {
        has_epoch_ = true;
        epoch_ns_ = in.now_ns;
    }

    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    draw_background(in.now_ns);
    draw_notes(in);
    draw_keyboard(in);
    draw_sparks(in);
    draw_chord(in.chord);
    draw_prompt(in);
}

// 1 at the note-on, falling to 0 over Look::flash_ns.
float Scene::flash_at(std::int64_t now_ns, std::int64_t pressed_ns) const {
    if (!settings_.key_flash || look_.flash_ns <= 0) {
        return 0.0f;
    }
    const std::int64_t age = now_ns - pressed_ns;
    if (age < 0 || age >= look_.flash_ns) {
        return 0.0f;
    }
    return 1.0f - static_cast<float>(age) / static_cast<float>(look_.flash_ns);
}

SDL_FColor Scene::hand_fill(int note, int velocity, Hand hand) const {
    if (hand == kFreePlayHand) {
        return fill_for(note, velocity);
    }
    return to_fcolor(
        note_color(settings_, note_index(note), static_cast<std::uint8_t>(velocity & 127), hand));
}

Scene::NotePaint Scene::paint(int note, int velocity, bool held, float flash, Hand hand) const {
    NotePaint p;
    const SDL_FColor fill = hand_fill(note, velocity, hand);
    const float whiten = look_.flash_whiten * flash;
    p.base = mix(fill, with_alpha(kWhite, fill.a), whiten);
    const SDL_FColor key_end = held ? mix(fill, kWhite, look_.held_highlight) : fill;
    p.lower = with_alpha(mix(key_end, kWhite, whiten), fill.a);
    // The style's own glow, more while held (its own switch) and a pulse on each press.
    const float held_extra = held && settings_.held_glow ? look_.held_glow_strength : 0.0f;
    p.glow = std::min(1.0f, recipe_.glow_alpha + held_extra + look_.flash_glow_strength * flash);
    return p;
}

SDL_FPoint Scene::draw_name(const std::string& text, float size, float centre_x, float bottom,
                            SDL_FColor colour, bool bold) {
    // Even sizes, like the text cache rounds to, so the cache key is the size actually drawn.
    const int px = std::max(2, static_cast<int>(size) & ~1);
    auto it = name_cache_.find({text, px, bold});
    if (it == name_cache_.end()) {
        it = name_cache_
                 .emplace(std::piecewise_construct, std::forward_as_tuple(text, px, bold),
                          std::forward_as_tuple())
                 .first;
    }
    TextSlot& slot = it->second;
    const SDL_FPoint extent = text_.prepare(slot, text, static_cast<float>(px), bold);
    text_.draw(slot, centre_x, bottom - extent.y, Align::Centre, colour);
    return extent;
}

void Scene::draw_background(std::int64_t now_ns) {
    mesh_.clear();
    if (settings_.effective_background() == BackgroundStyle::Plain) {
        mesh_.rect({0.0f, 0.0f, width_, height_}, look_.background_bottom, look_.background_bottom);
    } else {
        mesh_.rect({0.0f, 0.0f, width_, height_}, look_.background_top, look_.background_bottom);
    }
    // Faint lanes at every C and F, like the gaps between groups of black keys.
    const RectF& notes = layout_.note_area;
    const float line = std::max(1.0f, unit_);
    for (int note = range_.lowest + 1; note <= range_.highest; ++note) {
        const int pc = pitch_class(note);
        if (pc != 0 && pc != 5) {
            continue;
        }
        const KeyPx& k = keys_[static_cast<std::size_t>(note)];
        const SDL_FColor c = pc == 0 ? look_.guide : look_.guide_faint;
        mesh_.rect({k.x - line * 0.5f, notes.y, line, notes.h}, c, c);
    }
    mesh_.draw(renderer_, SDL_BLENDMODE_BLEND);

    // Motion: slow drifting light behind the notes, never over the bars above them.
    if (settings_.effective_background() == BackgroundStyle::Motion && budget_.motion_background &&
        !notes.empty()) {
        fx_.clear();
        build_motion_background(fx_, {notes.x, notes.y, notes.w, notes.h},
                                static_cast<double>(now_ns - epoch_ns_) * 1e-9);
        const SDL_Rect clip = effects_clip();
        SDL_SetRenderClipRect(renderer_, &clip);
        fx_.draw(renderer_, SDL_BLENDMODE_ADD, textures_.glow());
        SDL_SetRenderClipRect(renderer_, nullptr);
    }
}

void Scene::draw_notes(const FrameInput& in) {
    mesh_.clear();
    glow_.clear();
    painter_.begin(recipe_, unit_);
    block_names_.clear();
    spawn_sparks(in);
    const RectF& area = layout_.note_area;
    if (area.empty()) {
        return;
    }
    // The hit line is where the notes meet the key bed: the bottom of the note area.
    const float base_y = area.bottom();
    const float min_height = look_.min_block_height * unit_;
    const float gap = look_.block_gap * unit_;
    const float glow_size = recipe_.glow_width_share * white_width_;
    // Clip to the note area: blocks never draw over the chord label or the key bed.
    const SDL_Rect clip = pixel_rect(area);
    edge_marks_.clear();
    draw_song_bars(in, base_y, min_height, gap, glow_size);
    for (const TrailBar& bar : in.bars) {
        const KeyPx& k = keys_[bar.note];
        if (!k.in_range) {
            // Off the left or right edge of the view (or off the keyboard): an arrow at that edge
            // instead of the block vanishing. The out-of-range count reports the ones off the
            // keyboard too.
            const int side = in.view.side_of(bar.note);
            if (side != 0 && edge_marks_.size() < kMaxEdgeMarks) {
                float top = base_y - static_cast<float>(bar.top) * area.h;
                float bottom = bar.held ? base_y : base_y - static_cast<float>(bar.bottom) * area.h;
                top = std::min(top, bottom - min_height);
                float alpha = 1.0f;
                if (!bar.held && settings_.trail_fade) {
                    const float fade = (static_cast<float>(bar.bottom) - look_.fade_start) /
                                       std::max(1.0f - look_.fade_start, 1e-3f);
                    alpha = 1.0f - smoothstep(fade);
                }
                if (alpha > 0.0f) {
                    const NotePaint p =
                        paint(bar.note, bar.velocity, bar.held, flash_at(in.now_ns, bar.start_ns));
                    edge_marks_.push_back({side, 0.5f * (top + bottom), 0.5f * (bottom - top),
                                           with_alpha(p.base, p.base.a * alpha)});
                }
            }
            continue;
        }
        const float inset = k.width * (k.black ? look_.black_note_inset : look_.white_note_inset);
        const float x = k.x + inset;
        const float w = k.width - 2.0f * inset;
        const float radius = std::min(look_.block_radius * w, look_.block_radius_max * unit_);
        float top = base_y - static_cast<float>(bar.top) * area.h;
        float bottom = base_y - static_cast<float>(bar.bottom) * area.h;
        if (bar.held) {
            bottom = base_y + radius; // Cut flat by the clip, so it meets the bed's edge cleanly.
        } else if (bottom - top > min_height + gap) {
            bottom -= gap;
        }
        top = std::min(top, bottom - min_height);

        float alpha = 1.0f;
        if (!bar.held && settings_.trail_fade) {
            const float fade = (static_cast<float>(bar.bottom) - look_.fade_start) /
                               std::max(1.0f - look_.fade_start, 1e-3f);
            alpha = 1.0f - smoothstep(fade);
        }
        alpha *= in.trail_alpha;
        if (alpha <= 0.0f) {
            continue;
        }
        const NotePaint p =
            paint(bar.note, bar.velocity, bar.held, flash_at(in.now_ns, bar.start_ns));
        const SDL_FRect rect{x, top, w, bottom - top};
        if (p.glow > 0.0f && glow_size > 0.0f) {
            glow_.nine_slice({x - glow_size, top - glow_size, w + 2.0f * glow_size,
                              bottom - top + 2.0f * glow_size},
                             glow_size + radius,
                             with_alpha(saturated(p.base, recipe_.saturation), p.glow * alpha));
        }
        painter_.add(rect, radius, p.base, p.lower, alpha);

        // The name sits at the block's bottom edge: whole when it fits at a legible size, else
        // the letter alone, else nothing. The frame draws at most kMaxBlockLabelsPerFrame names:
        // the blocks come oldest first, nearest the keys.
        add_block_name(bar.note, {x, top, w, bottom - top}, area,
                       label_colour_for(bar.note, bar.velocity), alpha);
    }
    // Blocks reaching the top dissolve into the background instead of ending at a hard edge.
    const float fog = look_.top_fade * area.h;
    const SDL_FColor bottom_colour = look_.background_bottom;
    const SDL_FColor sky =
        settings_.effective_background() == BackgroundStyle::Plain
            ? bottom_colour
            : mix(look_.background_top, bottom_colour, area.y / std::max(height_, 1.0f));
    mesh_.rect({area.x, area.y, area.w, fog}, sky, with_alpha(sky, 0.0f));
    draw_edge_indicators();
    SDL_SetRenderClipRect(renderer_, &clip);
    glow_.draw(renderer_, SDL_BLENDMODE_ADD, textures_.glow());
    painter_.draw(renderer_, textures_);
    mesh_.draw(renderer_, SDL_BLENDMODE_BLEND);
    for (const PendingName& n : block_names_) {
        draw_name(*n.text, n.size, n.centre_x, n.bottom, n.colour);
    }
    draw_grades(in);
    SDL_SetRenderClipRect(renderer_, nullptr);
}

// The song's notes falling to the hit line, in their hand's colour like the keys. A note of a hand
// you are not practising is faint; a graded note brightens when hit and greys out when missed. A
// note moved by octaves to fit the keyboard carries a small arrow at its top, pointing the way
// it moved.
void Scene::draw_song_bars(const FrameInput& in, float base_y, float min_height, float gap,
                           float glow_size) {
    const RectF& area = layout_.note_area;
    const SDL_FColor grey = rgba(120, 124, 136);
    for (const SongBar& bar : in.song_bars) {
        const KeyPx& k = keys_[bar.key & 127];
        float top = base_y - bar.top * area.h;
        const float bottom = base_y - bar.bottom * area.h;
        if (bottom - top > min_height + gap) {
            top += gap; // a repeated note shows as two blocks
        }
        top = std::min(top, bottom - min_height);
        float alpha = bar.practised ? 1.0f : 0.32f;
        const bool graded = bar.grade >= 0;
        const bool missed = graded && static_cast<NoteGrade>(bar.grade) == NoteGrade::Missed;
        const float flash = graded && !missed ? 0.45f : 0.0f;
        NotePaint p = paint(bar.key, bar.velocity, false, flash, bar.hand);
        if (missed) {
            p.base = with_alpha(mix(p.base, grey, 0.7f), p.base.a);
            p.lower = with_alpha(mix(p.lower, grey, 0.7f), p.lower.a);
            p.glow = 0.0f;
            alpha *= 0.6f;
        }
        if (!k.in_range) {
            const int side = in.view.side_of(bar.key);
            if (side != 0 && edge_marks_.size() < kMaxEdgeMarks && bottom > area.y &&
                top < base_y) {
                edge_marks_.push_back({side, 0.5f * (top + bottom), 0.5f * (bottom - top),
                                       with_alpha(p.base, p.base.a * alpha)});
            }
            continue;
        }
        const float inset = k.width * (k.black ? look_.black_note_inset : look_.white_note_inset);
        const float x = k.x + inset;
        const float w = k.width - 2.0f * inset;
        const float radius = std::min(look_.block_radius * w, look_.block_radius_max * unit_);
        const SDL_FRect rect{x, top, w, bottom - top};
        if (p.glow > 0.0f && glow_size > 0.0f && bar.practised) {
            glow_.nine_slice({x - glow_size, top - glow_size, w + 2.0f * glow_size,
                              bottom - top + 2.0f * glow_size},
                             glow_size + radius,
                             with_alpha(saturated(p.base, recipe_.saturation), p.glow * alpha));
        }
        painter_.add(rect, radius, p.base, p.lower, alpha);
        if (bar.fold != 0) {
            const float a = std::min(0.4f * w, 10.0f * unit_);
            const float cx = x + 0.5f * w;
            const float ty = top + 0.6f * a + 2.0f * unit_;
            const float dir = bar.fold > 0 ? -1.0f : 1.0f; // raised: points up
            const SDL_FColor c = with_alpha(kWhite, 0.85f * alpha);
            mesh_.triangle({cx, ty + dir * 0.5f * a}, {cx - 0.5f * a, ty - dir * 0.5f * a},
                           {cx + 0.5f * a, ty - dir * 0.5f * a}, c, c, c);
        }
        add_block_name(bar.key & 127, {x, top, w, bottom - top}, area,
                       bar.hand == kFreePlayHand ? label_colour_for(bar.key, bar.velocity)
                                                 : to_fcolor(style_label_color(
                                                       settings_, bar.key, bar.velocity, bar.hand)),
                       alpha);
    }
}

// Queues the name of `note` for the block: the whole name when it fits at a legible size, else
// the letter alone, else none. Names are collected while the blocks are built and drawn after
// them; the frame draws at most kMaxBlockLabelsPerFrame, nearest the keyboard first.
void Scene::add_block_name(int note, const RectF& block, const RectF& clip, SDL_FColor colour,
                           float alpha) {
    if (!layout_.block_labels_visible ||
        block_names_.size() >= static_cast<std::size_t>(kMaxBlockLabelsPerFrame)) {
        return;
    }
    const auto i = static_cast<std::size_t>(note_index(note));
    const std::string& full = names_[i];
    if (full.empty()) {
        return;
    }
    const std::string& letter = short_names_[i];
    const NamePlacement np =
        place_block_name(block, clip, layout_.note_label_font_px, static_cast<int>(full.size()),
                         static_cast<int>(letter.size()), min_name_px_);
    if (np.form == NameForm::Hidden) {
        return;
    }
    colour.a *= alpha;
    const LabelPlacement& lp = np.label;
    block_names_.push_back({np.form == NameForm::Full ? &full : &letter, lp.font_px,
                            lp.rect.x + lp.rect.w * 0.5f, lp.rect.bottom(), colour, lp.rect});
}

float Scene::grade_font_px() const {
    return std::max(kMinLabelFontPx, std::max(layout_.note_label_font_px * 1.15f, 18.0f * unit_));
}

// Flow Mode's grade words, kept in a row just above the hit line or at the top of the note area
// (the look's choice) at the look's opacity. Core's place_grade_words decides where each goes so
// none overlaps another word, a note name or the chord label; a word with no room is left out.
void Scene::draw_grades(const FrameInput& in) {
    const RectF& area = layout_.note_area;
    if (in.grades.empty() || area.empty() || !settings_.grade_words_shown()) {
        return;
    }
    static const std::string kWords[] = {"Perfect", "Good", "Early", "Late", "Miss", "OK"};
    static const SDL_FColor kColours[] = {rgba(255, 214, 92),  rgba(120, 230, 140),
                                          rgba(120, 190, 255), rgba(255, 160, 90),
                                          rgba(235, 90, 90),   rgba(200, 220, 255)};
    const float size = grade_font_px();
    const float pad_x = 0.45f * size;
    const float pad_y = 0.12f * size;
    const float opacity = settings_.grade_alpha();

    std::array<GradeWordIn, kMaxGradeWords> words{};
    std::array<std::uint8_t, kMaxGradeWords> kind{};
    std::size_t n = 0;
    float text_h = 0.0f;
    for (const GradePopup& g : in.grades) {
        const KeyPx& k = keys_[g.key & 127];
        const auto i = static_cast<std::size_t>(g.grade);
        if (!k.in_range || i >= std::size(kWords) || n >= kMaxGradeWords) {
            continue;
        }
        const SDL_FPoint extent = text_.prepare(grade_slot_[i], kWords[i], size, true);
        words[n] = {k.x + 0.5f * k.width, extent.x + 2.0f * pad_x, std::clamp(g.age, 0.0f, 1.0f)};
        kind[n] = static_cast<std::uint8_t>(i);
        text_h = std::max(text_h, extent.y);
        ++n;
    }
    if (n == 0) {
        return;
    }
    std::array<RectF, kMaxBlockLabelsPerFrame> avoid{};
    std::size_t avoid_count = 0;
    for (const PendingName& name : block_names_) {
        if (avoid_count < avoid.size()) {
            avoid[avoid_count++] = name.rect;
        }
    }
    GradeLayoutParams params;
    params.note_area = area;
    params.chord_label = layout_.chord_label;
    params.position = settings_.grade_position;
    params.word_h = text_h + 2.0f * pad_y;
    std::array<GradeWordOut, kMaxGradeWords> placed{};
    place_grade_words(params, std::span<const RectF>(avoid.data(), avoid_count),
                      std::span<const GradeWordIn>(words.data(), n),
                      std::span<GradeWordOut>(placed.data(), n));

    // Backdrops first, so a word reads over the falling blocks, then the words.
    mesh_.clear();
    for (std::size_t j = 0; j < n; ++j) {
        if (!placed[j].visible) {
            continue;
        }
        const float a = opacity * grade_word_alpha(words[j].age);
        const SDL_FColor backdrop = with_alpha(look_.chord_backdrop, look_.chord_backdrop.a * a);
        const RectF& r = placed[j].rect;
        mesh_.rounded_rect({r.x, r.y, r.w, r.h}, 0.3f * r.h, backdrop, backdrop, 0.05f * r.h);
    }
    mesh_.draw(renderer_, SDL_BLENDMODE_BLEND);
    for (std::size_t j = 0; j < n; ++j) {
        if (!placed[j].visible) {
            continue;
        }
        const float a = opacity * grade_word_alpha(words[j].age);
        const RectF& r = placed[j].rect;
        TextSlot& slot = grade_slot_[kind[j]];
        text_.draw(slot, r.x + 0.5f * r.w, r.y + pad_y, Align::Centre,
                   with_alpha(kColours[kind[j]], a));
    }
}

// A soft strip down the window edge at the height of each block whose key is off screen, with a
// small arrow pointing the way to the key, in the block's own colour. Added to `mesh_` before it
// is drawn, so it shares the note clip.
void Scene::draw_edge_indicators() {
    const RectF& area = layout_.note_area;
    const float arrow = 16.0f * unit_;
    const float strip = 30.0f * unit_;
    for (const EdgeMark& m : edge_marks_) {
        const float dir = static_cast<float>(m.side); // -1 points left
        const float edge = m.side < 0 ? area.x : area.right();
        const float h = std::max(m.half_h, 0.5f * arrow);
        const SDL_FColor strong = with_alpha(m.colour, 0.7f * m.colour.a);
        const SDL_FColor clear = with_alpha(m.colour, 0.0f);
        const float inner = edge - dir * strip;
        // Gradient strip: strong at the edge, fading inward.
        const float x0 = std::min(edge, inner);
        const float x1 = std::max(edge, inner);
        const SDL_FColor at_x0 = m.side < 0 ? strong : clear;
        const SDL_FColor at_x1 = m.side < 0 ? clear : strong;
        const float y0 = m.centre - h;
        const float y1 = m.centre + h;
        mesh_.triangle({x0, y0}, {x1, y0}, {x0, y1}, at_x0, at_x1, at_x0);
        mesh_.triangle({x1, y0}, {x1, y1}, {x0, y1}, at_x1, at_x1, at_x0);
        // Arrow tip on the very edge, base inside it.
        const SDL_FColor solid = with_alpha(m.colour, 0.95f * m.colour.a);
        const float tip = edge;
        const float base = edge - dir * arrow;
        mesh_.triangle({tip, m.centre}, {base, m.centre - 0.6f * arrow},
                       {base, m.centre + 0.6f * arrow}, solid, solid, solid);
    }
}

void Scene::draw_keyboard(const FrameInput& in) {
    const RectF& area = layout_.key_area;
    const RectF& bed = layout_.key_bed;
    if (area.empty()) {
        return;
    }
    // `glow_` still holds the blocks' glow from draw_notes; the keys' glow joins it, and the lot
    // is drawn once more over the bed at the end.
    const float top = area.y;
    const float bottom = area.bottom() - 1.5f * unit_;
    const float gap = std::max(1.0f, 1.6f * unit_);
    const float white_radius = 0.12f * white_width_;
    const float black_length = area.h * look_.black_length;
    const float glow_size = 0.7f * white_width_ * budget_.glow_width;
    const float tip_h = std::min(1.6f * layout_.key_shadow_h, area.h);
    // A see-through style shows the dark background through a lit key, as through its block,
    // tinted with the note's colour (core key_underlay_color) so the key still reads as lit.
    const bool see_through = style_is_see_through(recipe_);

    // A lit key is painted like its note's block, by the same painter and recipe: the same
    // colour, style, flash and glow.
    struct Lit {
        bool on = false;
        int velocity = 0;
        NotePaint paint;
    };
    const auto lit_paint = [&](int note) {
        const std::size_t i = static_cast<std::size_t>(note);
        const float flash = flash_at(in.now_ns, in.last_press_ns[i]);
        const bool held = in.keys.is_down(note);
        Lit lit;
        lit.on = held || flash > 0.0f;
        lit.velocity =
            held ? in.keys.press_velocity(note).value_or(in.last_velocity[i]) : in.last_velocity[i];
        lit.paint = paint(note, lit.velocity, held, flash);
        if (!lit.on && in.hint_hands != nullptr && (*in.hint_hands)[i] != 0) {
            // Wait Mode: a key to press pulses in its hand's colour.
            const Hand hand = static_cast<Hand>((*in.hint_hands)[i] - 1);
            const double t = static_cast<double>(in.now_ns - epoch_ns_) * 1e-9;
            const float pulse =
                0.5f + 0.5f * static_cast<float>(std::sin(t * 2.0 * 3.14159265 * 1.2));
            lit.on = true;
            lit.velocity = 80;
            lit.paint = paint(note, lit.velocity, false, 0.15f + 0.25f * pulse, hand);
            lit.paint.glow = std::max(lit.paint.glow, 0.45f + 0.45f * pulse);
        }
        return lit;
    };
    const auto underlay_for = [&](const NotePaint& p) {
        return with_alpha(mix(look_.background_bottom, p.base, kKeyUnderlayTint), 1.0f);
    };
    const auto add_glow = [&](const KeyPx& k, const NotePaint& p) {
        if (p.glow > 0.0f && glow_size > 0.0f) {
            // On the bed, where it may bleed up onto the notes' end but never onto the key faces.
            const float h = 0.6f * bed.h;
            glow_.nine_slice({k.x - glow_size, bed.y + 0.2f * bed.h - glow_size,
                              k.width + 2.0f * glow_size, h + 2.0f * glow_size},
                             glow_size + 0.3f * bed.h,
                             with_alpha(saturated(p.base, recipe_.saturation), p.glow));
        }
    };
    // The key tips down: a darker band along its top, as tall as the bed's shadow and then some.
    const auto add_tip_band = [&](float x, float width, const NotePaint& p) {
        const SDL_FColor dark =
            with_alpha(scaled(p.base, look_.pressed_band_darken), look_.pressed_band_alpha);
        overlay_.rect({x, top, width, tip_h}, dark, with_alpha(dark, 0.0f));
    };

    // Keys draw only inside the key area, so nothing crosses the bed.
    const SDL_Rect key_clip = pixel_rect(area);
    SDL_SetRenderClipRect(renderer_, &key_clip);

    // White keys first so black keys draw on top. Every key starts at the top of the key area (the
    // rounded corners sit above it and are clipped away), so the keys begin where the bed ends.
    // Each pass: unlit faces, then the lit faces in the note style, then the bands over them.
    mesh_.clear();
    overlay_.clear();
    painter_.begin(recipe_, unit_);
    for (int note = range_.lowest; note <= range_.highest; ++note) {
        const KeyPx& k = keys_[static_cast<std::size_t>(note)];
        if (k.black || !k.in_range) {
            continue;
        }
        const Lit lit = lit_paint(note);
        const SDL_FRect face{k.x + gap * 0.5f, top - white_radius, k.width - gap,
                             bottom - top + white_radius};
        if (!lit.on) {
            mesh_.rounded_rect(face, white_radius, look_.key_white_top, look_.key_white_bottom,
                               0.8f * unit_);
            continue;
        }
        painter_.add(face, white_radius, lit.paint.base, lit.paint.lower, 1.0f,
                     see_through ? underlay_for(lit.paint) : SDL_FColor{0, 0, 0, 0});
        add_tip_band(face.x, face.w, lit.paint);
        add_glow(k, lit.paint);
    }
    mesh_.draw(renderer_, SDL_BLENDMODE_BLEND);
    painter_.draw(renderer_, textures_);
    overlay_.draw(renderer_, SDL_BLENDMODE_BLEND);

    mesh_.clear();
    overlay_.clear();
    painter_.begin(recipe_, unit_);
    for (int note = range_.lowest; note <= range_.highest; ++note) {
        const KeyPx& k = keys_[static_cast<std::size_t>(note)];
        if (!k.black || !k.in_range) {
            continue;
        }
        const Lit lit = lit_paint(note);
        const float radius = 0.16f * k.width;
        const SDL_FRect face{k.x, top - radius, k.width, black_length + radius};
        // A lighter lip near the front edge gives the key some depth.
        const float lip = 0.07f * black_length;
        const SDL_FRect lip_rect{k.x + 0.12f * k.width, top + black_length - 1.6f * lip,
                                 k.width * 0.76f, lip};
        if (!lit.on) {
            mesh_.rounded_rect(face, radius, look_.key_black_top, look_.key_black_bottom,
                               0.8f * unit_);
            mesh_.rounded_rect(lip_rect, 0.5f * lip, with_alpha(look_.key_black_lip, 0.0f),
                               look_.key_black_lip, 0.0f);
            continue;
        }
        painter_.add(face, radius, lit.paint.base, lit.paint.lower, 1.0f,
                     see_through ? underlay_for(lit.paint) : SDL_FColor{0, 0, 0, 0});
        const SDL_FColor lip_colour = with_alpha(mix(lit.paint.base, kWhite, 0.25f), 0.6f);
        overlay_.rounded_rect(lip_rect, 0.5f * lip, with_alpha(lip_colour, 0.0f), lip_colour, 0.0f);
        add_tip_band(k.x, k.width, lit.paint);
        add_glow(k, lit.paint);
    }
    // The bed's shadow on the top of every key, so they look like they slide under it.
    const SDL_FColor shadow = with_alpha(kBlack, look_.shadow_alpha);
    overlay_.rect({0.0f, top, width_, layout_.key_shadow_h}, shadow, with_alpha(shadow, 0.0f));
    mesh_.draw(renderer_, SDL_BLENDMODE_BLEND);
    painter_.draw(renderer_, textures_);
    overlay_.draw(renderer_, SDL_BLENDMODE_BLEND);

    // Names on the keys, near the front edge of each. A lit key takes the label colour that reads
    // on its note colour, an unlit one a fixed colour for its key colour. A C key shows its marker
    // (name and octave, middle C bold and strong, the others dim) instead of its plain name.
    if (layout_.key_labels_visible || c_marker_px_ > 0.0f) {
        mesh_.clear();
        for (int note = range_.lowest; note <= range_.highest; ++note) {
            const std::size_t i = static_cast<std::size_t>(note);
            const KeyPx& k = keys_[i];
            if (!k.in_range) {
                continue;
            }
            const float cx = k.x + 0.5f * k.width;
            const float label_bottom = k.black ? top + 0.88f * black_length : bottom - 4.0f * unit_;
            if (c_marker_px_ > 0.0f && !c_names_[i].empty()) {
                const bool middle = is_middle_c(note);
                const Lit lit = lit_paint(note);
                SDL_FColor colour = middle ? look_.c_marker_middle : look_.c_marker_other;
                if (lit.on) {
                    colour = key_label_colour_for(note, lit.velocity);
                    colour.a = middle ? 1.0f : look_.c_marker_lit_alpha;
                }
                const SDL_FPoint extent =
                    draw_name(c_names_[i], c_marker_px_, cx, label_bottom, colour, middle);
                if (middle) {
                    // An accent underline, so middle C is found at a glance.
                    const float h = std::max(1.5f, look_.c_underline_height * c_marker_px_);
                    const float w = look_.c_underline_width * extent.x;
                    const SDL_FColor accent = lit.on ? colour : look_.accent;
                    mesh_.rounded_rect(
                        {cx - 0.5f * w, label_bottom - 0.05f * c_marker_px_ - h, w, h}, 0.5f * h,
                        accent, accent, 0.0f);
                }
                continue;
            }
            const float size = k.black ? black_name_px_ : white_name_px_;
            if (!layout_.key_labels_visible || size <= 0.0f || names_[i].empty()) {
                continue;
            }
            const Lit lit = lit_paint(note);
            SDL_FColor colour = k.black ? look_.key_label_black : look_.key_label_white;
            if (lit.on) {
                colour = key_label_colour_for(note, lit.velocity);
            }
            draw_name(names_[i], size, cx, label_bottom, colour);
        }
        mesh_.draw(renderer_, SDL_BLENDMODE_BLEND);
    }
    // The computer-keyboard letters as small keycaps, white keys at their middle and black keys on
    // their face, so they never meet the note names near the front edge.
    if (in.typing_labels != nullptr) {
        const float cap_px = std::min(0.46f * white_width_, 24.0f * unit_);
        if (cap_px >= kMinLabelFontPx) {
            struct Cap {
                int note;
                float cx;
                float cy;
                bool black;
            };
            std::vector<Cap> caps;
            for (int note = range_.lowest; note <= range_.highest; ++note) {
                const std::size_t i = static_cast<std::size_t>(note);
                const KeyPx& k = keys_[i];
                if (!k.in_range || (*in.typing_labels)[i].empty()) {
                    continue;
                }
                const float cy =
                    k.black ? top + 0.40f * black_length : top + 0.60f * (bottom - top);
                caps.push_back({note, k.x + 0.5f * k.width, cy, k.black});
            }
            const float side = 1.55f * cap_px;
            mesh_.clear();
            for (const Cap& c : caps) {
                const SDL_FColor fill =
                    c.black ? rgba(214, 220, 232, 0.92f) : rgba(36, 40, 54, 0.88f);
                mesh_.rounded_rect({c.cx - 0.5f * side, c.cy - 0.5f * side, side, side},
                                   0.22f * side, fill, fill, 0.8f * unit_);
            }
            mesh_.draw(renderer_, SDL_BLENDMODE_BLEND);
            for (const Cap& c : caps) {
                const std::string text((*in.typing_labels)[static_cast<std::size_t>(c.note)]);
                const SDL_FColor ink = c.black ? rgba(20, 24, 34) : rgba(240, 244, 255);
                // draw_name sets the text's lower edge at `bottom`: centre the glyphs on the cap.
                draw_name(text, cap_px, c.cx, c.cy + 0.5f * cap_px, ink, true);
            }
        }
    }
    SDL_SetRenderClipRect(renderer_, nullptr);

    // The key bed: the strip the notes land on. It ends exactly where the keys begin, with a thin
    // felt line on its lower edge and the accent hit line on its top edge.
    mesh_.clear();
    mesh_.rect({bed.x, bed.y, bed.w, bed.h}, look_.bed_top, look_.bed_bottom);
    mesh_.rect({0.0f, bed.bottom() - layout_.felt_line_h, width_, layout_.felt_line_h},
               look_.felt_line, look_.felt_line);
    const SDL_FColor line = with_alpha(look_.accent, 0.5f);
    mesh_.rect({0.0f, bed.y, width_, std::min(layout_.felt_line_h, bed.h)}, line, line);
    mesh_.draw(renderer_, SDL_BLENDMODE_BLEND);

    // Glow of the blocks and keys that reaches onto the bed, clipped so it stops at the keys.
    const SDL_Rect bed_clip = pixel_rect(bed);
    SDL_SetRenderClipRect(renderer_, &bed_clip);
    glow_.draw(renderer_, SDL_BLENDMODE_ADD, textures_.glow());
    SDL_SetRenderClipRect(renderer_, nullptr);
}

void Scene::spawn_sparks(const FrameInput& in) {
    const bool on = settings_.effective_sparks() && budget_.particles;
    for (int note = range_.lowest; note <= range_.highest; ++note) {
        const std::size_t i = static_cast<std::size_t>(note);
        const std::int64_t pressed = in.last_press_ns[i];
        if (pressed == seen_press_[i]) {
            continue;
        }
        seen_press_[i] = pressed;
        // Only fresh presses: a press older than a few frames (a hitch, the first frame) is past.
        constexpr std::int64_t kFreshNs = 100'000'000;
        const KeyPx& k = keys_[i];
        if (!on || !k.in_range || in.now_ns - pressed > kFreshNs || layout_.note_area.empty()) {
            continue;
        }
        const int velocity = in.last_velocity[i];
        sparks_.spawn(k.x + 0.5f * k.width, layout_.note_area.bottom(), k.width,
                      saturated(fill_for(note, velocity), 1.2f), velocity, unit_);
    }
}

void Scene::draw_sparks(const FrameInput& in) {
    if (!settings_.effective_sparks() || !budget_.particles) {
        sparks_.clear();
        return;
    }
    sparks_.update(in.now_ns, unit_);
    fx_.clear();
    sparks_.build(fx_);
    // Over the notes and the bed, never over the keys or the top row.
    const SDL_Rect clip = effects_clip();
    SDL_SetRenderClipRect(renderer_, &clip);
    fx_.draw(renderer_, SDL_BLENDMODE_ADD, textures_.glow());
    SDL_SetRenderClipRect(renderer_, nullptr);
}

SDL_Rect Scene::effects_clip() const {
    const RectF& notes = layout_.note_area;
    const float bottom = layout_.key_bed.empty() ? notes.bottom() : layout_.key_bed.bottom();
    return pixel_rect({notes.x, notes.y, notes.w, std::max(0.0f, bottom - notes.y)});
}

// Centred in the chord strip.
void Scene::draw_chord(const ChordLabel& chord) {
    const RectF& area = layout_.chord_label;
    const float right = area.right();
    if (chord.text.empty() || chord.opacity <= 0.0 || !layout_.chord_label_visible) {
        return;
    }
    const float opacity = static_cast<float>(chord.opacity);
    const float size = layout_.chord_label_font_px;
    const float pad_x = 0.35f * size;
    // A long name shrinks to fit the width, and hides when that would make it unreadable.
    const SDL_FPoint extent = text_.prepare_fit(chord_slot_, chord.text, size,
                                                right - area.x - 2.0f * pad_x, kMinChordFontPx);
    if (extent.x <= 0.0f) {
        return;
    }
    const float y = area.y + (area.h - extent.y) * 0.5f;
    const float cx = area.x + 0.5f * (right - area.x);
    // A soft dark backdrop keeps the name readable on any background.
    const float pad_y = 0.04f * size;
    const SDL_FColor backdrop = with_alpha(look_.chord_backdrop, look_.chord_backdrop.a * opacity);
    mesh_.clear();
    mesh_.rounded_rect(
        {cx - extent.x * 0.5f - pad_x, y - pad_y, extent.x + 2.0f * pad_x, extent.y + 2.0f * pad_y},
        0.25f * size, backdrop, backdrop, 0.08f * size);
    mesh_.draw(renderer_, SDL_BLENDMODE_BLEND);
    text_.draw(chord_slot_, cx, y, Align::Centre,
               with_alpha(look_.chord_text, look_.chord_text.a * opacity));
}

void Scene::draw_prompt(const FrameInput& in) {
    draw_results(in);
    if (in.no_keyboard && !layout_.note_area.empty()) {
        // Centred in the note area, shrunk to fit it.
        const RectF& notes = layout_.note_area;
        const float free_w = notes.w;
        const SDL_FPoint extent = text_.prepare_fit(
            prompt_slot_, "Connect a MIDI keyboard, or click the keys or type A S D F",
            std::max(kMinTextPx, 44.0f * unit_), free_w * 0.9f, kMinTextPx);
        text_.draw(prompt_slot_, notes.x + 0.5f * free_w, notes.y + (notes.h - extent.y) * 0.5f,
                   Align::Centre, look_.prompt_text);
    }
}

// The last run's results: one line on a soft backdrop in the note area while the song is
// stopped, shrunk to fit the width. (The resume countdown is the UI's.)
void Scene::draw_results(const FrameInput& in) {
    const RectF& notes = layout_.note_area;
    if (notes.empty() || in.results.empty()) {
        return;
    }
    const float free_w = notes.w;
    const SDL_FPoint extent = text_.prepare_fit(
        results_slot_, in.results, std::max(kMinTextPx, 36.0f * unit_), free_w * 0.9f, kMinTextPx);
    if (extent.x <= 0.0f) {
        return;
    }
    const float cx = notes.x + 0.5f * free_w;
    const float y = notes.y + 0.4f * notes.h - 0.5f * extent.y;
    const float pad = 0.5f * extent.y;
    const SDL_FColor backdrop = with_alpha(look_.chord_backdrop, 0.8f);
    mesh_.clear();
    mesh_.rounded_rect(
        {cx - 0.5f * extent.x - pad, y - 0.5f * pad, extent.x + 2.0f * pad, extent.y + pad},
        0.5f * pad, backdrop, backdrop, 0.06f * extent.y);
    mesh_.draw(renderer_, SDL_BLENDMODE_BLEND);
    text_.draw(results_slot_, cx, y, Align::Centre, look_.chord_text);
}

} // namespace toonval::app
