# rf-fan-esphome

ESPHome custom component that drives Hampton Bay / Fanimation
ceiling-fan remotes over a CC1101 sub-1 GHz transceiver. Replaces the
[hampton-bay-fan-mqtt](https://github.com/patrickdk77/hampton-bay-fan-mqtt)
PlatformIO sketch with a native ESPHome component (no MQTT, no ArduinoOTA,
no PubSubClient — just the ESPHome API).

## What it does

- Talks to a TI CC1101 module over hardware SPI.
- Encodes/decodes the 12-bit Fanimation-family RF protocol used by the
  Hampton Bay / Home Decorators Collection **Federigo** (model **SW1618MBK**)
  paired with the **DL-4114** remote (FCC ID **Y7ZDL4114T**, 303.916 MHz).
- Exposes a `fan` entity (6 preset speeds), one `light` entity (down light),
  and two `switch` entities (top light + direction reverse) to Home
  Assistant via the native ESPHome API.
- Bidirectional: pressing the physical remote updates HA, HA commands
  emit RF.

## Status

Confirmed-protocol facts (taken from the running C code this replaces):

| Item | Value |
|---|---|
| RCSwitch protocol | **13** (active `#define RF_PROTOCOL` in `fanimation.cpp:17`) |
| Bit length | 12 |
| RX center | 303.870 MHz |
| TX center | 303.870 MHz (Fanimation); other protocols at 303.631 / 304.015 MHz remain out of scope |
| Repeat count | 7 |
| Dip encoding | `(~dip_id & 0x0f) << 7` — no reversal, no XOR table |
| Fade bit (bit 6) | `1 = instant`, `0 = ramped` (matches `fanimation.cpp:70` and `:354-357`) |
| Top light command | `0x36` (NOT `0x37` — the upstream source comment at `fanimation.cpp:60` is wrong; the RX case at `:362` is correct) |

> If you are migrating from the plan `ESPHOME_PORT_PLAN.md`, please note
> that **the plan claims Federigo uses RCSwitch protocol 11** — that is
> incorrect; the running code at the time of the port uses **protocol 13**
> (with `11` left as a commented-out hint at `rf-fans/fanimation.cpp:16`).
> This component ships protocol 13 because that is what works on the
> actual hardware.

## Hardware

Wemos D1 mini (ESP8266) + CC1101 module, wired exactly as the
PlatformIO predecessor (`FanController.pdf` in the source repo):

| Function | D1 mini | ESP8266 GPIO |
|---|---|---|
| SPI SCK | D5 | GPIO14 |
| SPI MISO | D6 | GPIO12 |
| SPI MOSI | D7 | GPIO13 |
| CC1101 CS | D8 | GPIO15 |
| GDO0 (RX interrupt) | D2 | GPIO4 |
| GDO2 (TX trigger) | D1 | GPIO5 |

No rewiring is required to migrate from the existing sketch.

## Repository layout

```
.
├── components/
│   └── rf_fan/
│       ├── __init__.py          Python config schema + codegen
│       ├── rffan.h              Component class declaration
│       ├── rffan.cpp            Implementation: radio setup, encode/decode, TX/RX loop
│       ├── RCSwitch.h           Vendored verbatim from hampton-bay-fan-mqtt/rf-fans/
│       └── RCSwitch.cpp         Vendored verbatim from hampton-bay-fan-mqtt/rf-fans/
└── examples/
    └── office-ceiling-fan.yaml  Drop-in example (your base + the rf_fan block)
```

## Quick start

1. Copy `examples/office-ceiling-fan.yaml` to your device's project folder.
2. In the same folder (or next to your YAML), add:

   ```yaml
   external_components:
     - source:
         type: local
         path: components
       components: [rf_fan]
   ```

   (Or, if you host this repo on GitHub, point at the URL.)

3. Set `dip_id:` to match the 4-position dip switch on your receiver
   (0..15). Set `fade:` if you want the receiver to ramp the light
   instead of snapping it.
4. Flash with `esphome run office-ceiling-fan.yaml`.

## Configuration variables

| Key | Required | Default | Notes |
|---|---|---|---|
| `cc1101_cs_pin` | yes | — | CC1101 SPI chip-select (D8 on the reference wiring) |
| `cc1101_gdo0_pin` | yes | — | CC1101 GDO0 — RX interrupt pin (D2) |
| `cc1101_gdo2_pin` | yes | — | CC1101 GDO2 — TX trigger pin (D1) |
| `dip_id` | yes | — | 4-bit dip switch ID, 0..15 |
| `rx_frequency` | no | `303.870` | MHz, 300..464 |
| `tx_frequency` | no | `303.870` | MHz, 300..464 |
| `fade` | no | `false` | `true` = ramped light, `false` = instant |
| `rx_debounce_ms` | no | `300` | Suppress duplicate codes within this window |
| `repeat_transmit` | no | `7` | RF transmit repeat count |

Entity names are configured under nested `fan:`, `light:`, `top_light:`,
and `direction:` blocks.

## License

The vendored `RCSwitch.{h,cpp}` are licensed under the GNU LGPL (see the
headers in those files). The rest of this repository follows the upstream
[hampton-bay-fan-mqtt](https://github.com/patrickdk77/hampton-bay-fan-mqtt)
convention.

## Credits

- [patrickdk77/hampton-bay-fan-mqtt](https://github.com/patrickdk77/hampton-bay-fan-mqtt)
  — the original reverse-engineering + working sketch this replaces.
- [sui77/rc-switch](https://github.com/sui77/rc-switch) — RCSwitch
  library, with extended protocols 11/12/13/14 contributed via the
  SmartRC-CC1101-Driver-Lib fork.
- [LSatan/SmartRC-CC1101-Driver-Lib](https://github.com/LSatan/SmartRC-CC1101-Driver-Lib)
  — Arduino CC1101 driver that exposes the RCSwitch protocol set we need.