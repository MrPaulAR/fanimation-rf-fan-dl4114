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

  boot_time_ = millis();
  ESP_LOGCONFIG(TAG, "rf_fan setup complete (TX grace %u ms)", BOOT_TX_GRACE_MS);
}

void RFFan::loop() {
  if (rf_.available()) {
    handle_rx_();
    rf_.resetAvailable();
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
  // separate Switch entity — on this remote direction is a binary toggle,
  // not a settable property).  Expose 3 discrete speeds so HA's fan card
  // shows speed buttons instead of just an on/off toggle.
  fan::FanTraits traits;
  traits.set_speed(true);
  traits.set_supported_speed_count(3);
  return traits;
}

void RFFan::control(const fan::FanCall &call) {
  // Speed (1..3) has priority over plain on/off.
  if (call.get_speed().has_value()) {
    int s = *call.get_speed();
    if (s >= 1 && s <= 3) {
      send_command(speed_to_cmd_(s));
      fan_speed_ = s;
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
  }

  publish_all_();
}

uint8_t RFFan::speed_to_cmd_(uint8_t speed) const {
  // Federigo protocol codes (6-speed family per ESPHOME_PORT_PLAN.md
  // section 8).  The DL-4114 is 6-speed, but a 3-button remote typically
  // only exposes three of them.  Pair the 3-button remote's buttons to
  // HA's "low / medium / high" by jumping over the in-between codes the
  // remote never sends:
  //   1 (low)    = FAN_I  (0x37)  — Speed I   = L
  //   2 (medium) = FAN_III (0x2f) — Speed III = M
  //   3 (high)   = FAN_VI (0x1f)  — Speed VI  = H
  // The original "II" (0x35) is skipped — most 3-button remotes do not
  // transmit it, so the receiver ignores it.
  switch (speed) {
    case 1: return cmd::FAN_I;
    case 2: return cmd::FAN_III;
    case 3: return cmd::FAN_VI;
  }
  return cmd::FAN_VI;
}

// ---------------------------------------------------------------------------
// Public TX API used by helper entities
// ---------------------------------------------------------------------------

void RFFan::send_command(uint8_t rf_cmd) {
  if (millis() - boot_time_ < BOOT_TX_GRACE_MS) {
    // Suppress RF for the first few seconds after boot.  HA's API
    // re-sync routinely calls control()/write_state() with the
    // last-known state — without this gate that re-sync would emit
    // a DOWN_LIGHT toggle on the receiver and turn the physical
    // light off.  The internal state mirror is still updated by the
    // caller, so HA's view stays correct.
    ESP_LOGD(TAG, "TX suppressed (boot grace %u ms): cmd=0x%02X",
             BOOT_TX_GRACE_MS - (millis() - boot_time_), rf_cmd);
    return;
  }
  uint32_t code = encode_code_(rf_cmd);
  ESP_LOGD(TAG, "TX: cmd=0x%02X wire code=0x%03X", rf_cmd, code);
  transmit_code_(code);
}

// ---------------------------------------------------------------------------
// TX path — mirrors fanimation.cpp:40..84 of the legacy sketch exactly.
// Toggle RX off, switch CC1101 to TX on tx_freq_, send N repeats, switch back.
// ---------------------------------------------------------------------------
void RFFan::transmit_code_(uint32_t code_12bit) {
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
  this->publish_state();

  // Down light — push into the LightState's current_values directly so we
  // don't loop back into RFLightOutput::write_state() (which re-sends RF).
  if (light_state_ != nullptr) {
    light_state_->current_values.set_state(down_light_state_);
    light_state_->current_values.set_color_mode(light::ColorMode::ON_OFF);
    light_state_->publish_state();
  }

  // Top light — same pattern as the down light, but a separate LightState.
  if (top_light_state_ != nullptr) {
    top_light_state_->current_values.set_state(top_light_state_);
    top_light_state_->current_values.set_color_mode(light::ColorMode::ON_OFF);
    top_light_state_->publish_state();
  }
}

// ===========================================================================
// RFLightOutput
// ===========================================================================

void RFLightOutput::setup_state(light::LightState *state) {
  // Back-pointer to the LightState so the parent RFFan can push RX-driven
  // updates into the light without invoking this class' write_state (which
  // would re-trigger an RF transmission).
  if (kind_ == 0) {
    parent_->light_state_ = state;
  } else {
    parent_->top_light_state_ = state;
  }
}

void RFLightOutput::write_state(light::LightState *state) {
  bool want_on;
  state->current_values_as_binary(&want_on);
  // Each light is a toggling command. Emit the right code based on kind_,
  // then assume the hardware toggled to the desired state.
  parent_->send_command(kind_ == 0 ? cmd::DOWN_LIGHT : cmd::TOP_LIGHT);
  if (kind_ == 0) {
    parent_->down_light_state_ = want_on;
  } else {
    parent_->top_light_state_ = want_on;
  }
  state->publish_state();
}

// ===========================================================================
// (RFSwitch has been removed — the only toggle was direction, which the
// user's remote doesn't have and HA's switch did nothing on the fan.
// The component now exposes only the fan + 2 lights.)

}  // namespace esphome::rf_fan