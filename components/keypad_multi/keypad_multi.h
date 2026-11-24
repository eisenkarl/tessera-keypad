#pragma once
#include "esphome/core/component.h"
#include "esphome/components/smart_display/plugin_api.h"

namespace esphome::keypad_multi {

static constexpr size_t MAX_ENTRIES = 8;

// One code and the entity that code switches.
struct Entry {
  char code[17] = "";
  char entity[96] = "";
};

// What a tile hands the keypad it opens: one or more code→entity pairs. The card is made after the tile's tap and
// reads it from the plugin, as it has no options of its own.
struct Lock {
  Entry entries[MAX_ENTRIES];
  uint8_t count = 0;          // how many entries are filled
  bool from_entity = false;   // entries[0].code comes from a helper in Home Assistant
  int attempts = 3;           // wrong codes in a row before the keypad waits
  bool auto_check = true;     // check the code as its last digit comes, without OK
  bool free_close = false;    // lock/close/switch off entries[0] with a tap; code only to open
  int wait_s = 30;            // how long it waits
  char tries_key[96] = "";    // key for wrong-try tracking (first entity)
};

// The plugin: a tile that shows the first entity and opens the keypad, and the keypad card. It keeps the wrong tries
// per tile across the card's openings, so closing and opening the keypad does not reset the wait.
class KeypadMulti : public Component, public tessera::Plugin {
 public:
  void setup() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  Lock chosen;

  // Wrong tries of one keypad (the last few tiles seen; a screen has only a handful of keypads).
  struct Tries {
    char entity[96] = "";
    int wrong = 0;
    uint32_t blocked_until = 0;  // millis(), 0 for not blocked
  };
  Tries &tries(const char *entity);

 private:
  Tries tries_[4];
  uint8_t next_ = 0;
};

}  // namespace esphome::keypad_multi
