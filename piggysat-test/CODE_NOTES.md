# Code Notes

The application is a hardware test tool for the PiggySat board. `main()` initialises the board and
the modem and returns; the tests are Zephyr shell commands over RTT (`src/shell.c`). Continuous
tests are `start`/`stop` pairs so the shell stays responsive, and every modem test leaves the modem
offline (`CFUN=0`) when it ends.

## Source Layout

- `src/main.c`: reset-cause logging, board and modem init
- `src/shell.c`: shell commands `led`, `magnet`, `cw`, `gnss`, `lte`, `sim`, and the modem lock
- `src/leds.c`: LED init and the background LED cycle
- `src/magnet.c`: `magnet`, magnet sensor detection/removal reporting
- `src/rftest.c`: `cw`, LTE CW TX with `AT%XRFTEST`
- `src/gnss.c`: `gnss`, continuous GNSS with NMEA output; also `format_timestamp()`
- `src/simtest.c`: `sim`, SIM card presence check
- `src/transport.c`: `lte`, DNS resolution, CoAP client and the one-shot telemetry `POST`
- `src/modem.c`: modem firmware check, identity logging, NTN setup with the fixed Kconfig location,
  LTE event handling and link state
- `src/antenna.c`: J3 antenna selection switch (low = LTE, high = GNSS)
- `src/app.h`: shared constants and the cross-module API

## Boot

1. log the reset cause
2. set the antenna switch to LTE and turn all LEDs off, start the `lte` work queue
3. initialise the modem library, check `AT+CGMR` against `CONFIG_APP_MODEM_FW_EXPECTED`, log
   `AT+CGMM` and `%HWVERSION`, start the CoAP client

If any modem step fails, only `led` and `magnet` are accepted.

## Shell

The RTT shell backend (`CONFIG_SHELL_BACKEND_RTT`, board conf) uses RTT channel 0; logs are printed
through the shell (`CONFIG_SHELL_LOG_BACKEND`), the separate RTT log backend is off. Zephyr's default
shell commands (`SHELL_CMDS`, `date`, `device`, `devmem`, `kernel`) are disabled. The NCS `lte`
link-control shell is disabled (`CONFIG_LTE_SHELL=n`) because it clashes with the test tool's `lte`.
The RTT shell backend does not support immediate log mode, so `fatal-debug.conf` only disables
reset-on-fatal-error.

Shell commands run in the shell thread, so nothing may block there for long:

- `led`: a delayable work item steps the LEDs every 500 ms
- `magnet`: a both-edges GPIO interrupt submits a work item that reads the pin and prints state
  changes
- `cw`: start configures the modem and turns `%XRFTEST` on; stop turns it off
- `gnss`: the GNSS event handler (interrupt context) queues NMEA sentences and submits a work item
  that prints them with `shell_print()` to the shell that started the test
- `lte send`: runs on a dedicated work queue; `lte abort` sets a flag checked while waiting for the
  network; the job releases the modem lock when it ends
- `sim`: short (up to about 5 s), runs directly in the shell thread

`cw`, `gnss`, `lte` and `sim` take a modem lock (`modem_test_claim()` / `modem_test_release()`), so a
second modem test is refused while one is active.

## Antenna Switch

The switch only changes while the radio is off: GNSS before entering GPS mode (`gnss`), LTE before
entering NTN mode (`cw`, `lte`).

## `cw`: CW TX

`CFUN=0`, antenna on LTE, `LTE_LC_SYSTEM_MODE_NTN_NBIOT`, then

`AT%XRFTEST=1,1,<band>,<freq_100kHz>,<power>,0,4,0,0,0,0,0,0`

(TX on, NB-IoT system mode, CW modulation; RB/tone fields are don't-care for CW). The response
`%XRFTEST: <antenna_power>` is the internally measured power in q4 dBm (divide by 16). The modem
offsets the CW tone from `<freq>` (48.75 kHz in NB-IoT mode). `AT%XRFTEST=1,0` stops it.

## `gnss`: GNSS NMEA

GPS system mode, GNSS functional mode, NMEA mask GGA/GLL/GSA/GSV/RMC, fix retry 0 (no timeout), fix
interval 1 s (continuous). The GNSS event handler runs in interrupt context, so it only reads each
NMEA sentence into a message queue; a work item prints them. Dropped sentences are counted and
reported when the test stops. Fixes are not stored anywhere.

## `sim`: SIM Check

`CFUN=0`, NTN system mode (the UICC cannot be activated in GPS-only mode, which `gnss` leaves
behind), `AT+CMEE=1`, then `AT+CFUN=41` (UICC on, radio off). `AT%XSIM?` is polled every 250 ms for up to 5 s;
state 1 means the SIM is initialised. On success the PIN state (`AT+CPIN?`), ICCID (`AT%XICCID`) and
IMSI (`AT+CIMI`) are printed. Otherwise `AT%XSIM?` and `AT+CPIN?` are printed, the latter with its CME error (10 = SIM not
inserted, 13 = SIM failure). The modem returns to `CFUN=0` afterwards.

## `lte`: LTE Message

`lte_modem_configure()` (antenna LTE, NTN mode, band lock, APN, `ntn_location_set()` with the Kconfig
location), `lte_attach_start()`, then poll `lte_link_ready()` until registration and PDN are up,
the attach timeout passes, or `lte abort` is used. Then the host is resolved (every run), the socket
opened, and one confirmable CoAP `POST` sent to `api/v1/<token>/telemetry` with
`{"message":"...","latitude":<lat>,"longitude":<lon>,"altitude":<alt>}`, using the same fixed Kconfig
location that is given to the modem for NTN.

CoAP retransmission timing comes from `CONFIG_APP_COAP_ACK_TIMEOUT_MS` and
`CONFIG_APP_COAP_MAX_RETRANSMIT`, passed to `coap_client_req()`. The wait for the response is derived
from the same values (worst-case retransmission sequence + 10 s). Logs never print the access token.

`NTN_EVT_LOCATION_REQUEST` is answered with the same fixed Kconfig location.

## Flash Layout and Debug Access

There is no bootloader. The board devicetree removes the MCUboot and update slots from the default
nRF91 layout, so TF-M starts at `0x0` and the application at `0x40000` (NCS v3.4 uses devicetree
partitioning; without this the reset vector would be empty).

The debug port stays open: `CMakeLists.txt` merges `boards/piggysat/uicr_debug_open.hex`
(`UICR.APPROTECT` / `UICR.SECUREAPPROTECT` = `0x50FA50FA`) into `tfm_merged.hex` after the final link.
With the default `NRF_APPROTECT_USE_UICR`, the firmware opens both access ports at boot.
`CONFIG_NRF91_ANOMALY_36_WORKAROUND` (board defconfig, also passed to TF-M) is required: without it the
access port locks again whenever the CPU sleeps in WFI. Flash with `--erase`, since the image writes UICR.

## Debug Build

`fatal-debug.conf` disables reset-on-fatal-error (the system halts and the fault log stays readable). Add it with
`west build -b piggysat/nrf9151/ns -- -DBOARD_ROOT=. -DEXTRA_CONF_FILE=fatal-debug.conf`.
