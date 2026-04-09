# ESP32 Calendar UI

Touchscreen calendar client for the ESP32-4848S040. It connects to the Go daemon, fetches compact event pages, and renders a day view in LVGL.

## Hardware

- ESP32-S3 processor
- 16 MByte Flash in QIO mode
- 8 MByte PSRAM OPI
- 4.0 inch 480x480 ST7701 display
- GT911 touch controller on I2C

## Configuration

Edit `include/app_config.h` before flashing:

- `APP_WIFI_SSID`
- `APP_WIFI_PASSWORD`
- `APP_DAEMON_URL`
- `APP_API_TOKEN`
- `APP_TIMEZONE`

Example daemon URL:

```cpp
#define APP_DAEMON_URL "http://192.168.1.10:8090"
```

## UI behavior

- Connects to Wi-Fi on boot
- Syncs time with NTP
- Fetches one day of events from `/v1/events`
- Lets the user move to the previous day, today, next day, or refresh
- Shows a selectable event list and detail panel

## Build

```bash
pio run
```

The Danish-capable LVGL fonts are vendored under `src/generated/`, so no extra font tools or system fonts are required on the build machine.
