# AGENTS.md

## Project Overview

This project has two parts:

- `daemon/`: a Go service that fetches an iCalendar feed, parses it, caches it in memory, and exposes a compact JSON API for the device UI.
- `esp32ui/`: a PlatformIO Arduino project for an ESP32-S3 touchscreen device that renders a calendar agenda using LVGL.

The current product is an MVP calendar appliance:

- daemon fetches a large remote `.ics`
- ESP32 never downloads the `.ics` directly
- ESP32 talks only to the daemon over a lightweight JSON API

## Repository Layout

- `/work/esp32_calthing/daemon/main.go`: daemon implementation
- `/work/esp32_calthing/daemon/main_test.go`: basic tests
- `/work/esp32_calthing/daemon/sample.ical`: sample feed used during debugging
- `/work/esp32_calthing/esp32ui/src/main.cpp`: full UI app
- `/work/esp32_calthing/esp32ui/include/app_config.h`: device config
- `/work/esp32_calthing/esp32ui/include/lv_conf.h`: LVGL config
- `/work/esp32_calthing/esp32ui/src/generated/*.c`: vendored LVGL fonts with Danish glyph support

## Daemon Architecture

The daemon:

- loads config from env
- downloads `ICAL_URL`
- caches parsed events in memory
- refreshes periodically
- serves JSON over HTTP

Main env vars:

- `ICAL_URL` required
- `API_TOKEN` required
- `LISTEN_ADDR` default `:8090`
- `REFRESH_INTERVAL` default `15m`
- `HTTP_TIMEOUT` default `20s`
- `DEFAULT_QUERY_DAYS` default `7`
- `MAX_WINDOW_DAYS` default `31`

Endpoints:

- `GET /healthz`: unauthenticated health check
- `GET /v1/status`: authenticated daemon/cache status
- `GET /v1/events`: authenticated paginated event query
- `POST /v1/refresh`: authenticated force refresh of remote iCal source before UI reload

Auth model:

- shared secret only
- request header: `Authorization: Bearer <API_TOKEN>`

## Daemon Event Model

The daemon currently returns event items with:

- `id`
- `title`
- `start_ts`
- `end_ts`
- `created_ts`
- `all_day`
- `location`

Notes:

- timestamps are unix seconds
- daemon sends absolute instants in UTC semantics
- ESP32 formats them into Copenhagen local time for display

## Daemon Parsing Rules

Important behavior already fixed:

1. Calendar timezone handling

- The daemon reads calendar default timezone from `X-WR-TIMEZONE`
- Floating iCal times without `Z` now use the calendar timezone instead of incorrectly defaulting to UTC

2. UTC event handling

- If event timestamps end in `Z`, they are already absolute UTC and are preserved as such
- Example from `sample.ical`: `DTSTART:20240828T133000Z` must display as `15:30` in Copenhagen summer time

3. All-day events

- `VALUE=DATE` entries are parsed in the calendar timezone, not as UTC midnight
- This prevents all-day events from spilling into the next local day

4. Created timestamp

- `created_ts` comes from iCal `CREATED`
- fallback is `DTSTAMP`

5. Recurring events

- recurring rules are expanded into normal instances
- exclusions via `EXDATE` are handled

## Daemon Pagination Rules

The daemon supports:

- `from=<RFC3339>`
- `to=<RFC3339>`
- `days=<n>` when `to` is omitted
- `limit=<n>`
- `cursor=<offset>`

Important paging rule added during this session:

- The daemon must not end a page in the middle of a day
- If a page hits `limit` in the middle of a day, it extends the page until the end of that day
- `next_cursor` then begins at the next day boundary

This matters because the ESP32 agenda groups by day and users should not see an incomplete last day in a page.

Current implementation note:

- day-boundary extension is currently based on the start day of the last included event

## ESP32 UI Architecture

The device UI is a full-screen agenda, not a dashboard.

Core behavior:

- idle screen shows only calendar content
- events are grouped into day sections
- no persistent buttons in the idle layout
- tap an event to open a detail overlay
- scroll down and a contextual `Back` button appears
- pull down from the top to refresh

The UI is intentionally content-first and touch-first.

## ESP32 Configuration

Edit `esp32ui/include/app_config.h`:

- `APP_WIFI_SSID`
- `APP_WIFI_PASSWORD`
- `APP_DAEMON_URL`
- `APP_API_TOKEN`
- `APP_TIMEZONE`

Current timezone config is Copenhagen with DST:

```cpp
#define APP_TIMEZONE "CET-1CEST,M3.5.0/2,M10.5.0/3"
```

Important:

- ESP32 uses `configTzTime(...)`
- display formatting uses local time via `localtime_r(...)`
- day grouping also uses local day boundaries

Do not revert this to plain `configTime(...)` unless you also rework timezone handling.

## ESP32 UI Details

Current screen model:

- vertically scrolling agenda list
- each day rendered as a day card/section
- each event rendered as a tappable card
- event detail shown in a modal overlay
- a floating `Back` button appears when the user scrolls away from top

Event card content:

- first row: absolute time on left, relative timing on right
- second row: title
- optional location

Relative timing:

- updates every minute without network reload
- format supports days, hours, minutes
- examples: `3d 3h`, `42m`

Color semantics:

- upcoming events use green-ish `#6EE7B7`
- ongoing events use orange-ish `#FF8A65`
- there is no `In` / `Ends` text anymore

Detail overlay:

- title
- time range
- colorized relative timing
- location if present
- created timestamp shown as local `YYYY-MM-DD HH:MM:SS`
- event id at bottom

## Pull-To-Refresh Behavior

The intended gesture is Snapchat-like pull-to-refresh.

Current implementation:

- agenda scroll is elastic
- when overscrolling at the top:
  - below threshold: shows `Pull to refresh`
  - past threshold: shows `Release to refresh`
- on release while still past threshold:
  - UI calls daemon `POST /v1/refresh`
  - daemon re-fetches and reparses `.ics`
  - UI then fetches events again

Important bugfixes already applied:

- threshold lowered to `80`
- arm/disarm state fixed so drifting back above threshold clears the armed state
- this was necessary because refresh could otherwise trigger on the wrong release state

Refresh overlay:

- centered modal dialog shown during reload
- pull refresh text: `Refreshing calendar...`
- normal timed/startup reload text: `Reloading calendar...`
- kept visible briefly so fast reloads do not blink invisibly

## Refresh Cadence

There are three refresh cadences to understand:

1. UI network reload cadence

- UI polls daemon every `5` minutes
- controlled by `APP_REFRESH_INTERVAL_MS`

2. UI local relative-time refresh cadence

- every `1` minute
- no network traffic
- agenda is rebuilt in place while preserving scroll and open detail overlay

3. Daemon source refresh cadence

- daemon refreshes remote `.ics` every `15` minutes by default
- controlled by daemon `REFRESH_INTERVAL`

Pull-to-refresh bypasses the daemon timer by forcing an immediate refresh.

## Fonts and Text Rendering

Do not assume built-in LVGL fonts support Danish characters.

Current solution:

- custom vendored font sources live in `esp32ui/src/generated/`
- glyph set includes Danish characters such as `æ ø å Æ Ø Å`
- these files are committed and build locally without external font tooling

Do not reintroduce dynamic font generation as a build requirement unless necessary.

## Build and Verification

Daemon:

```bash
cd /work/esp32_calthing/daemon
go test ./...
go build ./...
```

ESP32 UI:

```bash
cd /work/esp32_calthing/esp32ui
/root/.local/bin/pio run
```

PlatformIO is installed via `pipx` in this environment and the binary path used during this session was:

```bash
/root/.local/bin/pio
```

Recent memory numbers after the latest successful build:

- RAM: about `71.3%`
- Flash: about `42.0%`

## Known Practical Constraints

1. LVGL gesture handling is sensitive

- small widget tree changes can break taps inside scroll containers
- simpler direct-clickable event cards worked better than nested containers

2. Timezone handling is subtle

- daemon and ESP32 both matter
- daemon must interpret iCal correctly
- ESP32 must format locally with DST-aware rules

3. All-day events are special

- date-only events should be treated as local calendar days, not UTC-midnight timestamps

4. Pagination matters for UX

- partial final days are confusing on the agenda
- page extension to end-of-day is intentional

## Recommended Next Areas If Development Continues

- improve refresh overlay with spinner/progress animation
- add temporary debug view for timestamps if timezone regressions appear again
- improve README docs, which are behind the actual implemented behavior
- add more daemon tests around timezone handling, all-day events, and page-boundary extension
- consider local caching on device if daemon/network availability becomes an issue

## Session Summary Of Important Decisions

- use shared-secret bearer auth rather than open API
- keep ESP32 config in `app_config.h`
- use content-first agenda UI without persistent controls
- use Copenhagen local time with automatic DST
- keep vendored fonts in repo for portable builds
- use pull-to-refresh to force daemon source refresh
- never show half a day at the end of a daemon page

## If You Are A New Agent

Before changing behavior, read at least:

- `daemon/main.go`
- `esp32ui/src/main.cpp`
- `esp32ui/include/app_config.h`
- this `AGENTS.md`

If working on time behavior, also inspect:

- `daemon/sample.ical`

If a user reports time being off again:

1. verify whether the source event time ends in `Z`
2. verify daemon interpretation of floating vs UTC timestamps
3. verify ESP32 local timezone setup still uses `configTzTime(...)`
4. verify display formatting still uses `localtime_r(...)`

If a user reports pull-to-refresh problems:

1. verify elastic scrolling is enabled
2. verify threshold and armed-state logic in `onAgendaScrolled`
3. verify trigger happens on scroll-end, not generic touch release
