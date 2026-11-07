# piggysat-test

Hardware test tool for the PiggySat board (nRF9151, NB-IoT NTN). The tests are Zephyr shell
commands over RTT (TC2030 SWD cable). Use a VT100 terminal for the shell: `west rtt`,
`JLinkRTTClient`, or a telnet client on `localhost:19021` while a J-Link session is open.
J-Link RTT Viewer works but does not render the shell's line editing.

```
piggysat:~$ help
  magnet   : Magnet sensor test
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
| `led start` / `led stop` | Cycles the GNSS (green), LTE (orange) and Sent (red) LEDs, then all on, every 0.5 s, in the background. |
| `magnet start` / `magnet stop` | Prints whether a magnet is on the sensor, then every detection (numbered) and removal until stopped. |
| `cw start` / `cw stop` | Radio off, antenna on LTE, NTN system mode, then `AT%XRFTEST` CW in NB-IoT mode at the configured band, frequency and power. Prints the TX power the modem measured at the antenna port (returned in 1/16 dBm). Transmits until `cw stop`. |
| `gnss start` / `gnss stop` | Antenna on GNSS, modem in GPS mode, continuous tracking with GGA/GLL/GSA/GSV/RMC NMEA printed to the shell. Positions are not stored. |
| `lte send` / `lte abort` | Runs in the background: antenna on LTE, NTN setup (band lock, APN, fixed NTN location from Kconfig), attach, one confirmable CoAP `POST` to ThingsBoard with the configured location (`CONFIG_APP_NTN_LATITUDE` / `_LONGITUDE` / `_ALTITUDE_METERS`), then detach. `lte abort` cancels it while it waits for the network. |
| `sim` | Selects the NTN system mode, powers only the SIM interface (`AT+CFUN=41`, no radio) and waits up to 5 s for `%XSIM: 1`. Prints `SIM PRESENT` with PIN state, ICCID and IMSI, or `SIM NOT DETECTED` with the `%XSIM` state and the `AT+CPIN?` error (e.g. CME 10 "SIM not inserted", CME 13 "SIM failure"). |

`cw`, `gnss`, `lte` and `sim` all reconfigure the modem, so only one of them runs at a time; starting
another one while a test is active is refused ("Modem busy"). Each leaves the modem offline when it
ends.

At boot the tool checks that the modem runs `mfw_nrf9151-ntn` and logs the modem model and hardware
version. If the modem check fails, only `led` and `magnet` are available.

### GNSS plotter

[tools/gnss_plotter.py](tools/gnss_plotter.py) connects to the board through the J-Link (pylink),
sends `gnss start` over RTT and plots the NMEA output live: C/N0 per satellite (green = used in the
fix), a sky plot, satellites tracked/used and C/N0 over time, and the fix data (UTC, position,
altitude, HDOP/PDOP/VDOP, speed, course). Closing the window sends `gnss stop`.

```sh
pip install -r tools/requirements.txt
python tools/gnss_plotter.py            # --serial <J-Link SN>, --log nmea.txt, --echo
```

Close J-Link RTT Viewer and any other RTT client first; two RTT readers steal each other's data.

**CW test warning:** `cw start` transmits RF. Use a cabled setup or a shielded room.

See [CODE_NOTES.md](CODE_NOTES.md) for how the code is organised, and the [main README](../README.md) for
building, flashing and the board.

## Configuration

All test options live in [Kconfig](Kconfig) under the `CONFIG_APP_` prefix and are set in
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

## Troubleshooting

**`cw start` returns an AT error.** `AT%XRFTEST` needs the radio off (the tool does `CFUN=0` first) and a
modem firmware that supports the TX test on the chosen band. The error type and code printed come
from the modem.

**`lte send` never attaches.** The modem needs a location close to the real one for NTN; check
`CONFIG_APP_NTN_LATITUDE` / `CONFIG_APP_NTN_LONGITUDE`.

## Debug build

The `piggysat-debug` Workbench configuration adds [fatal-debug.conf](fatal-debug.conf): the device
halts instead of resetting on a fatal error, so the fault log stays readable.
