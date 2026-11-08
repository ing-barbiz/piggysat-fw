# PiggySat

<img src="Logo/piggysat.png" alt="PiggySat logo" width="180">

[![License: Apache-2.0](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)
![Version: v1.0.0](https://img.shields.io/badge/version-v1.0.0-brightgreen.svg)
![nRF Connect SDK: v3.4.0](https://img.shields.io/badge/nRF%20Connect%20SDK-v3.4.0-00a9ce.svg)
[![Hardware: ing-barbiz/piggysat](https://img.shields.io/badge/hardware-ing--barbiz%2Fpiggysat-lightgrey.svg)](https://github.com/ing-barbiz/piggysat)

Firmware for PiggySat, the open source NB-IoT NTN satellite communicator. The project overview,
hardware and measurements are in [ing-barbiz/piggysat](https://github.com/ing-barbiz/piggysat).

This repository holds two Zephyr applications for the same board:

- **[piggysat](piggysat/)** is the communicator. Every 5 minutes it acquires a GNSS fix, attaches
  to the satellite network and sends the position to ThingsBoard over CoAP, then sleeps with the
  modem off. A magnet on the sensor puts it in standby.
- **[piggysat-test](piggysat-test/)** is the hardware test tool. It exposes shell commands over RTT
  to check the LEDs, the magnet sensor, the SIM, GNSS reception, the LTE transmitter (CW) and a
  complete satellite message.

## Repository layout

| Folder | Content |
|---|---|
| [piggysat/](piggysat/) | Communicator firmware: every 5 minutes a GNSS fix, then the position over NTN to ThingsBoard; a magnet puts it in standby |
| [piggysat-test/](piggysat-test/) | Hardware test tool: LED, magnet sensor, CW TX, GNSS, SIM and LTE message tests as shell commands over RTT |
| [boards/piggysat/](boards/piggysat/) | Zephyr board definition shared by both apps |
| `secrets.conf` | ThingsBoard access token, git-ignored, shared by both apps (see [secrets.conf.example](secrets.conf.example)) |

## Requirements

- PiggySat board with an `nRF9151 LACA A1` module
- modem firmware `mfw_nrf9151-ntn`
- passive GNSS antenna (the board does not power active antennas)
- nRF Connect SDK v3.4.0
- a SIM and APN with NTN coverage, and a ThingsBoard device

## Building

The ThingsBoard access token is not committed. Copy [secrets.conf.example](secrets.conf.example) to
`secrets.conf` at the repository root (git-ignored) and set `CONFIG_APP_THINGSBOARD_ACCESS_TOKEN`
there; both apps merge it automatically.

The board definition lives in [boards/piggysat/](boards/piggysat/), one level above the apps, so the
board root is the repository root (`-DBOARD_ROOT=..` from an app folder).

### Zephyr Workbench (VS Code)

Open [piggysat.code-workspace](piggysat.code-workspace): it contains both apps as workspace
folders. Each app has its [Zephyr Workbench](https://marketplace.visualstudio.com/items?itemName=Ac6.zephyr-workbench)
build configurations in its own `.vscode/settings.json` (west workspace `c:\ncs\v3.4.0`, Zephyr SDK
1.0.1, board `piggysat/nrf9151/ns`, sysbuild, `-DBOARD_ROOT=..`, `nrfutil` runner with `--erase`).
The NCS and SDK paths are machine-specific; adjust them if your installation differs.

### Command line

```sh
cd piggysat            # or piggysat-test
west build -p always -b piggysat/nrf9151/ns -d build/piggysat --sysbuild -- -DBOARD_ROOT=..
west flash -d build/piggysat --erase
```

The board has no UART: logs (and the test shell) come out over RTT through the TC2030 SWD cable
(J-Link RTT Viewer, `west rtt`, or `JLinkRTTClient`).

### Debug access

The debug port is kept permanently open: each app's build merges the "unprotected" value
(`0x50FA50FA`) for `UICR.APPROTECT` and `UICR.SECUREAPPROTECT` into `tfm_merged.hex`
([boards/piggysat/uicr_debug_open.hex](boards/piggysat/uicr_debug_open.hex)), so J-Link and RTT work
after every flash, even after a full erase. Remove that step from the app's `CMakeLists.txt` for
production firmware.

- Because the image contains UICR data, flash with a full erase (`--erase`; the Workbench
  configurations pass it as runner argument), otherwise the runner refuses to overwrite UICR.
- Close J-Link RTT Viewer (or any other J-Link session) while flashing: an interrupted flash leaves
  the chip erased with blank UICR, and it then reports "secured" until recovered.
- The board enables `CONFIG_NRF91_ANOMALY_36_WORKAROUND` (TF-M and app): without it the access port
  locks again every time the CPU sleeps in WFI, and J-Link reports the device as secured.

If a board still reports "secured" (e.g. it was flashed by something else), unlock it once:

```sh
nrfutil device recover
nrfutil device write --address 0x00FF8000 --value 0x50FA50FA
nrfutil device write --address 0x00FF802C --value 0x50FA50FA
```

### Flash layout

There is no bootloader. The board devicetree removes the MCUboot and update slots from the default
nRF91 layout, so TF-M starts at `0x0` and the application at `0x40000`.

## PiggySat board I/O

| Function | Pad | GPIO | Devicetree | Polarity |
|---|---|---|---|---|
| GNSS LED, red (D1) | 48 | P0.29 | `led0` / `gnss_led` | active low |
| LTE LED, orange (D2) | 47 | P0.28 | `led1` / `lte_led` | active low |
| Sent LED, green (D3) | 45 | P0.27 | `led2` / `sent_led` | active low |
| Magnet sensor (U3 output) | 80 | P0.19 | `sw0` / `magnet_sensor` | active low = magnet present |
| Antenna selection switch (J3) | 12 | P0.25 | `antenna_switch` (`piggysat,antenna-switch`) | output: low = LTE, high = GNSS |

## End-to-end test

The final test sent real positions from PiggySat to a ThingsBoard dashboard over satellite, with
no terrestrial network involved.

| Link | What was used |
|---|---|
| Device | PiggySat running the [piggysat](piggysat/) firmware |
| Satellite network | Skylo NB-IoT NTN, with the modem locked to bands 255 and 256 (`CONFIG_APP_BAND_LOCK_LIST`) |
| SIM and data plan | Monogoto SIM with NTN coverage, APN `go.mono` (`CONFIG_APP_APN`) |
| Backend | ThingsBoard Cloud, CoAP endpoint `coap.thingsboard.cloud`, port 5683 |
| Location | Padova, Italy, outdoors with about half of the sky unobstructed |

### Setup

1. **ThingsBoard:** create a device on ThingsBoard Cloud and copy its access token into
   `secrets.conf` (see [Building](#building)).
2. **SIM:** insert the Monogoto SIM. The `sim` command of [piggysat-test](piggysat-test/) confirms
   it is detected and prints its ICCID and IMSI.
3. **Firmware:** build and flash [piggysat](piggysat/) with the APN and band lock above.
4. **Placement:** put the device outdoors with as much open sky as possible. Both the GNSS fix and
   the NTN link need it; the test worked with about half of the sky unobstructed.

### What happens in each cycle

1. The modem switches the antenna to GNSS and acquires a fix (red LED blinking).
2. The modem switches the antenna to LTE, receives the fix as its NTN location, and attaches to
   the Skylo network (orange LED blinking). Once attached, the RTT log shows the operator, band,
   EARFCN, cell, RSRP, SNR and IP address from `AT%XMONITOR`.
3. The firmware sends one confirmable CoAP `POST` to `api/v1/<token>/telemetry` with the position:

   ```json
   {"message":"piggysat","latitude":45.409290,"longitude":11.910460,"altitude":48.6,"accuracy":7.3}
   ```

4. ThingsBoard acknowledges the request; the green LED stays on for 30 s and the RTT log shows the
   CoAP response code (2.xx means accepted).
5. The device detaches and sleeps with the modem off until the next cycle.

On ThingsBoard, each message appears under the device's **Latest telemetry** as `message`,
`latitude`, `longitude`, `altitude` and `accuracy`, ready to plot on a map widget.

### Result

On 7 November 2026 the ThingsBoard dashboard received three positions from the device
`piggysat-01`, at 21:27:56, 21:32:37 and 21:35:30, all sent over the Skylo satellite link. The
dashboard plots them on a route map and lists them in a time series table.

![ThingsBoard dashboard with the positions received over satellite](Pictures/thingsboard-dashboard.png)

## Troubleshooting

**NTN mode fails (`-14` from `lte_lc_system_mode_set()`).** `-14` only means `AT%XSYSTEMMODE`
failed. The modem firmware must be `mfw_nrf9151-ntn` and the module an `nRF9151 LACA A1`; the test
tool logs both at boot and prints the modem's actual error.

**Never attaches.** The modem needs a location close to the real one for NTN. `piggysat` uses the
GNSS fix of the same cycle (no fix, no attach); the test tool uses `CONFIG_APP_NTN_LATITUDE` /
`_LONGITUDE`.

**Attached but ThingsBoard gets nothing.**

- the host must resolve from the modem (or set `CONFIG_APP_THINGSBOARD_IPV4`)
- the port must match the ThingsBoard CoAP listener
- the access token must match the ThingsBoard device exactly
- the logged CoAP response code shows what the server answered; non-2.xx is a rejection

## Licensing

The firmware is licensed under the [Apache License 2.0](LICENSE), except for the files derived from
Nordic Semiconductor's nRF Connect SDK UDP sample. Each file states its licence in an
`SPDX-License-Identifier` header; the full texts are in [LICENSES/](LICENSES/).

| Licence | Files |
|---|---|
| `Apache-2.0` | `boards/` (adapted from Zephyr's nRF9151 DK board), `piggysat/` except `modem.c` and `transport.c`, `piggysat-test/src/antenna.c`, `magnet.c`, `gnss.c`, `leds.c`, `rftest.c`, `simtest.c`, `shell.c`, `app.h`, `piggysat-test/tools/`, documentation |
| `LicenseRef-Nordic-5-Clause` | `piggysat/src/modem.c`, `piggysat/src/transport.c`, `piggysat-test/src/main.c`, `modem.c`, `transport.c`, `piggysat-test/CMakeLists.txt`, `Kconfig`, `prj.conf` (derived from the Nordic UDP sample) |

The project logo in [Logo/](Logo/) is the PiggySat artwork from the
[hardware repository](https://github.com/ing-barbiz/piggysat) and follows its licence.

The Nordic licence only permits use with Nordic Semiconductor integrated circuits. The built firmware
also links Nordic's closed-source modem library (`nrf_modem`) and other nRF Connect SDK components
under their own licences.
