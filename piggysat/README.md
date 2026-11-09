# piggysat

Communicator firmware for the PiggySat board. Every 15 minutes it:

1. acquires a GNSS fix (single fix, up to `CONFIG_APP_GNSS_FIX_TIMEOUT_SECONDS`); without a fix the
   cycle is aborted: no connection, no message
2. attaches to the satellite network (NB-IoT NTN) and sends the position to ThingsBoard
3. detaches and sleeps with the modem off until the next cycle

**Magnet = standby.** While a magnet is on the sensor (P0.19 low) the board does nothing: modem
off, all LEDs off. A magnet that arrives during a cycle stops it within a second while it searches
for GNSS, waits for the network or sleeps; a message already being sent is completed first. Removing
the magnet starts a new cycle at once, then the periodic reporting resumes.

| LED | Behaviour |
|---|---|
| GNSS (red) | blinks while the GNSS fix is being acquired |
| LTE (orange) | blinks while the NTN connection is set up and the message is sent |
| Sent (green) | on for 30 s after the server accepted the message |

The period runs from the start of one cycle to the start of the next. A cycle can take several
minutes (in the end-to-end test: GNSS fix 27 s, NTN attach 46 s, message 47 s, detach 34 s); if it takes longer than the
period, the next cycle starts immediately.

See the [main README](../README.md) for building, flashing, the board and debug access.

## Message

A confirmable CoAP `POST` (JSON) to `api/v1/<token>/telemetry` on ThingsBoard:

```json
{"message":"piggysat","latitude":45.409442,"longitude":11.910877,"altitude":53.0,"accuracy":26.5}
```

| Field | Meaning |
|---|---|
| `message` | `CONFIG_APP_TELEMETRY_MESSAGE` |
| `latitude`, `longitude`, `altitude` | position of the fix (degrees, metres) |
| `accuracy` | horizontal accuracy estimate (metres) |

A message is only sent with a fresh fix: there is no default or last-known position.

## RTT log

Besides the cycle progress, the log (over RTT) shows:

- **while searching for GNSS**, every 3 s: satellites tracked and used, and for each satellite its
  system, ID, C/N0, elevation, azimuth and used/unhealthy flags (also once more on a timeout)
- **the fix**: position, accuracy, satellites used, time to fix and UTC time
- **once attached**, the network details from `AT%XMONITOR` and `AT+CGPADDR`: registration,
  operator, MCC/MNC, technology, band, EARFCN, cell ID, tracking area, PCI, RSRP, SNR, eDRX,
  active time, periodic TAU and IP address (fields the network does not provide show `-`)

## NTN location

NTN needs a location close to the real one to attach. The modem gets the position of the fix just
acquired in the same cycle, so the attach is never attempted without a fix.

## Configuration

Options are in [Kconfig](Kconfig) and set in [prj.conf](prj.conf); the access token comes from
`secrets.conf` at the repository root.

| Option | Default | Purpose |
|---|---|---|
| `CONFIG_APP_REPORT_INTERVAL_SECONDS` | 900 | cycle period |
| `CONFIG_APP_GNSS_FIX_TIMEOUT_SECONDS` | 180 | maximum time for the GNSS fix; no fix aborts the cycle |
| `CONFIG_APP_LTE_ATTACH_TIMEOUT_SECONDS` | 600 | maximum time for the NTN attach |
| `CONFIG_APP_SENT_LED_ON_SECONDS` | 30 | sent LED on-time |
| `CONFIG_APP_LED_BLINK_MS` | 250 | blink half period |
| `CONFIG_APP_TELEMETRY_MESSAGE` | `piggysat` | `message` field |
| `CONFIG_APP_THINGSBOARD_HOST` / `_IPV4` / `_PORT` / `_ACCESS_TOKEN` | | ThingsBoard endpoint |
| `CONFIG_APP_COAP_ACK_TIMEOUT_MS` / `_MAX_RETRANSMIT` | 60000 / 2 | CoAP retransmission timing; the server's ACK took about 30 s over the satellite in testing |
| `CONFIG_APP_APN`, `CONFIG_APP_BAND_LOCK_LIST`, `CONFIG_APP_MODEM_FW_EXPECTED` | | modem and NTN setup |
| `CONFIG_APP_NTN_LOCATION_VALIDITY_SECONDS` | 43200 | validity of the NTN location given to the modem |

## Code

| File | Role |
|---|---|
| [src/main.c](src/main.c) | the report cycle and the JSON message |
| [src/gnss.c](src/gnss.c) | single GNSS fix with timeout (antenna on GNSS, GPS system mode), satellite log |
| [src/modem.c](src/modem.c) | modem init and firmware check, NTN setup, attach and link state, NTN location, network info log |
| [src/transport.c](src/transport.c) | DNS, socket and the CoAP `POST` to ThingsBoard |
| [src/leds.c](src/leds.c) | LED blink and timed on, driven by work items |
| [src/antenna.c](src/antenna.c) | J3 antenna switch; it only changes with the radio off |
| [src/magnet.c](src/magnet.c) | magnet sensor: state, interrupt-driven wake-up for standby |
