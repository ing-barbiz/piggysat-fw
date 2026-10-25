# piggy_sat

PiggySat is an **open source satellite communicator**: open hardware and open firmware that send
short messages with a position directly over satellite, without terrestrial coverage.

To the best of our knowledge, PiggySat is the **world's first open source satellite communicator
based on NB-IoT NTN**.

It is built on **NB-IoT NTN** (3GPP Release 17), the satellite extension of the cellular IoT
standard, instead of a proprietary satellite network. The radio is a standard cellular IoT SiP
(Nordic nRF9151) running NTN modem firmware, and connectivity comes from NTN operators with
IoT-style data plans. Communicators based on proprietary systems such as Iridium need a dedicated
satellite modem and per-message airtime, which makes them considerably more expensive to build and
to run.

> **Not an emergency beacon.** PiggySat is not a certified COSPAS-SARSAT PLB/EPIRB and does not
> alert rescue services: messages go to a server you configure (ThingsBoard). Do not rely on it as
> your only means of calling for help.

**Project status:** the firmware in this repository is currently a **hardware test tool** for the
PiggySat board. The communicator application (SOS message on button press, delivery confirmation,
battery management) is not implemented yet.

## Test tool

Hardware test tool for the PiggySat board (nRF9151, NB-IoT NTN). The tests are Zephyr shell
commands over RTT (TC2030 SWD cable). Use a VT100 terminal for the shell: `west rtt`,
`JLinkRTTClient`, or a telnet client on `localhost:19021` while a J-Link session is open.
J-Link RTT Viewer works but does not render the shell's line editing.

```
piggysat:~$ help
  button   : Touch button test
  cw       : LTE CW TX test (band, frequency and power from Kconfig)
  gnss     : GNSS test (positions are not stored)
  led      : LED test
  lte      : LTE message test
  sim      : Check that a SIM card is present (ICCID, IMSI)
```

Only `help` and the test commands are built in; Zephyr's default shell commands are disabled.
Up-arrow history and Tab completion work.

| Command | What it does |
|---|---|
| `led start` / `led stop` | Cycles green, orange, red, then all on, every 0.5 s, in the background. |
| `button start` / `button stop` | Prints the touch button's current state, then every press (numbered) and release until stopped. |
| `cw start` / `cw stop` | Radio off, antenna on LTE, NTN system mode, then `AT%XRFTEST` CW in NB-IoT mode at the configured band, frequency and power. Prints the TX power the modem measured at the antenna port (returned in 1/16 dBm). Transmits until `cw stop`. |
| `gnss start` / `gnss stop` | Antenna on GNSS, modem in GPS mode, continuous tracking with GGA/GLL/GSA/GSV/RMC NMEA printed to the shell. Positions are not stored. |
| `lte send` / `lte abort` | Runs in the background: antenna on LTE, NTN setup (band lock, APN, fixed NTN location from Kconfig), attach, one confirmable CoAP `POST` to ThingsBoard with the configured location (`CONFIG_APP_NTN_LATITUDE` / `_LONGITUDE` / `_ALTITUDE_METERS`), then detach. `lte abort` cancels it while it waits for the network. |
| `sim` | Selects the NTN system mode, powers only the SIM interface (`AT+CFUN=41`, no radio) and waits up to 5 s for `%XSIM: 1`. Prints `SIM PRESENT` with PIN state, ICCID and IMSI, or `SIM NOT DETECTED` with the `%XSIM` state and the `AT+CPIN?` error (e.g. CME 10 "SIM not inserted", CME 13 "SIM failure"). |

`cw`, `gnss`, `lte` and `sim` all reconfigure the modem, so only one of them runs at a time; starting
another one while a test is active is refused ("Modem busy"). Each leaves the modem offline when it
ends.

At boot the tool checks that the modem runs `mfw_nrf9151-ntn` and logs the modem model and hardware
version. If the modem check fails, only `led` and `button` are available.

**CW test warning:** `cw start` transmits RF. Use a cabled setup or a shielded room.

See [CODE_NOTES.md](CODE_NOTES.md) for how the code is organised.

## Requirements

- PiggySat board with an `nRF9151 LACA A1` module
- modem firmware `mfw_nrf9151-ntn`
- passive GNSS antenna (the board does not power active antennas)
- nRF Connect SDK v3.4.0
- for `lte send`: a SIM and APN with NTN coverage, and a ThingsBoard device

## Configuration

All application options live in [Kconfig](Kconfig) under the `CONFIG_APP_` prefix and are set in
[prj.conf](prj.conf).

| Option | Purpose |
|---|---|
| `CONFIG_APP_CW_BAND` | `cw`: 3GPP band (default 255) |
| `CONFIG_APP_CW_FREQUENCY_100KHZ` | `cw`: frequency in 100 kHz units (default 16435 = 1643.5 MHz) |
| `CONFIG_APP_CW_POWER_DBM` | `cw`: requested power, -50 to 23 dBm |
| `CONFIG_APP_THINGSBOARD_HOST` | `lte`: ThingsBoard host name or IP |
| `CONFIG_APP_THINGSBOARD_IPV4` | `lte`: optional IPv4 address that skips DNS |
| `CONFIG_APP_THINGSBOARD_PORT` | `lte`: CoAP port, normally 5683 |
| `CONFIG_APP_THINGSBOARD_ACCESS_TOKEN` | `lte`: device access token |
| `CONFIG_APP_TELEMETRY_MESSAGE` | `lte`: text in the JSON `message` field |
| `CONFIG_APP_LTE_ATTACH_TIMEOUT_SECONDS` | `lte`: how long to wait for the network |
| `CONFIG_APP_COAP_ACK_TIMEOUT_MS` | `lte`: initial CoAP ACK wait, doubled on each retransmission |
| `CONFIG_APP_COAP_MAX_RETRANSMIT` | `lte`: CoAP retransmissions |
| `CONFIG_APP_APN` | APN for PDP context 0 |
| `CONFIG_APP_BAND_LOCK_LIST` | NTN bands to lock, e.g. `255,256` |
| `CONFIG_APP_NTN_LATITUDE` / `_LONGITUDE` / `_ALTITUDE_METERS` | Fixed location given to the modem for NTN attach |
| `CONFIG_APP_NTN_LOCATION_VALIDITY_SECONDS` | Validity passed to `ntn_location_set()` |
| `CONFIG_APP_MODEM_FW_EXPECTED` | Substring required in the modem firmware version |
| `CONFIG_APP_PSM_ENABLE` / `CONFIG_APP_EDRX_ENABLE` | Request PSM / eDRX |
| `CONFIG_APP_LOG_LEVEL` | Log level for the application modules |

The `lte send` message body is:

```json
{"message":"...","latitude":45.406400,"longitude":11.876800,"altitude":12.0}
```

## Building

The ThingsBoard access token is not committed. Copy [secrets.conf.example](secrets.conf.example) to
`secrets.conf` (git-ignored) and set `CONFIG_APP_THINGSBOARD_ACCESS_TOKEN` there; the build merges it
automatically.

The PiggySat board definition lives in [boards/piggysat/](boards/piggysat/). Sysbuild only finds it
when the project folder is passed as a board root.

### Zephyr Workbench (VS Code)

The project is set up for [Zephyr Workbench](https://marketplace.visualstudio.com/items?itemName=Ac6.zephyr-workbench)
in [.vscode/settings.json](.vscode/settings.json): west workspace `c:\ncs\v3.4.0`, Zephyr SDK 1.0.1,
and two build configurations (both `piggysat/nrf9151/ns`, sysbuild, `-DBOARD_ROOT=.`, `nrfutil`
runner with `--erase`):

| Configuration | Build directory | Extra |
|---|---|---|
| `piggysat` (active) | `build/piggysat` | none |
| `piggysat-debug` | `build/piggysat-debug` | `fatal-debug.conf` (halt instead of reset on a fatal error) |

The NCS and toolchain paths are machine-specific; adjust them in `.vscode/settings.json` if your
installation differs.

### Command line

```sh
west build -p always -b piggysat/nrf9151/ns -d build/piggysat --sysbuild -- -DBOARD_ROOT=.
west flash -d build/piggysat --erase
```

For the debug variant, add `-DEXTRA_CONF_FILE=fatal-debug.conf` after the `--`.

The board has no UART, so logs come out over RTT through the TC2030 SWD cable
(J-Link RTT Viewer, or `west rtt`).

The debug port is kept permanently open: the build merges the "unprotected" value
(`0x50FA50FA`) for `UICR.APPROTECT` and `UICR.SECUREAPPROTECT` into `tfm_merged.hex`
([boards/piggysat/uicr_debug_open.hex](boards/piggysat/uicr_debug_open.hex)), so J-Link and RTT work
after every flash, even after a full erase. Remove that step from `CMakeLists.txt` for production
firmware. Close J-Link RTT Viewer (or any other J-Link session) while flashing: an interrupted
flash leaves the chip erased with blank UICR, and it then reports "secured" until recovered.
Because the image contains UICR data, flash with a full erase (`--erase`; the Workbench
configurations pass it as runner argument), otherwise the runner refuses to overwrite UICR.

The board also enables `CONFIG_NRF91_ANOMALY_36_WORKAROUND` (TF-M and app): without it the access port
locks again every time the CPU sleeps in WFI, and J-Link reports the device as secured.

If a board still reports "secured" (e.g. it was flashed by something else), unlock it once:

```sh
nrfutil device recover
nrfutil device write --address 0x00FF8000 --value 0x50FA50FA
nrfutil device write --address 0x00FF802C --value 0x50FA50FA
```

Board-specific application settings are in
[boards/piggysat_nrf9151_ns.conf](boards/piggysat_nrf9151_ns.conf).

## PiggySat board I/O

| Function | Pad | GPIO | Devicetree | Polarity |
|---|---|---|---|---|
| Green LED (D3) | 48 | P0.29 | `led0` / `led_green` | active low |
| Orange LED (D1) | 47 | P0.28 | `led1` / `led_orange` | active low |
| Red LED (D2) | 45 | P0.27 | `led2` / `led_red` | active low |
| Touch button (TTP223) | 80 | P0.19 | `sw0` / `touch_button` | active high |
| Antenna selection switch (J3) | 12 | P0.25 | `antenna_switch` (`piggysat,antenna-switch`) | output: low = LTE, high = GNSS |

## Troubleshooting

**NTN mode fails (`Failed to set modem to NTN-only mode: -14`).**
`-14` only means `AT%XSYSTEMMODE` failed. Check the lines logged just before it:

- `Detected modem firmware` must contain `mfw_nrf9151-ntn`
- `Detected modem HW version` must be the `nRF9151 LACA A1` module
- `Raw AT%XSYSTEMMODE failure` shows the modem's actual CME error

**`cw start` returns an AT error.** `AT%XRFTEST` needs the radio off (the tool does `CFUN=0` first) and a
modem firmware that supports the TX test on the chosen band. The error type and code printed come
from the modem.

**`lte send` never attaches.** The modem needs a location close to the real one for NTN; check
`CONFIG_APP_NTN_LATITUDE` / `CONFIG_APP_NTN_LONGITUDE`.

**Attached but ThingsBoard gets nothing.**

- the host must resolve from the modem (or set `CONFIG_APP_THINGSBOARD_IPV4`)
- the port must match the ThingsBoard CoAP listener
- the access token must match the ThingsBoard device exactly
- `CoAP response code` in the log shows what the server answered; non-2.xx is a rejection

## Licensing

The firmware is licensed under the [Apache License 2.0](LICENSE), except for the files derived from
Nordic Semiconductor's nRF Connect SDK UDP sample. Each file states its licence in an
`SPDX-License-Identifier` header; the full texts are in [LICENSES/](LICENSES/).

| Licence | Files |
|---|---|
| `Apache-2.0` | `src/app_antenna.c`, `src/app_button.c`, `src/app_gnss.c`, `src/app_leds.c`, `src/app_rftest.c`, `src/app_simtest.c`, `src/app_shell.c`, `src/app_internal.h`, `boards/` (adapted from Zephyr's nRF9151 DK board), documentation |
| `LicenseRef-Nordic-5-Clause` | `src/main.c`, `src/app_modem.c`, `src/app_transport.c`, `CMakeLists.txt`, `Kconfig`, `prj.conf` (derived from the Nordic UDP sample) |

The Nordic licence only permits use with Nordic Semiconductor integrated circuits. The built firmware
also links Nordic's closed-source modem library (`nrf_modem`) and other nRF Connect SDK components
under their own licences.
