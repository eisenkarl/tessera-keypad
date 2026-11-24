#include "code_lock.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace esphome::code_lock {

using tessera::Font;
using tessera::Tone;
namespace ui = tessera::ui;

static constexpr uint32_t ICON_LOCK = 0xF033E, ICON_LOCK_OPEN = 0xF033F, ICON_CHECK = 0xF012C, ICON_CLOSE = 0xF0156;
static constexpr size_t MAX_DIGITS = 16;  // what the keypad takes

// Option ids for entry n (0-based): target / code for the first, target_2 / code_2 … for the rest.
static const char *target_option(size_t i) {
  static const char *IDS[] = {"target", "target_2", "target_3", "target_4",
                              "target_5", "target_6", "target_7", "target_8"};
  return i < MAX_ENTRIES ? IDS[i] : IDS[0];
}
static const char *code_option(size_t i) {
  static const char *IDS[] = {"code", "code_2", "code_3", "code_4", "code_5", "code_6", "code_7", "code_8"};
  return i < MAX_ENTRIES ? IDS[i] : IDS[0];
}

// The action the right code runs, by the entity's domain (named in the manifest's home_assistant_actions).
static const char *service_for(const char *entity) {
  static const struct {
    const char *domain, *service;
  } MAP[] = {
      {"lock.", "lock.unlock"},
      {"cover.", "cover.open_cover"},
      {"switch.", "switch.toggle"},
      {"input_boolean.", "input_boolean.toggle"},
      {"light.", "light.toggle"},
      {"script.", "script.turn_on"},
      {"scene.", "scene.turn_on"},
      {"button.", "button.press"},
  };
  for (const auto &m : MAP)
    if (std::strncmp(entity, m.domain, std::strlen(m.domain)) == 0) return m.service;
  return nullptr;
}

// The action that closes the entity from what it is now, for a tap without the code (the option free_close).
static const char *closing_service(const char *entity, const char *state) {
  auto is = [&](const char *domain) { return std::strncmp(entity, domain, std::strlen(domain)) == 0; };
  auto now = [&](const char *word) { return std::strcmp(state, word) == 0; };
  if (is("lock.")) return now("unlocked") || now("open") ? "lock.lock" : nullptr;
  if (is("cover.")) return now("open") || now("opening") ? "cover.close_cover" : nullptr;
  if (!now("on")) return nullptr;
  if (is("light.")) return "light.turn_off";
  if (is("switch.")) return "switch.turn_off";
  if (is("input_boolean.")) return "input_boolean.turn_off";
  return nullptr;
}

// A text option as digits only (spaces dropped).
static void option_digits(JsonVariantConst value, char *out, size_t size) {
  out[0] = 0;
  char raw[32];
  raw[0] = 0;
  if (value.is<const char *>()) std::strncpy(raw, value.as<const char *>(), sizeof(raw) - 1);
  else if (value.is<long>()) std::snprintf(raw, sizeof(raw), "%ld", value.as<long>());
  raw[sizeof(raw) - 1] = 0;
  size_t n = 0;
  for (size_t i = 0; raw[i] && n + 1 < size; ++i)
    if (raw[i] >= '0' && raw[i] <= '9') out[n++] = raw[i];
  out[n] = 0;
}

// Helper state → digits only; empty when unavailable or not digits.
static void code_from_state(const char *state, char *out, size_t size) {
  out[0] = 0;
  size_t n = 0;
  for (const char *p = state ? state : ""; *p; ++p) {
    if (*p == ' ') continue;
    if (*p < '0' || *p > '9' || n + 1 >= size) {
      out[0] = 0;
      return;
    }
    out[n++] = *p;
  }
  out[n] = 0;
}

static void code_from_data(JsonObjectConst data, char *out, size_t size) {
  JsonVariantConst helper = data["entities"]["code_entity"];
  code_from_state(helper.isNull() ? "" : (helper["state"] | ""), out, size);
}

// Fill lock.entries from tile options (and optional helper for the first code).
static void load_entries(Lock &lock, JsonObjectConst options) {
  lock.count = 0;
  for (size_t i = 0; i < MAX_ENTRIES; ++i) {
    Entry e;
    std::strncpy(e.entity, options[target_option(i)] | "", sizeof(e.entity) - 1);
    if (i == 0 && lock.from_entity) {
      // First code comes from the helper later in on_state; leave empty for now if helper chosen.
      e.code[0] = 0;
    } else {
      option_digits(options[code_option(i)], e.code, sizeof(e.code));
    }
    if (!e.entity[0] || !service_for(e.entity)) continue;
    // Slot 1 may still be waiting for a helper code; keep it if entity is set.
    if (i > 0 && !e.code[0]) continue;
    if (i == 0 && !lock.from_entity && !e.code[0]) continue;
    lock.entries[lock.count++] = e;
  }
  if (lock.count)
    std::strncpy(lock.tries_key, lock.entries[0].entity, sizeof(lock.tries_key) - 1);
  else
    lock.tries_key[0] = 0;
}

static bool entry_ready(const Entry &e) { return e.entity[0] && e.code[0] && service_for(e.entity); }

static bool lock_has_code(const Lock &lock) {
  for (uint8_t i = 0; i < lock.count; ++i)
    if (lock.entries[i].code[0]) return true;
  return false;
}

// Index of the matching entry, or -1. Constant-time compare per candidate.
static int matching_entry(const Lock &lock, const char *typed, size_t typed_len) {
  int found = -1;
  for (uint8_t i = 0; i < lock.count; ++i) {
    const Entry &e = lock.entries[i];
    if (!e.code[0]) continue;
    const size_t n = std::strlen(e.code);
    unsigned diff = typed_len != n;
    for (size_t j = 0; j < n; ++j) {
      const char t = j < typed_len ? typed[j] : 0;
      diff |= static_cast<unsigned>(e.code[j] ^ t);
    }
    if (diff == 0) found = static_cast<int>(i);
  }
  return found;
}

static bool codes_have_length(const Lock &lock, size_t len) {
  for (uint8_t i = 0; i < lock.count; ++i)
    if (lock.entries[i].code[0] && std::strlen(lock.entries[i].code) == len) return true;
  return false;
}

CodeLock::Tries &CodeLock::tries(const char *entity) {
  for (auto &t : tries_)
    if (std::strcmp(t.entity, entity) == 0) return t;
  Tries &t = tries_[next_];
  next_ = (next_ + 1) % 4;
  t = Tries();
  std::strncpy(t.entity, entity, sizeof(t.entity) - 1);
  return t;
}

// ---- The tile ----
class KeypadTile : public tessera::Tile {
 public:
  explicit KeypadTile(CodeLock *plugin) : plugin_(plugin) {}

  void create(const tessera::TileContext &c) override {
    width_ = c.width;
    height_ = c.height;
    tile_ = c.tile;
    lock_ = Lock();
    lock_.from_entity = (c.options["code_entity"] | "")[0] != 0;
    lock_.attempts = std::max(1, c.options["attempts"] | 3);
    lock_.auto_check = c.options["auto_check"] | true;
    lock_.free_close = c.options["free_close"] | false;
    lock_.wait_s = std::max(1, c.options["wait"] | 30);
    load_entries(lock_, c.options);
    std::strncpy(name_, c.name ? c.name : "", sizeof(name_) - 1);

    icon_ = ui::label(c.parent, Font::ICON, theme::ACCENT);
    name_label_ = ui::label(c.parent, Font::TITLE, theme::INK);
    line_ = ui::label(c.parent, Font::BODY, theme::MUTED);
    for (lv_obj_t *l : {icon_, name_label_, line_}) {
      lv_obj_set_width(l, width_);
      lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    }
    paint();
  }

  void on_state(JsonObjectConst data) override {
    // First entity drives the tile's name/state display.
    JsonObjectConst target = data["entities"]["target"];
    if (!target["name"].isNull() && !name_[0]) std::strncpy(name_, target["name"] | "", sizeof(name_) - 1);
    std::strncpy(state_, target["state"] | "", sizeof(state_) - 1);
    if (lock_.from_entity && lock_.count > 0) {
      code_from_data(data, lock_.entries[0].code, sizeof(lock_.entries[0].code));
    }
    // Refresh entity ids if Tessera resends options via entities.* (names only usually).
    paint();
  }

  void on_theme() override {
    ui::set_color(icon_, theme::ACCENT);
    ui::set_color(name_label_, theme::INK);
    ui::set_color(line_, theme::MUTED);
  }

  void on_tap() override {
    if (!ready()) return;
    const char *entity0 = lock_.count ? lock_.entries[0].entity : "";
    const char *closing = lock_.free_close ? closing_service(entity0, state_) : nullptr;
    if (closing) {
      const bool sent = tessera::action(closing, entity0);
      ESP_LOGI("code_lock", "Without the code: %s on %s %s", closing, entity0, sent ? "sent" : "NOT sent");
      (void) sent;
      return;
    }
    plugin_->chosen = lock_;
    tessera::open_card(plugin_->plugin_id(), "keys", lock_.tries_key, tile_);
  }

 private:
  bool ready() const {
    if (!lock_.count) return false;
    if (lock_.from_entity) return entry_ready(lock_.entries[0]) || (lock_.entries[0].entity[0] && service_for(lock_.entries[0].entity));
    for (uint8_t i = 0; i < lock_.count; ++i)
      if (entry_ready(lock_.entries[i])) return true;
    return false;
  }

  std::string line() const {
    if (!lock_.count) return plugin_->text("choose");
    const Entry &e0 = lock_.entries[0];
    if (!e0.entity[0] || !service_for(e0.entity)) return plugin_->text("choose");
    if (!lock_has_code(lock_)) return plugin_->text(lock_.from_entity ? "no_helper_code" : "no_code");
    const char *s = state_;
    if (!std::strcmp(s, "unavailable")) return plugin_->text("unavailable");
    if (lock_.free_close && closing_service(e0.entity, s))
      return plugin_->text(std::strncmp(e0.entity, "lock.", 5) == 0   ? "tap_lock"
                           : std::strncmp(e0.entity, "cover.", 6) == 0 ? "tap_close"
                                                                        : "tap_off");
    if (!std::strcmp(s, "locked")) return plugin_->text("locked");
    if (!std::strcmp(s, "unlocked")) return plugin_->text("unlocked");
    if (!std::strcmp(s, "open")) return plugin_->text("open");
    if (!std::strcmp(s, "closed")) return plugin_->text("closed");
    if (!std::strcmp(s, "on")) return plugin_->text("on");
    if (!std::strcmp(s, "off") && std::strncmp(e0.entity, "script.", 7)) return plugin_->text("off");
    if (lock_.count > 1) return plugin_->text("tap_multi");
    return plugin_->text("tap");
  }

  void paint() {
    const bool open = !std::strcmp(state_, "unlocked") || !std::strcmp(state_, "open");
    ui::set_text(icon_, ui::icon(open ? ICON_LOCK_OPEN : ICON_LOCK));
    ui::set_text(name_label_, name_);
    ui::set_text(line_, line());
    const bool roomy = ui::line_height(Font::ICON) + ui::px(4) + ui::line_height(Font::HEADLINE) +
                           ui::line_height(Font::BODY_LARGE) <= height_ / 2 &&
                       ui::text_width(name_, Font::HEADLINE) <= width_ &&
                       ui::text_width(line(), Font::BODY_LARGE) <= width_;
    const Font name_font = roomy ? Font::HEADLINE : Font::TITLE, line_font = roomy ? Font::BODY_LARGE : Font::BODY;
    ui::set_font(name_label_, name_font);
    ui::set_font(line_, line_font);
    const int ih = ui::line_height(Font::ICON), nh = ui::line_height(name_font), lh = ui::line_height(line_font);
    const int gap = ui::px(4);
    const bool with_icon = ih + gap + nh + lh <= height_;
    const bool with_name = name_[0] && nh + lh <= height_;
    const int total = (with_icon ? ih + gap : 0) + (with_name ? nh : 0) + lh;
    int y = std::max(0, (height_ - total) / 2);
    if (with_icon) {
      lv_obj_remove_flag(icon_, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_pos(icon_, 0, y);
      y += ih + gap;
    } else {
      lv_obj_add_flag(icon_, LV_OBJ_FLAG_HIDDEN);
    }
    if (with_name) {
      lv_obj_remove_flag(name_label_, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_pos(name_label_, 0, y);
      y += nh;
    } else {
      lv_obj_add_flag(name_label_, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_pos(line_, 0, y);
  }

  CodeLock *plugin_;
  Lock lock_;
  char name_[48] = "", state_[24] = "";
  int width_ = 0, height_ = 0, tile_ = -1;
  lv_obj_t *icon_{}, *name_label_{}, *line_{};
};

// ---- The keypad card ----
class KeysCard : public tessera::Card {
 public:
  explicit KeysCard(CodeLock *plugin) : plugin_(plugin) {}

  void open(const tessera::CardContext &c) override {
    lock_ = plugin_->chosen;
    const bool large = ui::large();
    const int side = ui::px(large ? 20 : 12);
    int g = ui::px(large ? 12 : 6);
    text_h_ = ui::line_height(Font::BODY_LARGE);
    const int least = std::max(ui::mm(8), text_h_ + ui::px(8));
    int kh = std::max(ui::px(large ? 64 : 42), least);
    dot_max_ = ui::px(large ? 16 : 10);
    const int x = side, top = side, w = c.width - 2 * side, h = c.height - 2 * side;
    beside_ = w * 10 >= h * 13;
    const int info_h = dot_max_ + g + text_h_;
    const int keys_h = beside_ ? h : h - info_h - 2 * g;
    if (4 * least + 3 * g > keys_h) g = std::max(2, (keys_h - 4 * least) / 3);
    if (4 * kh + 3 * g > keys_h) kh = std::max(least, (keys_h - 3 * g) / 4);
    kh = std::max(1, std::min(kh, (keys_h - 3 * g) / 4));
    int kw = kh * 3 / 2;
    const int third = ((beside_ ? w * 64 / 100 : w) - 2 * g) / 3;
    kw = std::min(std::max(std::min(kw, third), least), third);
    const int pad_w = 3 * kw + 2 * g, pad_h = 4 * kh + 3 * g;
    const int info_w = beside_ ? std::min(w - pad_w - 2 * g, pad_w) : w;
    const int pair_x = beside_ ? x + (w - (info_w + 2 * g + pad_w)) / 2 : x;
    const int px0 = beside_ ? pair_x + info_w + 2 * g : x + (w - pad_w) / 2;
    const int py0 = beside_ ? top + (h - pad_h) / 2 : top + (h - (info_h + 2 * g + pad_h)) / 2 + info_h + 2 * g;
    key_w_ = kw;
    key_h_ = kh;
    gap_ = g;
    x0_ = px0;
    y0_ = py0;
    info_x_ = beside_ ? pair_x : x;
    info_w_ = info_w;
    const int info_y = beside_ ? top : py0 - 2 * g - info_h, info_hh = beside_ ? h : info_h + g;
    const int lines = beside_ ? 3 : 1;
    const int block = dot_max_ + g + lines * text_h_;
    dots_y_ = info_y + (info_hh - block) / 2;

    for (auto &d : dots_) {
      d = ui::block(c.parent, theme::CARD);
      lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
      lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
    }
    line_ = lines > 1 ? ui::text(c.parent, Font::BODY_LARGE, theme::MUTED)
                      : ui::label(c.parent, Font::BODY_LARGE, theme::MUTED);
    lv_obj_set_size(line_, info_w, lines * text_h_);
    lv_obj_set_pos(line_, info_x_, dots_y_ + dot_max_ + g);
    lv_obj_set_style_text_align(line_, LV_TEXT_ALIGN_CENTER, 0);

    Font digit = Font::BODY_LARGE;
    for (Font f : {Font::VALUE, Font::HEADLINE, Font::TITLE, Font::BODY_LARGE}) {
      digit = f;
      if (ui::line_height(f) <= kh * 6 / 10) break;
    }
    const Font icon = ui::line_height(Font::ICON) <= kh - ui::px(10) ? Font::ICON : Font::ICON_SMALL;
    for (int i = 0; i < 12; ++i) {
      lv_obj_t *k = ui::block(c.parent, i == OK ? theme::ACCENT : theme::CARD);
      lv_obj_set_style_pad_all(k, 0, 0);
      lv_obj_set_size(k, kw, kh);
      lv_obj_set_pos(k, px0 + (i % 3) * (kw + g), py0 + (i / 3) * (kh + g));
      lv_obj_set_style_radius(k, kh / 2, 0);
      lv_obj_set_style_border_width(k, i == OK ? 0 : 1, 0);
      lv_obj_set_style_border_color(k, ui::color(theme::LINE), 0);
      const Font face = i == OK || i == CLEAR ? icon : digit;
      lv_obj_t *l = ui::label(k, face, i == OK ? theme::ON_ACCENT : theme::INK);
      lv_obj_set_width(l, kw);
      lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_pos(l, 0, (kh - ui::line_height(face)) / 2);
      if (i == CLEAR) ui::set_text(l, ui::icon(ICON_CLOSE));
      else if (i == OK) ui::set_text(l, ui::icon(ICON_CHECK));
      else ui::set_text(l, std::string(1, i == ZERO ? '0' : static_cast<char>('1' + i)));
      keys_[i] = k;
      labels_[i] = l;
    }
    paint();
  }

  bool keypad() const { return true; }

  void on_state(JsonObjectConst data) override {
    if (lock_.from_entity && lock_.count > 0 && !data["entities"].isNull())
      code_from_data(data, lock_.entries[0].code, sizeof(lock_.entries[0].code));
  }

  void on_theme() override {
    for (int i = 0; i < 12; ++i) {
      ui::set_fill(keys_[i], Tone::NORMAL, i == OK ? theme::ACCENT : theme::CARD);
      lv_obj_set_style_border_color(keys_[i], ui::color(theme::LINE), 0);
      ui::set_color(labels_[i], i == OK ? theme::ON_ACCENT : theme::INK);
    }
    paint();
  }

  void on_tick(uint32_t) override {
    if (said_ == Said::SENT && esphome::millis() - said_at_ >= 1500) {
      tessera::close_card();
      return;
    }
    paint();
  }

  void on_tap() override {
    if (said_ == Said::SENT || blocked()) return;
    const tessera::Point p = tessera::tap_point();
    const int key = key_at(p.x, p.y);
    if (key < 0) return;
    if (key == CLEAR) {
      clear();
      said_ = Said::NOTHING;
    } else if (key == OK) {
      if (typed_len_) submit();
    } else if (typed_len_ < MAX_DIGITS) {
      if (said_ == Said::WRONG) said_ = Said::NOTHING;
      typed_[typed_len_++] = key == ZERO ? '0' : static_cast<char>('1' + key);
      typed_[typed_len_] = 0;
      if (lock_.auto_check && lock_has_code(lock_) && codes_have_length(lock_, typed_len_)) submit();
    }
    paint();
  }

 private:
  enum class Said : uint8_t { NOTHING, WRONG, SENT, FAILED, NO_CODE };
  static constexpr int CLEAR = 9, ZERO = 10, OK = 11;

  void clear() {
    std::memset(typed_, 0, sizeof(typed_));
    typed_len_ = 0;
  }

  bool blocked() const {
    const auto &t = plugin_->tries(lock_.tries_key);
    return t.blocked_until && static_cast<int32_t>(t.blocked_until - esphome::millis()) > 0;
  }

  int key_at(int x, int y) const {
    if (x < 0 || y < 0) return -1;
    const int half = gap_ / 2;
    for (int i = 0; i < 12; ++i) {
      const int kx = x0_ + (i % 3) * (key_w_ + gap_), ky = y0_ + (i / 3) * (key_h_ + gap_);
      if (x >= kx - half && x < kx + key_w_ + half && y >= ky - half && y < ky + key_h_ + half) return i;
    }
    return -1;
  }

  void submit() {
    auto &t = plugin_->tries(lock_.tries_key);
    said_at_ = esphome::millis();
    if (!lock_has_code(lock_)) {
      clear();
      said_ = Said::NO_CODE;
      return;
    }
    const int idx = matching_entry(lock_, typed_, typed_len_);
    if (idx < 0) {
      clear();
      said_ = Said::WRONG;
      ESP_LOGI(TAG, "Wrong code");
      if (++t.wrong >= lock_.attempts) {
        t.wrong = 0;
        t.blocked_until = esphome::millis() + static_cast<uint32_t>(lock_.wait_s) * 1000u;
        if (!t.blocked_until) t.blocked_until = 1;
      }
      return;
    }
    t.wrong = 0;
    clear();
    const Entry &e = lock_.entries[idx];
    const char *service = service_for(e.entity);
    const bool sent = service && tessera::action(service, e.entity);
    ESP_LOGI(TAG, "Right code: %s on %s %s", service ? service : "(no action)", e.entity, sent ? "sent" : "NOT sent");
    said_ = sent ? Said::SENT : Said::FAILED;
  }

  void paint() {
    const uint32_t now = esphome::millis();
    const bool held = blocked();
    const int shown = std::max<int>(4, static_cast<int>(typed_len_));
    int dot = dot_max_;
    while (dot > 4 && shown * dot + (shown - 1) * (dot * 3 / 5) > info_w_) --dot;
    const int dgap = dot * 3 / 5, row = shown * dot + (shown - 1) * dgap;
    const int left = info_x_ + (info_w_ - row) / 2, y = dots_y_ + (dot_max_ - dot) / 2;
    const bool bad = said_ == Said::WRONG && now - said_at_ < 4000;
    for (int i = 0; i < static_cast<int>(MAX_DIGITS); ++i) {
      lv_obj_t *d = dots_[i];
      if (i >= shown) {
        lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
        continue;
      }
      lv_obj_remove_flag(d, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_size(d, dot, dot);
      lv_obj_set_pos(d, left + i * (dot + dgap), y);
      const bool filled = i < static_cast<int>(typed_len_) || (bad && i < 4);
      if (filled) ui::set_fill(d, bad ? Tone::ALERT : Tone::NORMAL, theme::INK);
      else ui::set_fill(d, Tone::NORMAL, theme::CARD);
      lv_obj_set_style_border_width(d, filled ? 0 : std::max(1, dot / 7), 0);
      lv_obj_set_style_border_color(d, ui::color(theme::TICK), 0);
    }
    std::string text = plugin_->text("enter");
    Tone tone = Tone::NORMAL;
    if (held) {
      const auto &tr = plugin_->tries(lock_.tries_key);
      text = tessera::format(plugin_->text("blocked"), (static_cast<int32_t>(tr.blocked_until - now) + 999) / 1000);
      tone = Tone::BUSY;
    } else if (said_ == Said::SENT) {
      text = plugin_->text("right");
      tone = Tone::GOOD;
    } else if (said_ == Said::FAILED && now - said_at_ < 4000) {
      text = plugin_->text("failed");
      tone = Tone::ALERT;
    } else if (said_ == Said::NO_CODE && now - said_at_ < 4000) {
      text = plugin_->text("no_helper_code");
      tone = Tone::ALERT;
    } else if (bad) {
      text = plugin_->text("wrong");
      tone = Tone::ALERT;
    }
    ui::set_text(line_, text);
    ui::set_tone(line_, tone, theme::MUTED);
    for (int i = 0; i < 12; ++i) {
      const bool off = held || said_ == Said::SENT || ((i == CLEAR || i == OK) && !typed_len_);
      lv_obj_set_style_opa(keys_[i], off ? LV_OPA_40 : LV_OPA_COVER, 0);
    }
  }

  static constexpr const char *TAG = "code_lock";
  CodeLock *plugin_;
  Lock lock_;
  char typed_[MAX_DIGITS + 1] = "";
  size_t typed_len_ = 0;
  Said said_ = Said::NOTHING;
  uint32_t said_at_ = 0;
  bool beside_ = false;
  int key_w_ = 0, key_h_ = 0, gap_ = 0, x0_ = 0, y0_ = 0, dot_max_ = 0, dots_y_ = 0, info_x_ = 0, info_w_ = 0,
      text_h_ = 0;
  lv_obj_t *keys_[12]{}, *labels_[12]{}, *dots_[MAX_DIGITS]{}, *line_{};
};

void CodeLock::setup() {
  add_tile("keypad", [this]() { return new KeypadTile(this); });
  add_card("keys", [this]() { return new KeysCard(this); });
}

}  // namespace esphome::code_lock
