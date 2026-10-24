# piggy_sat

Hardware test tool for the PiggySat board (nRF9151, NB-IoT NTN). It runs a menu over RTT
(TC2030 SWD cable, J-Link RTT Viewer or `west rtt`):

```
 1 - LED test
 2 - LTE CW TX test (band 255, 1643.5 MHz, 23 dBm)
 3 - GNSS mode (continuous reception, NMEA output)
 4 - LTE message send (position 0,0)
```

Type the number and press Enter. Continuous tests stop on `q` + Enter.

| Test | What it does |
|---|---|
| 1 - LED test | Cycles green, orange, red, then all on, every 0.5 s |
| 2 - LTE CW TX test | Radio off, antenna on LTE, NTN system mode, then `AT%XRFTEST` CW in NB-IoT mode at the configured band, frequency and power. Prints the TX power the modem measured at the antenna port (returned in 1/16 dBm). Transmits until stopped. |
| 3 - GNSS mode | Antenna on GNSS, modem in GPS mode, continuous tracking with GGA/GLL/GSA/GSV/RMC NMEA printed to RTT. Positions are not stored. |
| 4 - LTE message send | Antenna on LTE, NTN setup (band lock, APN, fixed NTN location from Kconfig), attach, one confirmable CoAP `POST` to ThingsBoard with position 0,0, then detach. Abort with `q` while waiting for the network. |

At boot the tool checks that the modem runs `mfw_nrf9151-ntn` and logs the modem model and hardware
version. If the modem check fails, only the LED test is available.

**CW test warning:** test 2 transmits RF. Use a cabled setup or a shielded room.

See [CODE_NOTES.md](CODE_NOTES.md) for how the code is organised.

## Requirements

- PiggySat board with an `nRF9151 LACA A1` module
- modem firmware `mfw_nrf9151-ntn`
- passive GNSS antenna (the board does not power active antennas)
- nRF Connect SDK v3.3.0
- for test 4: a SIM and APN with NTN coverage, and a ThingsBoard device

## Configuration

All application options live in [Kconfig](Kconfig) under the `CONFIG_APP_` prefix and are set in
[prj.conf](prj.conf).

| Option | Purpose |
|---|---|
| `CONFIG_APP_CW_BAND` | Test 2: 3GPP band (default 255) |
| `CONFIG_APP_CW_FREQUENCY_100KHZ` | Test 2: frequency in 100 kHz units (default 16435 = 1643.5 MHz) |
| `CONFIG_APP_CW_POWER_DBM` | Test 2: requested power, -50 to 23 dBm |
| `CONFIG_APP_THINGSBOARD_HOST` | Test 4: ThingsBoard host name or IP |
| `CONFIG_APP_THINGSBOARD_IPV4` | Test 4: optional IPv4 address that skips DNS |
| `CONFIG_APP_THINGSBOARD_PORT` | Test 4: CoAP port, normally 5683 |
| `CONFIG_APP_THINGSBOARD_ACCESS_TOKEN` | Test 4: device access token |
| `CONFIG_APP_TELEMETRY_MESSAGE` | Test 4: text in the JSON `message` field |
| `CONFIG_APP_LTE_ATTACH_TIMEOUT_SECONDS` | Test 4: how long to wait for the network |
| `CONFIG_APP_COAP_ACK_TIMEOUT_MS` | Test 4: initial CoAP ACK wait, doubled on each retransmission |
| `CONFIG_APP_COAP_MAX_RETRANSMIT` | Test 4: CoAP retransmissions |
| `CONFIG_APP_APN` | APN for PDP context 0 |
| `CONFIG_APP_BAND_LOCK_LIST` | NTN bands to lock, e.g. `255,256` |
| `CONFIG_APP_NTN_LATITUDE` / `_LONGITUDE` / `_ALTITUDE_METERS` | Fixed location given to the modem for NTN attach |
| `CONFIG_APP_NTN_LOCATION_VALIDITY_SECONDS` | Validity passed to `ntn_location_set()` |
| `CONFIG_APP_MODEM_FW_EXPECTED` | Substring required in the modem firmware version |
| `CONFIG_APP_PSM_ENABLE` / `CONFIG_APP_EDRX_ENABLE` | Request PSM / eDRX |
| `CONFIG_APP_LOG_LEVEL` | Log level for the application modules |

The test 4 message body is:

```json
{"message":"...","latitude":0.0,"longitude":0.0,"altitude":0.0}
```

## Building

The ThingsBoard access token is not committed. Copy [secrets.conf.example](secrets.conf.example) to
`secrets.conf` (git-ignored) and set `CONFIG_APP_THINGSBOARD_ACCESS_TOKEN` there; the build merges it
automatically.

The PiggySat board definition lives in [boards/piggysat/](boards/piggysat/). Sysbuild only finds it
when the project folder is passed as a board root (the nRF Connect VS Code extension does this
through `nrf-connect.boardRoots` in `.vscode/settings.json`):

```sh
west build -p always -b piggysat/nrf9151/ns -- -DBOARD_ROOT=.
west flash
```

The board has no UART, so logs come out over RTT through the TC2030 SWD cable
(J-Link RTT Viewer, or `west rtt`).

Board-specific application settings are in
[boards/piggysat_nrf9151_ns.conf](boards/piggysat_nrf9151_ns.conf). For debugging fatal errors (no reset on
fault, immediate logging), add `-DEXTRA_CONF_FILE=fatal-debug.conf` after the `--`.

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

**Test 2 returns an AT error.** `AT%XRFTEST` needs the radio off (the tool does `CFUN=0` first) and a
modem firmware that supports the TX test on the chosen band. The error type and code printed come
from the modem.

**Test 4 never attaches.** The modem needs a location close to the real one for NTN; check
`CONFIG_APP_NTN_LATITUDE` / `CONFIG_APP_NTN_LONGITUDE`.

**Attached but ThingsBoard gets nothing.**

- the host must resolve from the modem (or set `CONFIG_APP_THINGSBOARD_IPV4`)
- the port must match the ThingsBoard CoAP listener
- the access token must match the ThingsBoard device exactly
- `CoAP response code` in the log shows what the server answered; non-2.xx is a rejection
