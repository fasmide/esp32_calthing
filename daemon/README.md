# Calendar Daemon

Small Go daemon that downloads an iCalendar feed, keeps a parsed cache in memory, and exposes a compact JSON API for low-memory clients like the ESP32 UI.

## Environment

- `ICAL_URL` (required): HTTP(S) URL to the `.ics` feed
- `API_TOKEN` (required): shared bearer token for UI access
- `LISTEN_ADDR` (optional): bind address, default `:8090`
- `REFRESH_INTERVAL` (optional): how often the feed is refreshed, default `15m`
- `HTTP_TIMEOUT` (optional): timeout for feed downloads, default `20s`
- `DEFAULT_QUERY_DAYS` (optional): default `/v1/events` window size, default `7`
- `MAX_WINDOW_DAYS` (optional): maximum allowed query window, default `31`

## Run

```bash
cd /work/esp32_calthing/daemon
ICAL_URL="https://example.com/calendar.ics" API_TOKEN="replace-with-random-secret" go run .
```

## Endpoints

### `GET /healthz`

Reports whether the daemon has a usable cached calendar.

This endpoint is left open for local health checks.

### `GET /v1/status`

Returns cache metadata for debugging.

### `GET /v1/events`

Returns a small paginated list of events.

Authenticated endpoints require:

```text
Authorization: Bearer <API_TOKEN>
```

Query parameters:

- `from`: RFC3339 timestamp, default `now`
- `to`: RFC3339 timestamp
- `days`: used when `to` is omitted, default `DEFAULT_QUERY_DAYS`
- `limit`: page size, default `20`, max `100`
- `cursor`: numeric offset from a previous response

Example:

```bash
curl -H 'Authorization: Bearer replace-with-random-secret' 'http://localhost:8090/v1/events?from=2026-04-09T00:00:00Z&days=3&limit=10'
```

Response shape:

```json
{
  "events": [
    {
      "id": "abc123#0",
      "title": "Standup",
      "start_ts": 1775736000,
      "end_ts": 1775737800,
      "all_day": false,
      "location": "Room A"
    }
  ],
  "next_cursor": "10",
  "has_more": true,
  "last_sync_ts": 1775700000,
  "sync_age_seconds": 12,
  "source_hash": "1a2b3c4d5e6f7a8b",
  "count": 1,
  "window_start_ts": 1775692800,
  "window_end_ts": 1775952000
}
```

## Notes

- The daemon never forwards the full `.ics` file to the ESP32.
- Events are sorted by start time.
- Recurring events are expanded into normal instances.
- `ETag` and `Last-Modified` headers are reused when the source supports them.
- This is only a shared-secret gate; still keep the daemon on a trusted LAN or VPN.
