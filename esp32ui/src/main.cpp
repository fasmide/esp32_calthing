// ESP32 full-screen agenda UI backed by the calendar daemon.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Arduino_GFX_Library.h>
#include <ESP32_4848S040.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <lvgl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <time.h>

#include "app_config.h"
#include "touch.h"

LV_FONT_DECLARE(app_font_14);
LV_FONT_DECLARE(app_font_16);
LV_FONT_DECLARE(app_font_20);
LV_FONT_DECLARE(app_font_24);

#define GFX_BL 38

#define BLACK 0x0000

#define TFT_HOR_RES 480
#define TFT_VER_RES 480
#define TFT_ROTATION LV_DISPLAY_ROTATION_0
#define TFT_BRIGHTNESS 255
#define FRAMEBUFFER_SIZE (TFT_HOR_RES * TFT_VER_RES * sizeof(uint16_t))
// A small RGB bounce buffer removes nearly all idle flicker on this panel.
#define TFT_BOUNCE_BUFFER_PX (TFT_HOR_RES * 10)

#define APP_EVENT_CAPACITY 96
#define APP_EVENT_PAGE_LIMIT 12
#define APP_QUERY_DAYS 7
#define APP_REFRESH_INTERVAL_MS (5UL * 60UL * 1000UL)
#define APP_PULL_REFRESH_THRESHOLD 80

#define EVENT_START_COLOR 0x6EE7B7
#define EVENT_END_COLOR 0xFF8A65

Arduino_ESP32SPI *bus;
Arduino_RGB_Display *gfx;
Arduino_ESP32RGBPanel *rgbpanel;
uint16_t *framebufferA;
uint16_t *framebufferB;
SemaphoreHandle_t displayVsyncSemaphore;

struct CalendarEvent {
  String id;
  String title;
  String description;
  String location;
  time_t startTs;
  time_t endTs;
  time_t createdTs;
  bool allDay;
};

enum EventTimingState {
  EVENT_TIMING_NONE,
  EVENT_TIMING_UPCOMING,
  EVENT_TIMING_ONGOING,
};

struct AppState {
  CalendarEvent events[APP_EVENT_CAPACITY];
  int eventCount = 0;
  time_t rangeStart = 0;
  time_t rangeEnd = 0;
  time_t lastSyncTs = 0;
  time_t lastRenderMinuteTs = 0;
  unsigned long lastRefreshMs = 0;
  bool refreshRequested = true;
  bool fetchInProgress = false;
  bool wifiReady = false;
  bool clockReady = false;
  bool pullRefreshArmed = false;
  bool pullRefreshInProgress = false;
  bool refreshDeferredToNextLoop = false;
  unsigned long refreshOverlayShownAt = 0;
  String statusText;
  String detailText;
  String openDetailEventId;
} app;

static lv_style_t style_screen;
static lv_style_t style_day_panel;
static lv_style_t style_event_card;
static lv_style_t style_modal;
static lv_style_t style_fab;
static lv_style_t style_status;

static lv_obj_t *agenda_panel;
static lv_obj_t *status_pill;
static lv_obj_t *status_label;
static lv_obj_t *back_button;
static lv_obj_t *detail_overlay;
static lv_obj_t *detail_title_label;
static lv_obj_t *detail_body_label;
static lv_obj_t *refresh_overlay;
static lv_obj_t *refresh_label;

void my_disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  LV_UNUSED(area);
  // In direct mode LVGL renders into the inactive full-screen buffer. Only swap
  // on the last flush of a frame, and only after the next VSYNC.
  if (lv_display_flush_is_last(disp)) {
    if (displayVsyncSemaphore != nullptr) {
      xSemaphoreTake(displayVsyncSemaphore, portMAX_DELAY);
    }
    rgbpanel->drawBitmap(0, 0, TFT_HOR_RES, TFT_VER_RES, px_map);
  }

  lv_disp_flush_ready(disp);
}

bool onRgbVsync(esp_lcd_panel_handle_t panel, const esp_lcd_rgb_panel_event_data_t *edata, void *user_ctx) {
  LV_UNUSED(panel);
  LV_UNUSED(edata);
  BaseType_t highTaskWoken = pdFALSE;
  SemaphoreHandle_t semaphore = static_cast<SemaphoreHandle_t>(user_ctx);
  if (semaphore != nullptr) {
    xSemaphoreGiveFromISR(semaphore, &highTaskWoken);
  }
  return highTaskWoken == pdTRUE;
}

void my_touchpad_read(lv_indev_t *indev, lv_indev_data_t *data) {
  if (touch_has_signal() && touch_touched()) {
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = touch_last_x;
    data->point.y = touch_last_y;
    return;
  }

  data->state = LV_INDEV_STATE_RELEASED;
}

static uint32_t my_tick(void) {
  return millis();
}

bool hasAppConfig() {
  return strlen(APP_WIFI_SSID) > 0 && strlen(APP_DAEMON_URL) > 0 && strlen(APP_API_TOKEN) > 0;
}

String urlEncode(const String &value) {
  String encoded;
  const char *hex = "0123456789ABCDEF";

  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value.charAt(i);
    const bool unreserved = isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '~';
    if (unreserved) {
      encoded += c;
      continue;
    }

    encoded += '%';
    encoded += hex[(c >> 4) & 0x0F];
    encoded += hex[c & 0x0F];
  }

  return encoded;
}

time_t startOfLocalDay(time_t value) {
  struct tm parts;
  localtime_r(&value, &parts);
  parts.tm_hour = 0;
  parts.tm_min = 0;
  parts.tm_sec = 0;
  return mktime(&parts);
}

String formatRfc3339(time_t value) {
  struct tm parts;
  gmtime_r(&value, &parts);

  char buffer[32];
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &parts);
  return String(buffer);
}

String formatDayTitle(time_t value) {
  struct tm parts;
  localtime_r(&value, &parts);

  char buffer[48];
  strftime(buffer, sizeof(buffer), "%A", &parts);
  return String(buffer);
}

String formatDaySubtitle(time_t value) {
  struct tm parts;
  localtime_r(&value, &parts);

  char buffer[48];
  strftime(buffer, sizeof(buffer), "%d %b %Y", &parts);
  return String(buffer);
}

String formatCurrentTime(time_t value) {
  struct tm parts;
  localtime_r(&value, &parts);

  char buffer[16];
  strftime(buffer, sizeof(buffer), "%H:%M", &parts);
  return String(buffer);
}

String formatTimeRange(const CalendarEvent &event) {
  if (event.allDay) {
    return "All day";
  }

  struct tm startParts;
  struct tm endParts;
  localtime_r(&event.startTs, &startParts);
  localtime_r(&event.endTs, &endParts);

  char startBuffer[16];
  char endBuffer[16];
  strftime(startBuffer, sizeof(startBuffer), "%H:%M", &startParts);
  strftime(endBuffer, sizeof(endBuffer), "%H:%M", &endParts);

  return String(startBuffer) + " - " + endBuffer;
}

String formatRelativeDuration(time_t seconds) {
  if (seconds < 0) {
    seconds = -seconds;
  }

  const long days = seconds / 86400;
  seconds %= 86400;
  const long hours = seconds / 3600;
  const long minutes = (seconds % 3600) / 60;

  String result;
  if (days > 0) {
    result += String(days) + "d";
  }
  if (hours > 0) {
    if (!result.isEmpty()) {
      result += " ";
    }
    result += String(hours) + "h";
  }
  if (minutes > 0 || result.isEmpty()) {
    if (!result.isEmpty()) {
      result += " ";
    }
    result += String(minutes) + "m";
  }
  return result;
}

EventTimingState getEventTimingState(const CalendarEvent &event) {
  if (event.allDay) {
    return EVENT_TIMING_NONE;
  }

  const time_t now = time(nullptr);
  if (now < 1700000000) {
    return EVENT_TIMING_NONE;
  }

  if (now < event.startTs) {
    return EVENT_TIMING_UPCOMING;
  }
  if (now < event.endTs) {
    return EVENT_TIMING_ONGOING;
  }
  return EVENT_TIMING_NONE;
}

String formatEventStatus(const CalendarEvent &event) {
  const EventTimingState state = getEventTimingState(event);
  if (state == EVENT_TIMING_UPCOMING) {
    return formatRelativeDuration(event.startTs - time(nullptr));
  }
  if (state == EVENT_TIMING_ONGOING) {
    return formatRelativeDuration(event.endTs - time(nullptr));
  }
  return "";
}

lv_color_t getEventTimingColor(EventTimingState state) {
  if (state == EVENT_TIMING_UPCOMING) {
    return lv_color_hex(EVENT_START_COLOR);
  }
  if (state == EVENT_TIMING_ONGOING) {
    return lv_color_hex(EVENT_END_COLOR);
  }
  return lv_color_hex(0xD2DCEB);
}

String colorizeStatusText(const String &text, EventTimingState state) {
  if (text.isEmpty() || state == EVENT_TIMING_NONE) {
    return text;
  }

  char color[8];
  const uint32_t value = state == EVENT_TIMING_UPCOMING ? EVENT_START_COLOR : EVENT_END_COLOR;
  snprintf(color, sizeof(color), "%06lX", static_cast<unsigned long>(value));
  return String("#") + color + " " + text + "#";
}

String formatCreatedTimestamp(time_t value) {
  if (value <= 0) {
    return "Unknown";
  }

  struct tm parts;
  localtime_r(&value, &parts);

  char buffer[32];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &parts);
  return String(buffer);
}

void setStatus(const String &text, bool visible) {
  const bool currentlyVisible = status_pill != nullptr && !lv_obj_has_flag(status_pill, LV_OBJ_FLAG_HIDDEN);
  if (app.statusText == text && currentlyVisible == visible) {
    return;
  }

  app.statusText = text;
  if (status_label != nullptr) {
    lv_label_set_text(status_label, app.statusText.c_str());
  }
  if (status_pill != nullptr) {
    if (visible) {
      lv_obj_clear_flag(status_pill, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(status_pill, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

void setRefreshOverlay(const String &text, bool visible) {
  if (refresh_overlay == nullptr || refresh_label == nullptr) {
    return;
  }

  if (visible && text.isEmpty()) {
    visible = false;
  }

  lv_label_set_text(refresh_label, text.c_str());
  if (visible) {
    app.refreshOverlayShownAt = millis();
    lv_obj_clear_flag(refresh_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(refresh_overlay);
  } else {
    if (!lv_obj_has_flag(refresh_overlay, LV_OBJ_FLAG_HIDDEN)) {
      const unsigned long elapsed = millis() - app.refreshOverlayShownAt;
      if (elapsed < 250) {
        const unsigned long remaining = 250 - elapsed;
        const unsigned long startedAt = millis();
        while (millis() - startedAt < remaining) {
          lv_timer_handler();
          delay(5);
        }
      }
    }
    lv_obj_add_flag(refresh_overlay, LV_OBJ_FLAG_HIDDEN);
  }
}

void hideDetailOverlay();

void onDetailBackdropPressed(lv_event_t *e) {
  LV_UNUSED(e);
  hideDetailOverlay();
}

void showDetailOverlay(const CalendarEvent &event) {
  if (detail_overlay == nullptr) {
    return;
  }

  app.openDetailEventId = event.id;

  lv_label_set_text(detail_title_label, event.title.c_str());

  String details = formatTimeRange(event);
  const EventTimingState timingState = getEventTimingState(event);
  const String relative = formatEventStatus(event);
  if (!relative.isEmpty()) {
    details += "\n";
    details += colorizeStatusText(relative, timingState);
  }
  if (!event.location.isEmpty()) {
    details += "\n";
    details += event.location;
  }
  if (!event.description.isEmpty()) {
    details += "\n\n";
    details += event.description;
  }
  details += "\nCreated ";
  details += formatCreatedTimestamp(event.createdTs);
  details += "\n\n";
  details += event.id;
  app.detailText = details;
  lv_label_set_text(detail_body_label, app.detailText.c_str());

  lv_obj_clear_flag(detail_overlay, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(detail_overlay);
}

void hideDetailOverlay() {
  if (detail_overlay != nullptr) {
    lv_obj_add_flag(detail_overlay, LV_OBJ_FLAG_HIDDEN);
  }
  app.openDetailEventId = "";
}

void onEventPressed(lv_event_t *e) {
  CalendarEvent *event = static_cast<CalendarEvent *>(lv_event_get_user_data(e));
  if (event != nullptr) {
    showDetailOverlay(*event);
  }
}

void updateBackButtonVisibility() {
  if (agenda_panel == nullptr || back_button == nullptr) {
    return;
  }

  const bool shouldShow = lv_obj_get_scroll_y(agenda_panel) > 80;
  const bool isHidden = lv_obj_has_flag(back_button, LV_OBJ_FLAG_HIDDEN);
  if (shouldShow == !isHidden) {
    return;
  }

  if (shouldShow) {
    lv_obj_clear_flag(back_button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(back_button);
  } else {
    lv_obj_add_flag(back_button, LV_OBJ_FLAG_HIDDEN);
  }
}

void onAgendaScrolled(lv_event_t *e) {
  LV_UNUSED(e);

  if (agenda_panel != nullptr && !app.fetchInProgress && !app.pullRefreshInProgress) {
    const lv_coord_t scrollY = lv_obj_get_scroll_y(agenda_panel);
    if (scrollY < -APP_PULL_REFRESH_THRESHOLD) {
      app.pullRefreshArmed = true;
      setStatus("Release to refresh", true);
    } else if (scrollY < -40) {
      app.pullRefreshArmed = false;
      setStatus("Pull to refresh", true);
    } else if (!app.fetchInProgress) {
      app.pullRefreshArmed = false;
      if (app.eventCount > 0) {
        setStatus("", false);
      }
    }
  }

  updateBackButtonVisibility();
}

void onAgendaScrollEnd(lv_event_t *e) {
  LV_UNUSED(e);

  if (!app.pullRefreshArmed || agenda_panel == nullptr || app.fetchInProgress || app.pullRefreshInProgress) {
    return;
  }

  app.pullRefreshArmed = false;
  app.pullRefreshInProgress = true;
  app.refreshRequested = true;
  app.refreshDeferredToNextLoop = true;
  setStatus("Refreshing source", true);
  setRefreshOverlay("Refreshing calendar...", true);
}

void onBackPressed(lv_event_t *e) {
  LV_UNUSED(e);
  if (agenda_panel == nullptr) {
    return;
  }

  lv_obj_scroll_to_y(agenda_panel, 0, LV_ANIM_ON);
  hideDetailOverlay();
}

void initStyles() {
  lv_style_init(&style_screen);
  lv_style_set_bg_color(&style_screen, lv_color_hex(0x07111D));
  lv_style_set_text_color(&style_screen, lv_color_hex(0xF4F7FB));

  lv_style_init(&style_day_panel);
  lv_style_set_bg_opa(&style_day_panel, LV_OPA_TRANSP);
  lv_style_set_border_width(&style_day_panel, 0);
  lv_style_set_radius(&style_day_panel, 0);
  lv_style_set_pad_all(&style_day_panel, 8);

  lv_style_init(&style_event_card);
  lv_style_set_bg_color(&style_event_card, lv_color_hex(0x102033));
  lv_style_set_border_width(&style_event_card, 1);
  lv_style_set_border_color(&style_event_card, lv_color_hex(0x2E4B74));
  lv_style_set_radius(&style_event_card, 0);
  lv_style_set_pad_hor(&style_event_card, 10);
  lv_style_set_pad_ver(&style_event_card, 8);
  lv_style_set_text_color(&style_event_card, lv_color_hex(0xF4F7FB));

  lv_style_init(&style_modal);
  lv_style_set_bg_color(&style_modal, lv_color_hex(0x0E1A2D));
  lv_style_set_border_width(&style_modal, 1);
  lv_style_set_border_color(&style_modal, lv_color_hex(0x426389));
  lv_style_set_radius(&style_modal, 22);
  lv_style_set_pad_all(&style_modal, 18);

  lv_style_init(&style_fab);
  lv_style_set_bg_color(&style_fab, lv_color_hex(0x2B558C));
  lv_style_set_radius(&style_fab, LV_RADIUS_CIRCLE);
  lv_style_set_pad_hor(&style_fab, 18);
  lv_style_set_pad_ver(&style_fab, 12);
  lv_style_set_shadow_width(&style_fab, 0);

  lv_style_init(&style_status);
  lv_style_set_bg_color(&style_status, lv_color_hex(0x203653));
  lv_style_set_radius(&style_status, LV_RADIUS_CIRCLE);
  lv_style_set_pad_hor(&style_status, 12);
  lv_style_set_pad_ver(&style_status, 6);
}

void resetEvents() {
  app.eventCount = 0;
  for (int i = 0; i < APP_EVENT_CAPACITY; ++i) {
    app.events[i] = CalendarEvent();
  }
}

bool appendEvents(JsonArray events) {
  for (JsonObject item : events) {
    if (app.eventCount >= APP_EVENT_CAPACITY) {
      return false;
    }

    CalendarEvent &event = app.events[app.eventCount++];
    event.id = String(item["id"] | "");
    event.title = String(item["title"] | "(untitled)");
    event.description = String(item["description"] | "");
    event.location = String(item["location"] | "");
    event.startTs = item["start_ts"] | 0;
    event.endTs = item["end_ts"] | event.startTs;
    event.createdTs = item["created_ts"] | 0;
    event.allDay = item["all_day"] | false;
  }
  return true;
}

bool fetchAgendaWindow() {
  if (!app.wifiReady) {
    setStatus("Wi-Fi offline", true);
    setRefreshOverlay("", false);
    return false;
  }

  app.fetchInProgress = true;
  resetEvents();
  setStatus("Syncing calendar", true);

  String nextCursor;
  bool hasMore = false;
  int pageCount = 0;

  do {
    String url = String(APP_DAEMON_URL) + "/v1/events?from=" + urlEncode(formatRfc3339(app.rangeStart));
    url += "&to=" + urlEncode(formatRfc3339(app.rangeEnd));
    url += "&limit=" + String(APP_EVENT_PAGE_LIMIT);
    if (nextCursor.length() > 0) {
      url += "&cursor=" + urlEncode(nextCursor);
    }

    WiFiClient client;
    HTTPClient http;
    http.setConnectTimeout(8000);
    http.setTimeout(12000);

    if (!http.begin(client, url)) {
      setStatus("Request setup failed", true);
      setRefreshOverlay("", false);
      app.fetchInProgress = false;
      return false;
    }

    http.addHeader("Authorization", String("Bearer ") + APP_API_TOKEN);

    const int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
      String errorText = http.errorToString(httpCode);
      http.end();
      setStatus("Fetch failed: " + errorText, true);
      setRefreshOverlay("", false);
      app.fetchInProgress = false;
      return false;
    }

    String payload = http.getString();
    http.end();

    DynamicJsonDocument doc(12288);
    DeserializationError error = deserializeJson(doc, payload);
    if (error) {
      setStatus("Bad daemon response", true);
      setRefreshOverlay("", false);
      app.fetchInProgress = false;
      return false;
    }

    app.lastSyncTs = doc["last_sync_ts"] | app.lastSyncTs;
    hasMore = doc["has_more"] | false;
    nextCursor = String(doc["next_cursor"] | "");
    JsonArray events = doc["events"].as<JsonArray>();

    if (!appendEvents(events)) {
      hasMore = false;
      setStatus("Showing first 96 events", true);
    }

    ++pageCount;
  } while (hasMore && nextCursor.length() > 0 && pageCount < 10 && app.eventCount < APP_EVENT_CAPACITY);

  app.lastRefreshMs = millis();
  const time_t now = time(nullptr);
  if (now >= 1700000000) {
    app.lastRenderMinuteTs = now - (now % 60);
  }
  app.fetchInProgress = false;
  if (app.eventCount == 0) {
    setStatus("No upcoming events", true);
  } else {
    setStatus("", false);
  }
  setRefreshOverlay("", false);
  return true;
}

bool triggerDaemonRefresh() {
  if (!app.wifiReady) {
    setStatus("Wi-Fi offline", true);
    setRefreshOverlay("", false);
    return false;
  }

  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(20000);

  const String url = String(APP_DAEMON_URL) + "/v1/refresh";
  if (!http.begin(client, url)) {
    setStatus("Refresh setup failed", true);
    setRefreshOverlay("", false);
    return false;
  }

  http.addHeader("Authorization", String("Bearer ") + APP_API_TOKEN);
  const int httpCode = http.POST("");
  if (httpCode != HTTP_CODE_OK) {
    const String errorText = http.errorToString(httpCode);
    http.end();
    setStatus("Refresh failed: " + errorText, true);
    setRefreshOverlay("", false);
    return false;
  }

  http.end();
  return true;
}

void addDaySection(lv_obj_t *parent, time_t dayStart) {
  lv_obj_t *section = lv_obj_create(parent);
  lv_obj_add_style(section, &style_day_panel, LV_PART_MAIN);
  lv_obj_set_width(section, lv_pct(100));
  lv_obj_set_height(section, LV_SIZE_CONTENT);
  lv_obj_set_layout(section, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(section, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_gap(section, 6, 0);

  lv_obj_t *titleRow = lv_obj_create(section);
  lv_obj_remove_style_all(titleRow);
  lv_obj_set_width(titleRow, lv_pct(100));
  lv_obj_set_height(titleRow, LV_SIZE_CONTENT);
  lv_obj_set_layout(titleRow, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(titleRow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(titleRow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t *title = lv_label_create(titleRow);
  lv_obj_set_style_text_font(title, &app_font_24, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0xF7FBFF), 0);
  lv_label_set_text(title, formatDayTitle(dayStart).c_str());

  lv_obj_t *subtitleRow = lv_obj_create(section);
  lv_obj_remove_style_all(subtitleRow);
  lv_obj_set_width(subtitleRow, lv_pct(100));
  lv_obj_set_height(subtitleRow, LV_SIZE_CONTENT);
  lv_obj_set_layout(subtitleRow, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(subtitleRow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(subtitleRow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t *subtitle = lv_label_create(subtitleRow);
  lv_obj_set_style_text_font(subtitle, &app_font_14, 0);
  lv_obj_set_style_text_color(subtitle, lv_color_hex(0x9EB1CB), 0);
  lv_label_set_text(subtitle, formatDaySubtitle(dayStart).c_str());

  if (dayStart == app.rangeStart) {
    const time_t now = time(nullptr);

    lv_obj_t *clockLabel = lv_label_create(titleRow);
    lv_obj_set_style_text_font(clockLabel, &app_font_24, 0);
    lv_obj_set_style_text_color(clockLabel, lv_color_hex(0x9EB1CB), 0);
    lv_label_set_text(clockLabel, formatCurrentTime(now).c_str());
  }

  bool foundEvents = false;
  const time_t dayEnd = dayStart + 24 * 60 * 60;
  for (int i = 0; i < app.eventCount; ++i) {
    const CalendarEvent &event = app.events[i];
    if (event.endTs <= dayStart || event.startTs >= dayEnd) {
      continue;
    }

    foundEvents = true;
    lv_obj_t *button = lv_button_create(section);
    lv_obj_add_style(button, &style_event_card, LV_PART_MAIN);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(button, onEventPressed, LV_EVENT_CLICKED, &app.events[i]);
    lv_obj_set_width(button, lv_pct(100));
    lv_obj_set_height(button, LV_SIZE_CONTENT);

    lv_obj_set_layout(button, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(button, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(button, 4, 0);

    lv_obj_t *metaRow = lv_obj_create(button);
    lv_obj_remove_style_all(metaRow);
    lv_obj_set_width(metaRow, lv_pct(100));
    lv_obj_set_height(metaRow, LV_SIZE_CONTENT);
    lv_obj_set_layout(metaRow, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(metaRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(metaRow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *timeLabel = lv_label_create(metaRow);
    lv_obj_set_style_text_font(timeLabel, &app_font_16, 0);
    lv_obj_set_style_text_color(timeLabel, lv_color_hex(0xFFD166), 0);
    lv_label_set_text(timeLabel, formatTimeRange(event).c_str());

    const EventTimingState timingState = getEventTimingState(event);
    const String relative = formatEventStatus(event);
    if (!relative.isEmpty()) {
      lv_obj_t *relativeLabel = lv_label_create(metaRow);
      lv_obj_set_style_text_font(relativeLabel, &app_font_16, 0);
      lv_obj_set_style_text_color(relativeLabel, getEventTimingColor(timingState), 0);
      lv_label_set_text(relativeLabel, relative.c_str());
    }

    lv_obj_t *titleLabel = lv_label_create(button);
    lv_obj_set_style_text_font(titleLabel, &app_font_20, 0);
    lv_obj_set_style_text_color(titleLabel, lv_color_hex(0xF4F7FB), 0);
    lv_label_set_long_mode(titleLabel, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(titleLabel, lv_pct(100));
    lv_label_set_text(titleLabel, event.title.c_str());

    if (!event.location.isEmpty()) {
      lv_obj_t *locationLabel = lv_label_create(button);
      lv_obj_set_style_text_font(locationLabel, &app_font_14, 0);
      lv_obj_set_style_text_color(locationLabel, lv_color_hex(0xAFC0D8), 0);
      lv_label_set_long_mode(locationLabel, LV_LABEL_LONG_WRAP);
      lv_obj_set_width(locationLabel, lv_pct(100));
      lv_label_set_text(locationLabel, event.location.c_str());
    }
  }

  if (!foundEvents) {
    lv_obj_t *emptyLabel = lv_label_create(section);
    lv_obj_set_style_text_font(emptyLabel, &app_font_16, 0);
    lv_obj_set_style_text_color(emptyLabel, lv_color_hex(0x8EA2BE), 0);
    lv_label_set_text(emptyLabel, "No events");
  }
}

void rebuildAgenda(bool preserveView = false) {
  if (agenda_panel == nullptr) {
    return;
  }

  const lv_coord_t preservedScrollY = preserveView ? lv_obj_get_scroll_y(agenda_panel) : 0;
  const bool detailWasOpen = preserveView && detail_overlay != nullptr && !lv_obj_has_flag(detail_overlay, LV_OBJ_FLAG_HIDDEN);
  const String detailEventId = app.openDetailEventId;

  hideDetailOverlay();
  lv_obj_clean(agenda_panel);

  if (!hasAppConfig()) {
    lv_obj_t *label = lv_label_create(agenda_panel);
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(label, &app_font_20, 0);
    lv_label_set_text(label, "Set Wi-Fi, daemon URL and API token in app_config.h");
    return;
  }

  for (time_t day = app.rangeStart; day < app.rangeEnd; day += 24 * 60 * 60) {
    addDaySection(agenda_panel, day);
  }

  if (preserveView) {
    lv_obj_scroll_to_y(agenda_panel, preservedScrollY, LV_ANIM_OFF);
  } else {
    lv_obj_scroll_to_y(agenda_panel, 0, LV_ANIM_OFF);
  }

  if (detailWasOpen && !detailEventId.isEmpty()) {
    for (int i = 0; i < app.eventCount; ++i) {
      if (app.events[i].id == detailEventId) {
        showDetailOverlay(app.events[i]);
        break;
      }
    }
  }

  updateBackButtonVisibility();
}

void updateRelativeTimesIfNeeded() {
  const time_t now = time(nullptr);
  if (now < 1700000000) {
    return;
  }

  const time_t currentDayStart = startOfLocalDay(now);
  if (currentDayStart != app.rangeStart) {
    app.rangeStart = currentDayStart;
    app.rangeEnd = app.rangeStart + (APP_QUERY_DAYS * 24 * 60 * 60);
    app.refreshRequested = true;
    rebuildAgenda(true);
  }

  if (app.eventCount == 0) {
    return;
  }

  const time_t currentMinuteTs = now - (now % 60);
  if (app.lastRenderMinuteTs == currentMinuteTs) {
    return;
  }

  app.lastRenderMinuteTs = currentMinuteTs;
  rebuildAgenda(true);
}

void buildUi() {
  initStyles();

  lv_obj_t *screen = lv_screen_active();
  lv_obj_add_style(screen, &style_screen, LV_PART_MAIN);
  lv_obj_set_style_pad_all(screen, 0, 0);

  agenda_panel = lv_obj_create(screen);
  lv_obj_add_flag(agenda_panel, LV_OBJ_FLAG_SCROLL_ELASTIC);
  lv_obj_set_size(agenda_panel, lv_pct(100), lv_pct(100));
  lv_obj_set_style_pad_all(agenda_panel, 14, 0);
  lv_obj_set_style_pad_gap(agenda_panel, 12, 0);
  lv_obj_set_style_bg_opa(agenda_panel, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(agenda_panel, 0, 0);
  lv_obj_set_layout(agenda_panel, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(agenda_panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scrollbar_mode(agenda_panel, LV_SCROLLBAR_MODE_OFF);
  lv_obj_add_event_cb(agenda_panel, onAgendaScrolled, LV_EVENT_SCROLL, nullptr);
  lv_obj_add_event_cb(agenda_panel, onAgendaScrollEnd, LV_EVENT_SCROLL_END, nullptr);

  status_pill = lv_obj_create(screen);
  lv_obj_add_style(status_pill, &style_status, LV_PART_MAIN);
  lv_obj_remove_flag(status_pill, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_align(status_pill, LV_ALIGN_TOP_MID, 0, 12);
  status_label = lv_label_create(status_pill);
  lv_obj_set_style_text_font(status_label, &app_font_14, 0);
  lv_label_set_text(status_label, "");
  lv_obj_center(status_label);
  lv_obj_add_flag(status_pill, LV_OBJ_FLAG_HIDDEN);

  back_button = lv_button_create(screen);
  lv_obj_add_style(back_button, &style_fab, LV_PART_MAIN);
  lv_obj_remove_flag(back_button, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_align(back_button, LV_ALIGN_BOTTOM_RIGHT, -18, -18);
  lv_obj_add_event_cb(back_button, onBackPressed, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_flag(back_button, LV_OBJ_FLAG_HIDDEN);
  lv_obj_t *backLabel = lv_label_create(back_button);
  lv_obj_set_style_text_font(backLabel, &app_font_16, 0);
  lv_label_set_text(backLabel, "Back");
  lv_obj_center(backLabel);

  detail_overlay = lv_obj_create(screen);
  lv_obj_remove_flag(detail_overlay, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(detail_overlay, lv_pct(100), lv_pct(100));
  lv_obj_set_style_bg_color(detail_overlay, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(detail_overlay, LV_OPA_70, 0);
  lv_obj_set_style_border_width(detail_overlay, 0, 0);
  lv_obj_set_style_pad_all(detail_overlay, 0, 0);
  lv_obj_add_event_cb(detail_overlay, onDetailBackdropPressed, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_flag(detail_overlay, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t *modal = lv_obj_create(detail_overlay);
  lv_obj_add_style(modal, &style_modal, LV_PART_MAIN);
  lv_obj_remove_flag(modal, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(modal, 408, LV_SIZE_CONTENT);
  lv_obj_center(modal);
  lv_obj_set_layout(modal, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(modal, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_gap(modal, 10, 0);

  detail_title_label = lv_label_create(modal);
  lv_obj_set_style_text_font(detail_title_label, &app_font_24, 0);
  lv_obj_set_style_text_color(detail_title_label, lv_color_hex(0xF7FBFF), 0);
  lv_label_set_long_mode(detail_title_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(detail_title_label, lv_pct(100));
  lv_label_set_text(detail_title_label, "");

  detail_body_label = lv_label_create(modal);
  lv_obj_set_style_text_font(detail_body_label, &app_font_16, 0);
  lv_obj_set_style_text_color(detail_body_label, lv_color_hex(0xD2DCEB), 0);
  lv_label_set_long_mode(detail_body_label, LV_LABEL_LONG_WRAP);
  lv_label_set_recolor(detail_body_label, true);
  lv_obj_set_width(detail_body_label, lv_pct(100));
  lv_label_set_text(detail_body_label, "");

  refresh_overlay = lv_obj_create(screen);
  lv_obj_add_style(refresh_overlay, &style_modal, LV_PART_MAIN);
  lv_obj_remove_flag(refresh_overlay, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(refresh_overlay, 300, LV_SIZE_CONTENT);
  lv_obj_align(refresh_overlay, LV_ALIGN_CENTER, 0, 0);
  lv_obj_add_flag(refresh_overlay, LV_OBJ_FLAG_HIDDEN);

  refresh_label = lv_label_create(refresh_overlay);
  lv_obj_set_style_text_font(refresh_label, &app_font_20, 0);
  lv_obj_set_style_text_color(refresh_label, lv_color_hex(0xF7FBFF), 0);
  lv_label_set_long_mode(refresh_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(refresh_label, lv_pct(100));
  lv_label_set_text(refresh_label, "Refreshing calendar...");
  lv_obj_center(refresh_label);

  rebuildAgenda();
}

void connectWifi() {
  if (!hasAppConfig()) {
    setStatus("Set Wi-Fi, daemon, token", true);
    return;
  }

  WiFi.mode(WIFI_STA);
  // RGB panel + PSRAM framebuffers are much more stable with modem sleep disabled.
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(APP_WIFI_SSID, APP_WIFI_PASSWORD);
  setStatus("Connecting Wi-Fi", true);

  const unsigned long startedAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedAt < 15000) {
    delay(200);
  }

  app.wifiReady = WiFi.status() == WL_CONNECTED;
  if (!app.wifiReady) {
    setStatus("Wi-Fi connect failed", true);
    return;
  }

  configTzTime(APP_TIMEZONE, "pool.ntp.org", "time.nist.gov");

  const unsigned long clockStartedAt = millis();
  time_t now = time(nullptr);
  while (now < 1700000000 && millis() - clockStartedAt < 10000) {
    delay(200);
    now = time(nullptr);
  }

  app.clockReady = now >= 1700000000;
  app.rangeStart = startOfLocalDay(app.clockReady ? now : time(nullptr));
  app.rangeEnd = app.rangeStart + (APP_QUERY_DAYS * 24 * 60 * 60);
  setStatus(app.clockReady ? "Syncing calendar" : "Wi-Fi ready, no NTP", true);
}

void refreshIfNeeded() {
  if (app.fetchInProgress) {
    return;
  }

  if (app.refreshDeferredToNextLoop) {
    app.refreshDeferredToNextLoop = false;
    return;
  }

  if (!hasAppConfig()) {
    rebuildAgenda();
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    app.wifiReady = false;
    if (millis() - app.lastRefreshMs > 10000 || app.refreshRequested) {
      connectWifi();
    }
  } else {
    app.wifiReady = true;
  }

  if (!app.wifiReady) {
    return;
  }

  const bool stale = millis() - app.lastRefreshMs > APP_REFRESH_INTERVAL_MS;
  if (!app.refreshRequested && !stale) {
    return;
  }

  setRefreshOverlay(app.pullRefreshInProgress ? "Refreshing calendar..." : "Reloading calendar...", true);

  if (app.pullRefreshInProgress) {
    if (!triggerDaemonRefresh()) {
      app.pullRefreshInProgress = false;
      return;
    }
  }

  app.refreshRequested = false;
  if (fetchAgendaWindow()) {
    rebuildAgenda();
  }
  app.pullRefreshInProgress = false;
}

void setupDisplay() {
  touch_init();

  bus = new Arduino_ESP32SPI(GFX_NOT_DEFINED, 39, 48, 47, GFX_NOT_DEFINED);

  // Keep the panel timing at the original board values; the flicker fix comes
  // from the bounce-buffer/manual-flush path rather than porch retuning.
  rgbpanel = new Arduino_ESP32RGBPanel(
      18, 17, 16, 21,
      11, 12, 13, 14, 0,
      8, 20, 3, 46, 9, 10,
      4, 5, 6, 7, 15,
      1, 10, 8, 50,
      1, 10, 8, 20,
      0, GFX_NOT_DEFINED, false,
      0, 0, TFT_BOUNCE_BUFFER_PX, 2);

  gfx = new Arduino_RGB_Display(
      // Manual flush is intentional: auto flush caused frequent idle flicker
      // when the RGB panel scanned from PSRAM.
      480, 480, rgbpanel, 0, false,
      bus, GFX_NOT_DEFINED, st7701_4848s040_init_operations, sizeof(st7701_4848s040_init_operations));

  if (!gfx->begin()) {
    Serial.println("gfx->begin() failed");
  }

  framebufferA = rgbpanel->getFrameBufferByIndex(0);
  framebufferB = rgbpanel->getFrameBufferByIndex(1);
  if (framebufferA == nullptr || framebufferB == nullptr) {
    Serial.println("rgbpanel framebuffers unavailable");
  }

  displayVsyncSemaphore = xSemaphoreCreateBinary();
  if (displayVsyncSemaphore == nullptr) {
    Serial.println("displayVsyncSemaphore create failed");
  } else {
    esp_lcd_rgb_panel_event_callbacks_t callbacks = {};
    callbacks.on_vsync = onRgbVsync;
    rgbpanel->registerEventCallbacks(&callbacks, displayVsyncSemaphore);
  }

  pinMode(GFX_BL, OUTPUT);
  analogWrite(GFX_BL, TFT_BRIGHTNESS);

  gfx->displayOn();
  gfx->fillScreen(BLACK);
  gfx->setRotation((4 - TFT_ROTATION) % 4);
}

void setupLvgl() {
  lv_init();
  lv_tick_set_cb(my_tick);

  lv_display_t *disp = lv_display_create(TFT_HOR_RES, TFT_VER_RES);
  lv_display_set_flush_cb(disp, my_disp_flush);
  lv_display_set_buffers(disp, framebufferA, framebufferB, FRAMEBUFFER_SIZE, LV_DISPLAY_RENDER_MODE_DIRECT);
  lv_display_set_rotation(disp, TFT_ROTATION);

  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, my_touchpad_read);
}

void setup() {
  Serial.begin(115200);
  Serial.println("Starting agenda UI");

  setupDisplay();
  setupLvgl();

  app.rangeStart = startOfLocalDay(time(nullptr));
  app.rangeEnd = app.rangeStart + (APP_QUERY_DAYS * 24 * 60 * 60);

  buildUi();
  connectWifi();
  refreshIfNeeded();
}

void loop() {
  lv_timer_handler();
  refreshIfNeeded();
  updateRelativeTimesIfNeeded();
  delay(5);
}
