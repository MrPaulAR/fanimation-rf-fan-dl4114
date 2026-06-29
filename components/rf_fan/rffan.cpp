#include "rffan.h"

namespace esphome::rf_fan {

// ===========================================================================
// RFFan
// ===========================================================================

void RFFan::set_pins(InternalGPIOPin *cs, InternalGPIOPin *gdo0, InternalGPIOPin *gdo2) {
  cs_pin_ = cs;
  gdo0_pin_ = gdo0;
  gdo2_pin_ = gdo2;
}

void RFFan::setup() {
  // Hardware SPI pins are the standard D5/D6/D7 on the D1 mini. `setSpiPin`
  // lets the SmartRC library use the same hardware-SPI pins the legacy
  // sketch used. No ESPHome `spi:` block is required because ELECHOUSE
  // initializes SPI itself — just like the legacy sketch.
  ELECHOUSE_cc1101.setSpiPin(SCK, MISO, MOSI, cs_pin_->get_pin());
  ELECHOUSE_cc1101.setGDO(gdo0_pin_->get_pin(), gdo2_pin_->get_pin());
  ELECHOUSE_cc1101.Init();
  ELECHOUSE_cc1101.setMHZ(rx_freq_);
  ELECHOUSE_cc1101.SetRx();

  rf_.disableTransmit();
  rf_.setProtocol(RF_PROTOCOL);
  rf_.setRepeatTransmit(repeat_);
  rf_.enableReceive(gdo0_pin_->get_pin());

  // fan::Fan public fields — conservative defaults.
  this->state = false;
  this->speed = 0;
  this->oscillating = false;

  ESP_LOGCONFIG(TAG, "rf_fan setup complete");
}

void RFFan::loop() {
  if (rf_.available()) {
    handle_rx_();
    rf_.resetAvailable();
  }

  // Poll the CC1101 RSSI every ~250 ms so the diagnostic sensor tracks
  // ambient noise + any incoming signal.  Only emits when an RSSI
  // sensor was registered in YAML.
  if (rssi_sensor_ != nullptr) {
    uint32_t now = millis();
    if (now - last_rssi_publish_ > 250) {
      last_rssi_publish_ = now;
      rssi_sensor_->publish_state(read_rssi_dbm());
    }
  }
}

void RFFan::dump_config() {
  ESP_LOGCONFIG(TAG, "RFFan radio:");
  ESP_LOGCONFIG(TAG, "  RX freq:    %.3f MHz", rx_freq_);
  ESP_LOGCONFIG(TAG, "  TX freq:    %.3f MHz", tx_freq_);
  ESP_LOGCONFIG(TAG, "  dip_id:     0x%X (encode: (~id & 0x0F) << 7)", dip_id_);
  ESP_LOGCONFIG(TAG, "  fade:       %s (bit 6: 1=instant, 0=ramped)",
                fade_ ? "ramped" : "instant");
  ESP_LOGCONFIG(TAG, "  RCSwitch:   protocol=%u, repeats=%d", RF_PROTOCOL, repeat_);
  ESP_LOGCONFIG(TAG, "  top-light cmd = 0x%02X (see README: launcher comment",
                cmd::TOP_LIGHT);
  if (cs_pin_)
    ESP_LOGCONFIG(TAG, "  CS   pin: GPIO%d", cs_pin_->get_pin());
  if (gdo0_pin_)
    ESP_LOGCONFIG(TAG, "  GDO0 pin: GPIO%d (RX interrupt)", gdo0_pin_->get_pin());
  if (gdo2_pin_)
    ESP_LOGCONFIG(TAG, "  GDO2 pin: GPIO%d (TX trigger)", gdo2_pin_->get_pin());
}

fan::FanTraits RFFan::get_traits() {
  // No oscillation; no continuous speed field; no direction (direction is a
  // separate Switch entity — on this remote direction is binary toggle, not a
  // settable property).  The DL-4114 (and other Fanimation-family remotes
  // paired with 3-speed receivers) only has 3 buttons on the physical
  // remote, so we expose 3 preset modes to HA.
  this->set_supported_preset_modes({"I", "II", "III"});
  return fan::FanTraits();
}

void RFFan::control(const fan::FanCall &call) {
  // Preset mode has priority over plain speed/on-off.
  if (call.has_preset_mode()) {
    const char *preset = call.get_preset_mode();
    uint8_t cmd = 0;
    uint8_t new_speed = 0;
    if      (strcmp(preset, "I") == 0)  { cmd = cmd::FAN_I;   new_speed = 1; }
    else if (strcmp(preset, "II") == 0) { cmd = cmd::FAN_II;  new_speed = 2; }
    else if (strcmp(preset, "III") == 0){ cmd = cmd::FAN_III; new_speed = 3; }
    if (cmd != 0) {
      send_command(cmd);
      fan_speed_ = new_speed;
      fan_speed_is_set_ = true;
      fan_on_ = true;
    }
  } else if (call.get_state().has_value()) {
    bool on = *call.get_state();
    if (on) {
      // Turn on. Re-emit the last known speed (or fall back to Speed III).
      uint8_t cmd = (fan_speed_ > 0) ? speed_to_cmd_(fan_speed_) : cmd::FAN_III;
      if (fan_speed_ == 0) fan_speed_ = 3;
      send_command(cmd);
      fan_on_ = true;
    } else {
      send_command(cmd::FAN_OFF);
      fan_on_ = false;
    }
  } else if (call.get_speed().has_value()) {
    int s = *call.get_speed();
    if (s >= 1 && s <= 3) {
      send_command(speed_to_cmd_(s));
      fan_speed_ = s;
      fan_speed_is_set_ = true;
      fan_on_ = true;
    }
  }

  publish_all_();
}

uint8_t RFFan::speed_to_cmd_(uint8_t speed) const {
  switch (speed) {
    case 1: return cmd::FAN_I;
    case 2: return cmd::FAN_II;
    case 3: return cmd::FAN_III;
  }
  return cmd::FAN_III;
}

// ---------------------------------------------------------------------------
// Public TX API used by helper entities
// ---------------------------------------------------------------------------

int RFFan::read_rssi_dbm() {
  // ELECHOUSE_cc1101.getRssi() returns the raw CC1101 RSSI register
  // value (0..255).  The datasheet's two's-complement signed RSSI
  // formula is:  dBm = (RSSI_raw / 2) - RSSI_OFFSET  with
  // RSSI_OFFSET = 74 for 433 MHz / 868 MHz / 915 MHz; for 303 MHz the
  // value is approximately the same in practice.  We just expose the
  // raw register value here — what matters for the diagnostic is
  // "does it change when I press the remote?".
  if (this->rx_state_) {
    return static_cast<int>(ELECHOUSE_cc1101.getRssi());
  }
  return 0;
}

void RFFan::send_command(uint8_t rf_cmd) {
  uint32_t code = encode_code_(rf_cmd);
  ESP_LOGD(TAG, "TX: cmd=0x%02X wire code=0x%03X", rf_cmd, code);
  transmit_code_(code);
}

void RFFan::send_down_light_toggle() {
  send_command(cmd::DOWN_LIGHT);
  down_light_state_ = !down_light_state_;
}

void RFFan::send_top_light_toggle() {
  send_command(cmd::TOP_LIGHT);
  top_light_state_ = !top_light_state_;
}

void RFFan::send_direction_toggle() {
  send_command(cmd::DIRECTION);
  direction_state_ = !direction_state_;
}

// ---------------------------------------------------------------------------
// TX path — mirrors fanimation.cpp:40..84 of the legacy sketch exactly.
// Toggle RX off, switch CC1101 to TX on tx_freq_, send N repeats, switch back.
// ---------------------------------------------------------------------------
void RFFan::transmit_code_(uint32_t code_12bit) {
  rx_state_ = false;
  rf_.disableReceive();
  ELECHOUSE_cc1101.setMHZ(tx_freq_);
  ELECHOUSE_cc1101.SetTx();
  rf_.enableTransmit(gdo2_pin_->get_pin());
  rf_.setRepeatTransmit(repeat_);
  rf_.setProtocol(RF_PROTOCOL);
  rf_.send(code_12bit, 12);
  rf_.disableTransmit();

  ELECHOUSE_cc1101.setMHZ(rx_freq_);
  ELECHOUSE_cc1101.SetRx();
  rf_.enableReceive(gdo0_pin_->get_pin());
  rx_state_ = true;
  yield();
}

// ---------------------------------------------------------------------------
// RX path. Mirrors fanimation.cpp:340..403 of the legacy sketch.
// ---------------------------------------------------------------------------
void RFFan::handle_rx_() {
  uint32_t value = rf_.getReceivedValue();
  unsigned proto  = rf_.getReceivedProtocol();
  unsigned bits   = rf_.getReceivedBitlength();
  ESP_LOGD(TAG, "RX: proto=%u bits=%u value=0x%03X", proto, bits, value);

  if (!(proto >= 11 && proto <= 14 && bits == 12 && (value & 0x800) == 0x000)) {
    return;
  }

  uint8_t rx_dip = decode_dip_(value);
  if (rx_dip != dip_id_) {
    return;
  }

  uint32_t now = millis();
  if (value == last_rx_value_ && (now - last_rx_time_) < rx_debounce_ms_) {
    return;
  }
  last_rx_value_ = value;
  last_rx_time_ = now;

  // Fade bit is informational only (not exposed as an entity in v1).
  (void) decode_fade_bit_(value);

  uint8_t cmd = decode_cmd_(value);
  switch (cmd) {
    case cmd::DIRECTION:    direction_state_ = !direction_state_;     break;
    case cmd::TOP_LIGHT:    top_light_state_ = !top_light_state_;     break;
    case cmd::DOWN_LIGHT:   down_light_state_ = !down_light_state_;   break;
    case cmd::FAN_I:         fan_speed_ = 1; fan_speed_is_set_ = true; fan_on_ = true; break;
    case cmd::FAN_II:        fan_speed_ = 2; fan_speed_is_set_ = true; fan_on_ = true; break;
    case cmd::FAN_III:       fan_speed_ = 3; fan_speed_is_set_ = true; fan_on_ = true; break;
    case cmd::FAN_OFF:      fan_on_ = false;                            break;
    case cmd::SET_KEY:                                                  break;
    case cmd::DEBOUNCE_BAD:                                            break;
    case cmd::FAN_IV:
    case cmd::FAN_V:
    case cmd::FAN_VI:
    default:                                                            break;
  }

  publish_all_();
}

// ---------------------------------------------------------------------------
// Publishes our tracked state out to all four entities (fan + down light +
// top light switch + direction switch).
//
// For the fan, we set canonical fields and call publish_state().
// For the down light, we update LightState's current_values directly
// (bypassing write_state() so we don't re-trigger an RF transmission).
// For switches, we call publish_state() — switch_::Switch::publish_state
// only publishes a fresh value to HA (no back-call to our write_state).
// ---------------------------------------------------------------------------
void RFFan::publish_all_() {
  // Fan entity
  this->state = fan_on_;
  this->speed = fan_on_ ? fan_speed_ : 0;
  // Map current speed to preset string for HA.  Speed 0 (off) → no preset.
  const char *preset = nullptr;
  switch (fan_speed_) {
    case 1: preset = "I";    break;
    case 2: preset = "II";   break;
    case 3: preset = "III";  break;
  }
  if (preset != nullptr && fan_on_) {
    this->set_preset_mode_(preset, strlen(preset));
  } else if (!fan_on_) {
    this->clear_preset_mode_();
  }
  this->publish_state();

  // Down light — push into the LightState's current_values directly so we
  // don't loop back into RFLightOutput::write_state() (which re-sends RF).
  if (light_state_ != nullptr) {
    light_state_->current_values.set_state(down_light_state_);
    light_state_->current_values.set_color_mode(light::ColorMode::ON_OFF);
    light_state_->publish_state();
  }

  // Switches
  if (top_light_switch_ != nullptr)
    top_light_switch_->publish_state(top_light_state_);
  if (direction_switch_ != nullptr)
    direction_switch_->publish_state(direction_state_);
}

// ===========================================================================
// RFLightOutput
// ===========================================================================

void RFLightOutput::setup_state(light::LightState *state) {
  // Back-pointer to the LightState so the parent RFFan can push RX-driven
  // updates into the light without invoking this class' write_state (which
  // would re-trigger an RF transmission).
  parent_->light_state_ = state;
}

void RFLightOutput::write_state(light::LightState *state) {
  bool want_on;
  state->current_values_as_binary(&want_on);
  // Down-light is a toggling command. Emit the code, then assume the
  // hardware toggled to the desired state.
  parent_->send_command(cmd::DOWN_LIGHT);
  parent_->down_light_state_ = want_on;
  state->publish_state();
}

// ===========================================================================
// RFSwitch
// ===========================================================================

void RFSwitch::setup() {
  if (parent_ == nullptr) return;
  publish_state(kind_ == 0 ? parent_->top_light_state_ : parent_->direction_state_);
}

void RFSwitch::write_state(bool state) {
  if (parent_ == nullptr) {
    publish_state(false);
    return;
  }
  bool current = (kind_ == 0) ? parent_->top_light_state_ : parent_->direction_state_;
  if (state != current) {
    if (kind_ == 0)
      parent_->send_top_light_toggle();
    else
      parent_->send_direction_toggle();
    publish_state(state);
  } else {
    publish_state(current);
  }
}

}  // namespace esphome::rf_fan