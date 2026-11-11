// The splash at the start: the screen's emblem (emblem.h), a radar scope whose beam is a propeller, turning to rest
// while the screen joins its network and fetches its first map under it. On round glass the scope's rim is the glass's
// own edge, its ticks all the way around the bezel; on a rectangle the scope sits over the name. A touch ends it
// early (and is not the map's, which cannot be seen yet), and nothing waits for it: after a few seconds it fades into
// whatever the screen has ready by then, and its memory goes back.
#include "skyscope.h"

#include "colors.h"
#include "ui.h"
#include "widgets.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <esp_heap_caps.h>
#include <esp_timer.h>

namespace esphome::skyscope {
static const char *const TAG = "skyscope.splash";

namespace {
// The beam lights up as it starts, the emblem turns to rest (emblem.h SPIN_S), the name comes up meanwhile, and all of
// it fades out.
constexpr uint32_t FADE_IN_MS = 350, NAME_FROM_MS = 900, NAME_FADE_MS = 700, HOLD_UNTIL_MS = 3500, FADE_OUT_MS = 500;
// A frame at most this often: on a glass that sends slowly each one takes longer than this anyway.
constexpr uint32_t FRAME_MS = 33;
// PSRAM left alone for the rest of the start (the frames, the first tiles): with less, there is no splash.
constexpr size_t SPARE_BYTES = 1536 * 1024;
const char *const NAME = "SKYSCOPE";

lv_opa_t opa_of(float share) { return (lv_opa_t) std::lround(std::clamp(share, 0.0f, 1.0f) * 255); }
}  // namespace

void Skyscope::start_splash_() {
  if (splash_ || !ui_built_ || asleep_) return;
  // Round, the scope is the glass; on a rectangle it leaves room under it for the name and the version.
  const bool round = ui::shape.round;
  const lv_font_t *name_font = font_splash_ ? font_splash_->get_lv_font() : ui::fonts.title;
  const int name_tall = lv_font_get_line_height(name_font), line_tall = lv_font_get_line_height(ui::fonts.small);
  const int gap = ui::mm(2.5f), words = name_tall + line_tall + gap;
  int size = round ? std::min(width_, height_) : std::min(width_ * 9 / 10, height_ - words - 3 * gap);
  size &= ~1;  // even, so its middle is between four pixels and every pixel has its mirror
  if (size < ui::mm(15)) return;
  const float radius = size / 2.0f - (round ? 1 : 2);
  const size_t pixels = (size_t) size * size * 2, need = pixels + emblem::Emblem::bytes(size, radius);
  if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < need + SPARE_BYTES) {
    ESP_LOGI(TAG, "No splash: it takes %u KB, and too little is free", (unsigned) (need / 1024));
    return;
  }
  // Not cleared: the first frame draws every pixel of it.
  splash_pixels_ = static_cast<uint16_t *>(heap_caps_malloc(pixels, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!splash_pixels_) return;
  const int64_t from = esp_timer_get_time();
  splash_emblem_ = std::make_unique<emblem::Emblem>();
  // The screen's own colors; the propeller's shade is the emblem's.
  emblem::Colors paint;
  paint.ground = colors::BACKGROUND, paint.scope = colors::ACCENT, paint.blade = colors::INK;
  paint.blip = colors::WARNING;
  splash_emblem_->prepare(size, radius, paint);

  splash_ = widgets::plain(lv_screen_active());
  lv_obj_set_size(splash_, width_, height_);
  lv_obj_set_style_bg_color(splash_, lv_color_hex(colors::BACKGROUND), 0);
  lv_obj_set_style_bg_opa(splash_, LV_OPA_COVER, 0);
  // A touch is the splash's own, not the map's under it, which cannot be seen yet: it ends the splash now.
  lv_obj_add_flag(splash_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(splash_, [](lv_event_t *e) {
    auto *self = static_cast<Skyscope *>(lv_event_get_user_data(e));
    const uint32_t now = millis();
    if (!self->splash_waiting_ && now - self->splash_from_ < HOLD_UNTIL_MS) self->splash_from_ = now - HOLD_UNTIL_MS;
  }, LV_EVENT_PRESSED, this);
  splash_canvas_ = lv_canvas_create(splash_);
  lv_canvas_set_buffer(splash_canvas_, splash_pixels_, size, size, LV_COLOR_FORMAT_RGB565);
  lv_obj_remove_flag(splash_canvas_, LV_OBJ_FLAG_CLICKABLE);
  splash_name_ = widgets::text(splash_, name_font, colors::INK, NAME);
  lv_obj_set_style_text_letter_space(splash_name_, name_tall / 5, 0);
  lv_obj_set_style_text_opa(splash_name_, LV_OPA_TRANSP, 0);
  splash_line_ = widgets::text(splash_, ui::fonts.small, colors::MUTED, ("version " + version_).c_str());
  lv_obj_set_style_text_opa(splash_line_, LV_OPA_TRANSP, 0);
  if (round) {
    // The name and the version low in the scope, between the outer range ring and the ticks, clear of the blades at
    // rest and of the planes the beam finds.
    lv_obj_center(splash_canvas_);
    const int r = (int) radius;
    lv_obj_align(splash_name_, LV_ALIGN_CENTER, name_tall / 10, r * 72 / 100);
    lv_obj_align(splash_line_, LV_ALIGN_CENTER, 0, r * 835 / 1000);
  } else {
    const int top = (height_ - size - words - gap) / 2;
    lv_obj_align(splash_canvas_, LV_ALIGN_TOP_MID, 0, top);
    lv_obj_align(splash_name_, LV_ALIGN_TOP_MID, name_tall / 10, top + size + gap);
    lv_obj_align(splash_line_, LV_ALIGN_TOP_MID, 0, top + size + gap + name_tall + gap / 2);
  }
  splash_from_ = millis();
  splash_drawn_at_ = 0;
  splash_ticked_at_ = splash_from_;
  splash_gap_ms_ = 0;
  ESP_LOGI(TAG, "The emblem %d px across, made in %d ms, %u KB", size, (int) ((esp_timer_get_time() - from) / 1000),
           (unsigned) (need / 1024));
  splash_tick_(splash_from_);
  // Its clock starts again once that first frame is on the glass, so the turn is seen from its beginning.
  splash_waiting_ = true;
  splash_frame_at_ = ui::frame;
}

void Skyscope::splash_tick_(uint32_t now) {
  if (!splash_) return;
  // The loop read its clock before it built the screen and the splash: that pass is over before the splash began.
  if ((int32_t) (now - splash_from_) < 0) return;
  // The turn starts once the glass has shown the first frame.
  if (splash_waiting_) {
    if (ui::frame == splash_frame_at_) return;
    splash_waiting_ = false;
    splash_from_ = splash_ticked_at_ = now;
  }
  const uint32_t ms = now - splash_from_;
  splash_gap_ms_ = std::max(splash_gap_ms_, now - splash_ticked_at_);
  splash_ticked_at_ = now;
  if (asleep_ || ms >= HOLD_UNTIL_MS + FADE_OUT_MS) {
    end_splash_();
    return;
  }
  // Over whatever the screen has built since (the menu opens by itself on a screen with no place).
  stay_on_top_();
  // Out: the ground, the emblem and the words thin together, and what is under them shows through.
  const float out = ms <= HOLD_UNTIL_MS ? 1.0f : 1.0f - (float) (ms - HOLD_UNTIL_MS) / FADE_OUT_MS;
  const float in = std::min(1.0f, (float) ms / FADE_IN_MS);
  const float name = ms <= NAME_FROM_MS ? 0.0f : std::min(1.0f, (float) (ms - NAME_FROM_MS) / NAME_FADE_MS);
  if (out < 1) {
    lv_obj_set_style_bg_opa(splash_, opa_of(out), 0);
    lv_obj_set_style_image_opa(splash_canvas_, opa_of(out), 0);
  }
  lv_obj_set_style_text_opa(splash_name_, opa_of(name * out), 0);
  lv_obj_set_style_text_opa(splash_line_, opa_of(name * out * 0.9f), 0);
  // At rest nothing in the emblem changes: it is drawn once more there, then left.
  const float seconds = ms / 1000.0f;
  const bool turning = seconds < emblem::SPIN_S + 0.1f;
  // And not again until the glass has drawn the last one: two frames' parts at once could be more than the glass
  // keeps apart (32), and it would draw all of itself.
  const bool shown = !splash_drawn_at_ || ui::frame != splash_drawn_frame_;
  if ((turning || !splash_rested_) && shown && (!splash_drawn_at_ || now - splash_drawn_at_ >= FRAME_MS)) {
    splash_drawn_at_ = now | 1;
    splash_drawn_frame_ = ui::frame;
    splash_rested_ = !turning;
    const int64_t from = esp_timer_get_time();
    std::vector<emblem::Box> changed;
    splash_emblem_->draw(splash_pixels_, splash_emblem_->size(), emblem::bearing_at(seconds),
                         emblem::turned_at(seconds), in, &changed);
    // The first frame draws all of it; the rest only what turned.
    if (splash_frames_++) splash_draw_us_ += esp_timer_get_time() - from;
    else splash_first_us_ = esp_timer_get_time() - from;
    for (const emblem::Box &b : changed) splash_pixels_sent_ += (uint32_t) (b.w * b.h);
    // Only what the frame changed goes to the glass again.
    lv_area_t at;
    lv_obj_get_coords(splash_canvas_, &at);
    for (const emblem::Box &b : changed) {
      if (b.w <= 0 || b.h <= 0) continue;
      const lv_area_t area{at.x1 + b.x, at.y1 + b.y, at.x1 + b.x + b.w - 1, at.y1 + b.y + b.h - 1};
      lv_obj_invalidate_area(splash_canvas_, &area);
    }
  }
}

// The splash back over what was put on the glass after it. Moving it would have the glass draw all of itself again
// (a tenth of a second and more, the turn standing still); only what was over the splash needs drawing, now under it.
void Skyscope::stay_on_top_() {
  lv_obj_t *parent = lv_obj_get_parent(splash_);
  const int32_t at = lv_obj_get_index(splash_), count = (int32_t) lv_obj_get_child_count(parent);
  if (at < 0 || at == count - 1) return;
  lv_display_t *display = lv_obj_get_display(splash_);
  lv_display_enable_invalidation(display, false);
  lv_obj_move_foreground(splash_);
  lv_display_enable_invalidation(display, true);
  for (int32_t i = at; i < count - 1; ++i) lv_obj_invalidate(lv_obj_get_child(parent, i));
}

void Skyscope::end_splash_() {
  if (!splash_) return;
  lv_obj_delete(splash_);
  splash_ = splash_canvas_ = splash_name_ = splash_line_ = nullptr;
  splash_emblem_.reset();
  heap_caps_free(splash_pixels_);
  splash_pixels_ = nullptr;
  ESP_LOGI(TAG, "Done: %u frames; the first %.0f ms to draw, the rest %.1f ms each, %u px sent a frame",
           (unsigned) splash_frames_, splash_first_us_ / 1000.0f,
           splash_frames_ > 1 ? splash_draw_us_ / 1000.0f / (splash_frames_ - 1) : 0.0f,
           (unsigned) (splash_frames_ ? splash_pixels_sent_ / splash_frames_ : 0));
  splash_last_frames_ = splash_frames_;
  splash_last_gap_ms_ = splash_gap_ms_;
  splash_pixels_sent_ = 0;
  splash_frames_ = 0;
  splash_draw_us_ = 0;
  splash_rested_ = false;
  last_status_ = 0;
  if (canvas_) lv_obj_invalidate(lv_screen_active());
}
}  // namespace esphome::skyscope
