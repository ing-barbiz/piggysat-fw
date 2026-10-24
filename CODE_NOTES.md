# Code Notes

The application is a hardware test tool for the PiggySat board. `main()` initialises the board and
the modem, then loops on an RTT menu. Each test runs to completion (or until `q` + Enter) on the main
thread and leaves the modem offline (`CFUN=0`) before returning to the menu.

## Source Layout

- `src/main.c`: reset-cause logging, modem init, RTT input helpers (`console_key_pressed()`,
  `console_flush_input()`), and the menu loop
- `src/app_leds.c`: LED init and test 1
- `src/app_rftest.c`: test 2, LTE CW TX with `AT%XRFTEST`
- `src/app_gnss.c`: test 3, continuous GNSS with NMEA output; also `format_timestamp()`
- `src/app_transport.c`: test 4, DNS resolution, CoAP client and the one-shot telemetry `POST`
- `src/app_modem.c`: modem firmware check, identity logging, NTN setup with the fixed Kconfig location,
  LTE event handling and link state
- `src/app_antenna.c`: J3 antenna selection switch (low = LTE, high = GNSS)
- `src/app_internal.h`: shared constants and the cross-module API

## Boot

1. log the reset cause
2. set the antenna switch to LTE and turn all LEDs off
3. initialise the modem library, check `AT+CGMR` against `CONFIG_APP_MODEM_FW_EXPECTED`, log
   `AT+CGMM` and `%HWVERSION`, start the CoAP client

If any modem step fails, the menu still runs but only test 1 is accepted.

## RTT Console

Input is read directly from RTT down-buffer 0 with `SEGGER_RTT_Read()`. Output is `printk()` on the
RTT console; logs use the RTT log backend on the same channel. RTT viewers send a line on Enter, so
the menu reads the first non-blank character and then flushes the rest of the line. During a test,
any non-blank character stops it.

## Antenna Switch

The switch only changes while the radio is off: GNSS before entering GPS mode (test 3), LTE before
entering NTN mode (tests 2 and 4).

## Test 2: CW TX

`CFUN=0`, antenna on LTE, `LTE_LC_SYSTEM_MODE_NTN_NBIOT`, then

`AT%XRFTEST=1,1,<band>,<freq_100kHz>,<power>,0,4,0,0,0,0,0,0`

(TX on, NB-IoT system mode, CW modulation; RB/tone fields are don't-care for CW). The response
`%XRFTEST: <antenna_power>` is the internally measured power in q4 dBm (divide by 16). The modem
offsets the CW tone from `<freq>` (48.75 kHz in NB-IoT mode). `AT%XRFTEST=1,0` stops it.

## Test 3: GNSS NMEA

GPS system mode, GNSS functional mode, NMEA mask GGA/GLL/GSA/GSV/RMC, fix retry 0 (no timeout), fix
interval 1 s (continuous). The GNSS event handler runs in interrupt context, so it only reads each
NMEA sentence into a message queue; the test loop prints them. Dropped sentences are counted and
reported when the test stops. Fixes are not stored anywhere.

## Test 4: LTE Message

`lte_modem_configure()` (antenna LTE, NTN mode, band lock, APN, `ntn_location_set()` with the Kconfig
location), `lte_attach_start()`, then poll `lte_link_ready()` until registration and PDN are up,
the attach timeout passes, or the user aborts. Then the host is resolved (every run), the socket
opened, and one confirmable CoAP `POST` sent to `api/v1/<token>/telemetry` with
`{"message":"...","latitude":0.0,"longitude":0.0,"altitude":0.0}`.

CoAP retransmission timing comes from `CONFIG_APP_COAP_ACK_TIMEOUT_MS` and
`CONFIG_APP_COAP_MAX_RETRANSMIT`, passed to `coap_client_req()`. The wait for the response is derived
from the same values (worst-case retransmission sequence + 10 s). Logs never print the access token.

`NTN_EVT_LOCATION_REQUEST` is answered with the same fixed Kconfig location.

## Debug Build

`fatal-debug.conf` disables reset-on-fatal-error and switches logging to immediate mode. Add it with
`west build -b piggysat/nrf9151/ns -- -DBOARD_ROOT=. -DEXTRA_CONF_FILE=fatal-debug.conf`.
