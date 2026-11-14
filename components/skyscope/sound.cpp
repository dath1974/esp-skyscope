// The screen's sounds (sound.h), on a board with a speaker. Each cue's samples are made the first time it is wanted and
// kept (in PSRAM); a cue is handed to the speaker a piece at a time on each pass of the loop, as much as its buffer
// takes, so the loop never waits on it. The speaker is one source of the board's mixer, beside Home Assistant's media
// player: both play at once, and the mixer's source stops by itself once nothing has come for a while.
//
// The volume is the speaker's, one for the screen's sounds and the media player alike: the left strip, the menu, the
// page and Home Assistant all set the same. A change on the screen is handed to the media player (Home Assistant shows
// it), and one from Home Assistant comes back as the screen's setting.
//
// A click is heard at each tap LVGL takes on a key, a switch or a card (not on the map: a finger that pans is no
// tap), told to it by the touchscreen's input device.
#include "skyscope.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::skyscope {
static const char *const TAG = "skyscope.sound";

#ifdef USE_SKYSCOPE_SPEAKER
namespace {
// The most bytes handed over in one pass: a tenth of a second.
constexpr size_t FEED_BYTES = sound::RATE / 10 * 2;
}  // namespace
#endif

void Skyscope::start_sound_() {
#ifdef USE_SKYSCOPE_SPEAKER
  if (!speaker_) return;
  speaker_fitted = true;
#ifdef USE_MEDIA_PLAYER
  // Home Assistant turned the media player up or down: the screen's volume with it.
  if (media_player_) media_player_->add_on_state_callback([this](media_player::MediaPlayerState) {
    const int percent = sound::percent_of(media_player_->volume);
    if (percent != settings_.volume && drag_volume_ < 0) {
      ESP_LOGI(TAG, "Volume %d%%, from the media player", percent);
      this->set_volume(percent);
    }
  });
#endif
  ESP_LOGI(TAG, "A speaker: sounds at %d%%", settings_.volume);
#endif
}

void Skyscope::set_volume(int percent) {
  Settings s = settings_;
  s.volume = std::max(0, std::min(100, percent));
  if (s.volume != settings_.volume) use_settings_(s);
}

// The volume set, or the one a finger is dragging to, given to the speaker (the codec) once it changes; a setting
// changed on the screen or the page given to the media player too.
void Skyscope::apply_volume_() {
#ifdef USE_SKYSCOPE_SPEAKER
  const int want = drag_volume_ >= 0 ? drag_volume_ : settings_.volume;
  if (want == applied_volume_) return;
  applied_volume_ = want;
  speaker_->set_volume(sound::codec_volume(want));
#ifdef USE_MEDIA_PLAYER
  if (media_player_ && drag_volume_ < 0 && sound::percent_of(media_player_->volume) != want)
    media_player_->make_call().set_volume(want / 100.0f).perform();
#endif
#endif
}

void Skyscope::play_(sound::Cue cue, bool asked) {
#ifdef USE_SKYSCOPE_SPEAKER
  if (!speaker_ || cue >= sound::CUES) return;
  if (asked) {
    // From the web page: heard whatever its setting, but not with the speaker turned down to nothing.
    if (settings_.volume <= 0) return;
  } else {
    int minute = -1;
#ifdef USE_TIME
    if (clock_) {
      const ESPTime t = clock_->now();
      if (t.is_valid()) minute = t.hour * 60 + t.minute;
    }
#endif
    if (!sound::sounds(cue, settings_, minute, bed_dark_)) return;
  }
  // One playing goes on unless this one is as pressing or more; a click never cuts another short.
  if (playing_ && (cue == sound::CLICK || sound::rank(cue) < sound::rank(playing_cue_))) return;
  sound::Samples &samples = cues_[cue];
  if (samples.empty()) samples = sound::render(cue);
  playing_ = &samples;
  playing_cue_ = cue;
  played_ = 0;
  if (!speaker_->is_running()) {
    speaker_->set_audio_stream_info(audio::AudioStreamInfo(16, 1, sound::RATE));
    speaker_->start();
  }
  ESP_LOGD(TAG, "Playing %s", sound::CUE_NAMES[cue]);
  sound_tick_(millis());
#endif
}

void Skyscope::sound_tick_(uint32_t now) {
#ifdef USE_SKYSCOPE_SPEAKER
  if (!speaker_) return;
  apply_volume_();
  if (!playing_) return;
  // `played_` counts bytes: the speaker may take an odd number of them.
  const size_t total = playing_->size() * sizeof(int16_t);
  const auto *bytes = reinterpret_cast<const uint8_t *>(playing_->data());
  // Until the speaker has started, it takes nothing: the cue waits for it.
  played_ += speaker_->play(bytes + played_, std::min(FEED_BYTES, total - played_), 0);
  if (played_ >= total) playing_ = nullptr;
#endif
}

// A tap LVGL took on anything but the map and the two strips (which hand theirs to the map): a click.
void Skyscope::click_(lv_obj_t *target) {
#ifdef USE_SKYSCOPE_SPEAKER
  if (!speaker_ || asleep_ || !target) return;
  if (target == canvas_ || target == dimmer_ || target == volume_strip_) return;
  play_(sound::CLICK);
#endif
}

void Skyscope::listen_for_clicks_() {
#ifdef USE_SKYSCOPE_SPEAKER
  if (!speaker_) return;
  for (lv_indev_t *indev = lv_indev_get_next(nullptr); indev; indev = lv_indev_get_next(indev)) {
    if (lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) continue;
    lv_indev_add_event_cb(indev, [](lv_event_t *e) {
      static_cast<Skyscope *>(lv_event_get_user_data(e))->click_(static_cast<lv_obj_t *>(lv_event_get_param(e)));
    }, LV_EVENT_CLICKED, this);
  }
#endif
}
}  // namespace esphome::skyscope
