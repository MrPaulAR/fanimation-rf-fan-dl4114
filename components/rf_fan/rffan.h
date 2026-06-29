#pragma once

#include "esphome.h"
#include "RCSwitch.h"
#include <ELECHOUSE_CC1101_SRC_DRV.h>

#include <cstdint>
#include <functional>

namespace esphome::rf_fan {

static constexpr const char *TAG = "rf_fan";

// ---------------------------------------------------------------------------
// RF command table.
//
// Verified against the *running* sketch this replaces
// (hampton-bay-fan-mqtt/rf-fans/fanimation.cpp:53..69 and the case statement
// at :340..401).  Note that the legacy source comment block at lines 53..61
// is itself inconsistent with its own case statement (it claims the top
// light command is 0x37 when the case at :362 actually fires on 0x36 — the
// 0x37 hex is Speed I).  We use the case statement, not the comment.
// ---------------------------------------------------------------------------
namespace cmd {
static const uint8_t FAN_OFF        = 0x3d;
static const uint8_t FAN_I          = 0x37;
static const uint8_t FAN_II         = 0x35;
static const uint8_t FAN_III        = 0x2f;
static const uint8_t FAN_IV         = 0x27;
static const uint8_t FAN_V          = 0x1d;
static const uint8_t FAN_VI         = 0x1f;
static const uint8_t DOWN_LIGHT     = 0x3e;
static const uint8_t TOP_LIGHT      = 0x36;  // NOT 0x37 — see source comment above.
static const uint8_t DIRECTION      = 0x3b;
static const uint8_t DEBOUNCE_BAD   = 0x3f;  // ignored on RX
static const uint8_t SET_KEY        = 0x2d;  // decoded but unused
}  // namespace cmd

// RCSwitch protocol 13. Per the extended protocol table in the vendored
// RCSwitch.cpp at line 94, this is "M1E-N 182khz, fanimation": pulse 346us,
// sync [35,1], zero [1,2], one [2,1], inverted.
//
// NB: ESPHOME_PORT_PLAN.md claims Federigo uses protocol 11 — that is
// incorrect. The running sketch has `#define RF_PROTOCOL 13` active
// (with protocol 11 left as a commented-out hint at fanimation.cpp:16).
// Protocol 13 is what works on this hardware.
static const uint8_t RF_PROTOCOL = 13;
static const int RF_DEFAULT_REPEATS = 7;

class RFFan;
class RFLightOutput;
class RFSwitch;

// ---------------------------------------------------------------------------
// RFFan — drives the CC1101 + RCSwitch radio and owns the RF state
// machine. Inherits fan::Fan so it can also be the fan entity directly.
//
// Light entities (down light + two toggles) are exposed via helper classes
// (RFLightOutput, RFSwitch) that forward back to parent RFFan.
// ---------------------------------------------------------------------------
class RFFan : public Component, public fan::Fan {
 public:
  // ----- Component lifecycle -----
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  // ----- fan::Fan trait interface -----
  fan::FanTraits get_traits() override;
  void control(const fan::FanCall &call) override;

  // ----- Setters called from Python codegen -----
  void set_pins(InternalGPIOPin *cs, InternalGPIOPin *gdo0, InternalGPIOPin *gdo2);
  void set_rx_frequency(float f) { rx_freq_ = f; }
  void set_tx_frequency(float f) { tx_freq_ = f; }
  void set_dip_id(uint8_t id) { dip_id_ = id & 0x0F; }
  void set_fade(bool fade) { fade_ = fade; }
  void set_rx_debounce_ms(uint32_t ms) { rx_debounce_ms_ = ms; }
  void set_repeat_transmit(int n) { repeat_ = n; }
  void set_light_output(RFLightOutput *out) { light_output_ = out; }
  void set_top_light_switch(RFSwitch *s) { top_light_switch_ = s; }
  void set_direction_switch(RFSwitch *s) { direction_switch_ = s; }
  void set_rssi_sensor(sensor::Sensor *s) { rssi_sensor_ = s; }

  // ----- Public API for the helper classes (called from lambdas in
  // RFLightOutput::write_state / RFSwitch::write_state). -----
  void send_command(uint8_t rf_cmd);              // raw command byte
  void send_down_light_toggle();                  // 0x3e
  void send_top_light_toggle();                   // 0x36
  void send_direction_toggle();                   // 0x3b

  // Diagnostic: current CC1101 RSSI in dBm.  Read by the optional
  // rssi sensor so we can see whether the receiver is hearing the
  // remote at all.
  int  read_rssi_dbm();

  // Latest-known states — used by the helper classes' write_state() to
  // compute the expected post-toggle value, and by RX decode to publish.
  bool down_light_state_{false};
  bool top_light_state_{false};
  bool direction_state_{false};
  bool fan_on_{false};                // matches fans[id].fanState from fanimation.cpp
  bool fan_speed_is_set_{false};     // whether a speed command has been received
  uint8_t fan_speed_{0};             // 0 = off, 1..6 = I..VI

 protected:
  friend class RFLightOutput;
  friend class RFSwitch;

  InternalGPIOPin *cs_pin_{nullptr};
  InternalGPIOPin *gdo0_pin_{nullptr};
  InternalGPIOPin *gdo2_pin_{nullptr};
  float rx_freq_{303.870f};
  float tx_freq_{303.870f};
  uint8_t dip_id_{8};
  bool fade_{false};
  uint32_t rx_debounce_ms_{300};
  int repeat_{RF_DEFAULT_REPEATS};

  RCSwitch rf_;

  // RX debounce (mirrors fanimation.cpp:22-24,343-349).
  uint32_t last_rx_value_{0xFFFFFFFF};
  uint32_t last_rx_time_{0};

  // Tracks whether the CC1101 is currently in RX mode (true) or TX
  // mode (false).  Used by read_rssi_dbm() to only read the RSSI
  // register when the radio is actually listening.
  bool rx_state_{true};

  // Helper entity pointers.
  RFLightOutput *light_output_{nullptr};
  RFSwitch *top_light_switch_{nullptr};
  RFSwitch *direction_switch_{nullptr};
  light::LightState *light_state_{nullptr};  // set by RFLightOutput::setup_state
  sensor::Sensor *rssi_sensor_{nullptr};

  // RSSI polling state.
  uint32_t last_rssi_publish_{0};

  // Internal helpers.
  void transmit_code_(uint32_t rf_code_12bit);
  void handle_rx_();
  void publish_all_();  // publish fan + light + 2 switches to HA
  uint8_t speed_to_cmd_(uint8_t speed) const;

  // Encode the 12-bit wire code from (dip_id, fade, command) — matches
  // fanimation.cpp:70 exactly:
  //   rfCode = (!(fade) << 6) | ((~dip_id & 0x0F) << 7) | (cmd & 0x3F)
  uint32_t encode_code_(uint8_t cmd) const {
    uint32_t code = 0
                    | ((~dip_id_ & 0x0FUL) << 7)
                    | ((fade_ ? 0u : 1u) << 6)
                    | (static_cast<uint32_t>(cmd) & 0x3FUL);
    return code;  // 12-bit value (always < 0x800 due to fixed start bit)
  }

  // Decode the dip component out of a received 12-bit code. Per
  // fanimation.cpp:350: dipId = (~value >> 7) & 0x0f.  No XOR table.
  static uint8_t decode_dip_(uint32_t code) { return (~code >> 7) & 0x0F; }

  // Read the fade bit out of a received 12-bit code (informational only).
  static uint8_t decode_fade_bit_(uint32_t code) { return (code >> 6) & 0x01; }
  static uint8_t decode_cmd_(uint32_t code) { return code & 0x3F; }
};

// ---------------------------------------------------------------------------
// RFLightOutput — thin LightOutput that forwards writes to its parent RFFan.
//
// A codegen-created light::LightState wraps this object and becomes the HA
// down-light entity.  "Down light" is a toggling protocol command, so
// write_state() always sends the same 0x3e code and assumes the hardware
// toggles.
// ---------------------------------------------------------------------------
class RFLightOutput : public light::LightOutput {
 public:
  explicit RFLightOutput(RFFan *parent) : parent_(parent) {}

  light::LightTraits get_traits() override {
    light::LightTraits t;
    t.set_supported_color_modes({light::ColorMode::ON_OFF});
    return t;
  }

  // Called by LightState::setup() with its own pointer. We pass it back
  // to the parent RFFan so it can push RX-driven updates into the LightState
  // without going through write_state (which would re-trigger an RF send).
  void setup_state(light::LightState *state) override;

  void write_state(light::LightState *state) override;

 protected:
  friend class RFFan;
  RFFan *parent_;
};

// ---------------------------------------------------------------------------
// RFSwitch — tiny switch_::Switch + Component for the top-light and
// direction-reverse toggle entities.
//
// kind == 0: top light toggle  (cmd::TOP_LIGHT)
// kind == 1: direction reverse toggle  (cmd::DIRECTION)
//
// Both are "toggling" protocol commands — the same RF code is sent each
// time and the hardware flips its own state.
// ---------------------------------------------------------------------------
class RFSwitch : public switch_::Switch, public Component {
 public:
  void setup() override;
  void write_state(bool state) override;

  void set_parent(RFFan *p) { parent_ = p; }
  void set_kind(int k) { kind_ = k; }

 protected:
  RFFan *parent_{nullptr};
  int kind_{0};
};

}  // namespace esphome::rf_fan