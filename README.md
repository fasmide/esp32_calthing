# Calendar Thing

Small self-hosted calendar display built from two parts:

- `daemon/`: a Go service that fetches an iCal feed, expands recurring events, caches them, and serves a small JSON API
- `esp32ui/`: an ESP32-S3 touchscreen agenda UI built with LVGL and PlatformIO

The ESP32 does not fetch `.ics` directly. It talks to the daemon over an authenticated local API.

## Data Source

Calendar data comes from a remote iCal / `.ics` feed configured on the daemon side.

The daemon downloads that feed, expands recurring events, normalizes time handling, and exposes the result to the ESP32 UI as a small JSON API.

## Hardware

The UI targets an ESP32-S3 RGB touchscreen panel.

This repo has been developed and tested with a `480x480` ESP32-S3 display using:

- RGB LCD panel driven by the ESP32-S3 RGB peripheral
- GT911 touch controller
- on-device Wi-Fi for daemon/API access and NTP time sync

## Demo

Add demo video here.

## License

This project is released under the `Unlicense`. See `LICENSE`.

## Run

Daemon:

```bash
cd daemon
go test ./...
go build ./...
ICAL_URL="..." API_TOKEN="..." ./calendar-daemon
```

ESP32 UI:

```bash
cd esp32ui
pio run
pio run --target upload --upload-port /dev/ttyUSB0
```

Device runtime config lives in `esp32ui/include/app_config.h`.

Daemon runtime config is environment-driven; see `loadConfig()` in `daemon/main.go`.

## Notes

- Events are grouped by local day on the device
- Pull-to-refresh triggers the daemon refresh endpoint
- The UI keeps local time via NTP and local timezone APIs
- Recurring and all-day event expansion happens in the daemon
