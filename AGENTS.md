# AGENTS.md

## Overview

This repo has two runnable parts at the workspace root:

- `/work/daemon`: Go calendar daemon
- `/work/esp32ui`: PlatformIO ESP32-S3 touchscreen UI

The device does not fetch `.ics` directly. It talks to the daemon over a small authenticated JSON API.

## Environment Notes

- Workspace root is `/work`
- This container runs as `root` on Arch Linux
- Tools can be installed when needed with `pacman` or `pip`
- `platformio` may not already be installed; `python -m pip install --break-system-packages platformio` works here

## Files To Read First

Read these before changing behavior:

- `/work/daemon/main.go`
- `/work/esp32ui/src/main.cpp`
- `/work/esp32ui/include/app_config.h`
- `/work/AGENTS.md`

If working on calendar parsing or time behavior, also read:

- `/work/daemon/sample.ical`
- `/work/daemon/main_test.go`

## Daemon

The daemon:

- loads config from env
- downloads the remote iCal feed
- parses and caches expanded events in memory
- refreshes periodically
- serves JSON to the ESP32 UI

Current HTTP routes are implemented in `daemon/main.go`.

Important durable behavior:

- shared-secret bearer auth is used for API routes
- floating iCal times use the calendar timezone, not UTC
- `...Z` times are treated as absolute UTC instants
- all-day `VALUE=DATE` entries are handled as local calendar days
- recurring events are expanded and `EXDATE` is respected
- event `created_ts` comes from `CREATED`, falling back to `DTSTAMP`
- event pagination must not stop in the middle of a day

## ESP32 UI

The UI is a full-screen touch agenda built with LVGL.

Important durable behavior:

- content-first layout with no persistent idle-screen controls
- events are grouped by local day
- tapping an event opens a detail overlay
- a contextual back button appears when scrolled away from the top
- pull-to-refresh triggers daemon `POST /v1/refresh`
- relative time updates locally every minute without a network reload

Important implementation constraints:

- keep timezone handling on device based on `configTzTime(...)`
- display formatting and day grouping should continue using local time APIs such as `localtime_r(...)`
- do not assume built-in LVGL fonts cover Danish glyphs; vendored font sources in `esp32ui/src/generated/` are intentional
- LVGL gesture/click behavior is fragile inside scroll containers; prefer minimal widget-tree changes unless needed

## Config

ESP32 runtime config lives in `esp32ui/include/app_config.h`.

Daemon runtime config is environment-driven; inspect `loadConfig()` in `daemon/main.go` for the current source of truth.

Do not duplicate config defaults in this file when they are easy to read directly from code.

## Build And Flash

Daemon:

```bash
go test ./...
go build ./...
```

Run those in `/work/daemon`.

ESP32 UI build:

```bash
pio run
```

Run that in `/work/esp32ui`.

ESP32 UI upload:

```bash
pio device list
pio run --target upload --upload-port /dev/ttyUSB0
```

This was verified in this environment against a connected ESP32-S3 on `/dev/ttyUSB0`.

## Troubleshooting Cues

If time is wrong:

1. Check whether the source timestamp ends in `Z`.
2. Check daemon handling of floating times and all-day dates.
3. Check that the ESP32 still uses `configTzTime(...)`.
4. Check that formatting and grouping still use local time.

If pull-to-refresh breaks:

1. Check top overscroll behavior in the agenda scroll container.
2. Check threshold and armed-state logic in `onAgendaScrolled`.
3. Check that refresh triggers on scroll end rather than a generic touch release.

## Editing Guidance

- Prefer small changes.
- Do not rewrite documented behavior just because this file is shorter than before.
- Keep durable product constraints here; keep code-level details in code.

## Git Safety

- Using git for read-only inspection is fine, for example `git status`, `git diff`, and `git log`.
- Never mutate the git repository from an agent.
- Do not create commits, amend commits, stage files, switch branches, rebase, reset, push, or otherwise write git state.
- Only humans commit and mutate the repo.
