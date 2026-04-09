package main

import (
	"bytes"
	"context"
	"crypto/sha1"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	ics "github.com/arran4/golang-ical"
	"github.com/teambition/rrule-go"
)

const (
	defaultListenAddr     = ":8090"
	defaultRefresh        = 15 * time.Minute
	defaultHTTPTimeout    = 20 * time.Second
	defaultQueryDays      = 7
	defaultLimit          = 20
	maxLimit              = 100
	defaultMaxWindowDays  = 31
	defaultStartupTimeout = 30 * time.Second
)

type config struct {
	listenAddr      string
	icalURL         string
	apiToken        string
	refreshInterval time.Duration
	httpTimeout     time.Duration
	queryDays       int
	maxWindowDays   int
}

type event struct {
	ID       string
	UID      string
	Title    string
	Start    time.Time
	End      time.Time
	Created  time.Time
	AllDay   bool
	Location string
}

type eventResponse struct {
	ID        string `json:"id"`
	Title     string `json:"title"`
	StartTS   int64  `json:"start_ts"`
	EndTS     int64  `json:"end_ts"`
	CreatedTS int64  `json:"created_ts,omitempty"`
	AllDay    bool   `json:"all_day"`
	Location  string `json:"location,omitempty"`
}

type calendarState struct {
	events           []event
	lastSync         time.Time
	sourceHash       string
	etag             string
	lastModified     string
	lastError        string
	lastSuccess      time.Time
	sourceEventCount int
}

type daemon struct {
	cfg        config
	httpClient *http.Client

	mu    sync.RWMutex
	state calendarState
}

type listResponse struct {
	Events         []eventResponse `json:"events"`
	NextCursor     string          `json:"next_cursor,omitempty"`
	HasMore        bool            `json:"has_more"`
	LastSync       int64           `json:"last_sync_ts,omitempty"`
	SyncAgeSeconds int64           `json:"sync_age_seconds,omitempty"`
	SourceHash     string          `json:"source_hash,omitempty"`
	Count          int             `json:"count"`
	WindowStartTS  int64           `json:"window_start_ts"`
	WindowEndTS    int64           `json:"window_end_ts"`
}

type statusResponse struct {
	Healthy          bool   `json:"healthy"`
	LastSyncTS       int64  `json:"last_sync_ts,omitempty"`
	LastSuccessTS    int64  `json:"last_success_ts,omitempty"`
	SyncAgeSeconds   int64  `json:"sync_age_seconds,omitempty"`
	CachedEvents     int    `json:"cached_events"`
	LastError        string `json:"last_error,omitempty"`
	SourceHash       string `json:"source_hash,omitempty"`
	SourceEventCount int    `json:"source_event_count"`
}

func main() {
	cfg, err := loadConfig()
	if err != nil {
		log.Fatal(err)
	}

	d := &daemon{
		cfg: cfg,
		httpClient: &http.Client{
			Timeout: cfg.httpTimeout,
		},
	}

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	startupCtx, startupCancel := context.WithTimeout(ctx, defaultStartupTimeout)
	if err := d.refresh(startupCtx); err != nil {
		log.Printf("initial refresh failed: %v", err)
	}
	startupCancel()

	go d.refreshLoop(ctx)

	mux := http.NewServeMux()
	mux.HandleFunc("/healthz", d.handleHealth)
	mux.HandleFunc("/v1/status", d.handleStatus)
	mux.HandleFunc("/v1/refresh", d.handleRefresh)
	mux.HandleFunc("/v1/events", d.handleEvents)

	server := &http.Server{
		Addr:         cfg.listenAddr,
		Handler:      loggingMiddleware(d.authMiddleware(mux)),
		ReadTimeout:  10 * time.Second,
		WriteTimeout: 15 * time.Second,
		IdleTimeout:  30 * time.Second,
	}

	log.Printf("calendar daemon listening on %s", cfg.listenAddr)
	if err := server.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
		log.Fatal(err)
	}
}

func loadConfig() (config, error) {
	cfg := config{
		listenAddr:      envOrDefault("LISTEN_ADDR", defaultListenAddr),
		icalURL:         strings.TrimSpace(os.Getenv("ICAL_URL")),
		apiToken:        strings.TrimSpace(os.Getenv("API_TOKEN")),
		refreshInterval: durationEnvOrDefault("REFRESH_INTERVAL", defaultRefresh),
		httpTimeout:     durationEnvOrDefault("HTTP_TIMEOUT", defaultHTTPTimeout),
		queryDays:       intEnvOrDefault("DEFAULT_QUERY_DAYS", defaultQueryDays),
		maxWindowDays:   intEnvOrDefault("MAX_WINDOW_DAYS", defaultMaxWindowDays),
	}

	if cfg.icalURL == "" {
		return config{}, errors.New("ICAL_URL is required")
	}
	if cfg.apiToken == "" {
		return config{}, errors.New("API_TOKEN is required")
	}
	if cfg.queryDays < 1 {
		return config{}, errors.New("DEFAULT_QUERY_DAYS must be at least 1")
	}
	if cfg.maxWindowDays < 1 {
		return config{}, errors.New("MAX_WINDOW_DAYS must be at least 1")
	}

	return cfg, nil
}

func (d *daemon) refreshLoop(ctx context.Context) {
	ticker := time.NewTicker(d.cfg.refreshInterval)
	defer ticker.Stop()

	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			refreshCtx, cancel := context.WithTimeout(ctx, d.cfg.httpTimeout+10*time.Second)
			if err := d.refresh(refreshCtx); err != nil {
				log.Printf("refresh failed: %v", err)
			}
			cancel()
		}
	}
}

func (d *daemon) refresh(ctx context.Context) error {
	d.mu.RLock()
	etag := d.state.etag
	lastModified := d.state.lastModified
	d.mu.RUnlock()

	req, err := http.NewRequestWithContext(ctx, http.MethodGet, d.cfg.icalURL, nil)
	if err != nil {
		return d.setRefreshError(fmt.Errorf("build request: %w", err))
	}
	if etag != "" {
		req.Header.Set("If-None-Match", etag)
	}
	if lastModified != "" {
		req.Header.Set("If-Modified-Since", lastModified)
	}

	resp, err := d.httpClient.Do(req)
	if err != nil {
		return d.setRefreshError(fmt.Errorf("download ical: %w", err))
	}
	defer resp.Body.Close()

	if resp.StatusCode == http.StatusNotModified {
		d.mu.Lock()
		d.state.lastSync = time.Now().UTC()
		d.state.lastError = ""
		d.mu.Unlock()
		return nil
	}
	if resp.StatusCode != http.StatusOK {
		return d.setRefreshError(fmt.Errorf("ical source returned %s", resp.Status))
	}

	body, err := io.ReadAll(resp.Body)
	if err != nil {
		return d.setRefreshError(fmt.Errorf("read ical body: %w", err))
	}

	events, err := parseICS(body)
	if err != nil {
		return d.setRefreshError(fmt.Errorf("parse ical body: %w", err))
	}

	now := time.Now().UTC()
	d.mu.Lock()
	d.state.events = events
	d.state.lastSync = now
	d.state.lastSuccess = now
	d.state.lastError = ""
	d.state.etag = resp.Header.Get("ETag")
	d.state.lastModified = resp.Header.Get("Last-Modified")
	d.state.sourceHash = hashBytes(body)
	d.state.sourceEventCount = len(events)
	d.mu.Unlock()

	log.Printf("refreshed calendar with %d events", len(events))
	return nil
}

func (d *daemon) setRefreshError(err error) error {
	d.mu.Lock()
	d.state.lastSync = time.Now().UTC()
	d.state.lastError = err.Error()
	d.mu.Unlock()
	return err
}

func (d *daemon) authMiddleware(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/healthz" {
			next.ServeHTTP(w, r)
			return
		}

		authHeader := strings.TrimSpace(r.Header.Get("Authorization"))
		if authHeader != "Bearer "+d.cfg.apiToken {
			writeJSON(w, http.StatusUnauthorized, map[string]string{"error": "unauthorized"})
			return
		}

		next.ServeHTTP(w, r)
	})
}

func (d *daemon) handleHealth(w http.ResponseWriter, r *http.Request) {
	state := d.snapshot()
	healthy := state.lastSuccess.Unix() > 0 && state.lastError == ""
	status := http.StatusOK
	if !healthy {
		status = http.StatusServiceUnavailable
	}
	writeJSON(w, status, statusResponse{
		Healthy:          healthy,
		LastSyncTS:       state.lastSync.Unix(),
		LastSuccessTS:    state.lastSuccess.Unix(),
		SyncAgeSeconds:   ageSeconds(state.lastSuccess),
		CachedEvents:     len(state.events),
		LastError:        state.lastError,
		SourceHash:       state.sourceHash,
		SourceEventCount: state.sourceEventCount,
	})
}

func (d *daemon) handleStatus(w http.ResponseWriter, r *http.Request) {
	state := d.snapshot()
	writeJSON(w, http.StatusOK, statusResponse{
		Healthy:          state.lastSuccess.Unix() > 0 && state.lastError == "",
		LastSyncTS:       state.lastSync.Unix(),
		LastSuccessTS:    state.lastSuccess.Unix(),
		SyncAgeSeconds:   ageSeconds(state.lastSuccess),
		CachedEvents:     len(state.events),
		LastError:        state.lastError,
		SourceHash:       state.sourceHash,
		SourceEventCount: state.sourceEventCount,
	})
}

func (d *daemon) handleRefresh(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeJSON(w, http.StatusMethodNotAllowed, map[string]string{"error": "method not allowed"})
		return
	}

	ctx, cancel := context.WithTimeout(r.Context(), d.cfg.httpTimeout+10*time.Second)
	defer cancel()

	if err := d.refresh(ctx); err != nil {
		writeJSON(w, http.StatusBadGateway, map[string]string{"error": err.Error()})
		return
	}

	state := d.snapshot()
	writeJSON(w, http.StatusOK, statusResponse{
		Healthy:          state.lastSuccess.Unix() > 0 && state.lastError == "",
		LastSyncTS:       state.lastSync.Unix(),
		LastSuccessTS:    state.lastSuccess.Unix(),
		SyncAgeSeconds:   ageSeconds(state.lastSuccess),
		CachedEvents:     len(state.events),
		LastError:        state.lastError,
		SourceHash:       state.sourceHash,
		SourceEventCount: state.sourceEventCount,
	})
}

func (d *daemon) handleEvents(w http.ResponseWriter, r *http.Request) {
	windowStart, windowEnd, err := d.parseWindow(r)
	if err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": err.Error()})
		return
	}

	limit := intEnvOrDefaultFromQuery(r, "limit", defaultLimit)
	if limit < 1 {
		limit = defaultLimit
	}
	if limit > maxLimit {
		limit = maxLimit
	}

	cursor, err := parseCursor(r.URL.Query().Get("cursor"))
	if err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "invalid cursor"})
		return
	}

	state := d.snapshot()
	if len(state.events) == 0 && state.lastError != "" {
		writeJSON(w, http.StatusServiceUnavailable, map[string]string{"error": state.lastError})
		return
	}

	filtered := filterEvents(state.events, windowStart, windowEnd)
	if cursor > len(filtered) {
		cursor = len(filtered)
	}

	end := cursor + limit
	if end > len(filtered) {
		end = len(filtered)
	}
	end = extendPageToDayBoundary(filtered, cursor, end)

	page := make([]eventResponse, 0, end-cursor)
	for _, evt := range filtered[cursor:end] {
		page = append(page, eventResponse{
			ID:        evt.ID,
			Title:     evt.Title,
			StartTS:   evt.Start.Unix(),
			EndTS:     evt.End.Unix(),
			CreatedTS: unixOrZero(evt.Created),
			AllDay:    evt.AllDay,
			Location:  evt.Location,
		})
	}

	resp := listResponse{
		Events:         page,
		HasMore:        end < len(filtered),
		LastSync:       state.lastSuccess.Unix(),
		SyncAgeSeconds: ageSeconds(state.lastSuccess),
		SourceHash:     state.sourceHash,
		Count:          len(page),
		WindowStartTS:  windowStart.Unix(),
		WindowEndTS:    windowEnd.Unix(),
	}
	if resp.HasMore {
		resp.NextCursor = strconv.Itoa(end)
	}

	writeJSON(w, http.StatusOK, resp)
}

func (d *daemon) parseWindow(r *http.Request) (time.Time, time.Time, error) {
	query := r.URL.Query()
	now := time.Now().UTC()
	windowStart := now
	if raw := strings.TrimSpace(query.Get("from")); raw != "" {
		parsed, err := time.Parse(time.RFC3339, raw)
		if err != nil {
			return time.Time{}, time.Time{}, errors.New("from must be RFC3339")
		}
		windowStart = parsed.UTC()
	}

	if raw := strings.TrimSpace(query.Get("to")); raw != "" {
		windowEnd, err := time.Parse(time.RFC3339, raw)
		if err != nil {
			return time.Time{}, time.Time{}, errors.New("to must be RFC3339")
		}
		windowEnd = windowEnd.UTC()
		if !windowEnd.After(windowStart) {
			return time.Time{}, time.Time{}, errors.New("to must be after from")
		}
		if windowEnd.Sub(windowStart) > time.Duration(d.cfg.maxWindowDays)*24*time.Hour {
			return time.Time{}, time.Time{}, fmt.Errorf("requested window exceeds %d days", d.cfg.maxWindowDays)
		}
		return windowStart, windowEnd, nil
	}

	days := intEnvOrDefaultFromQuery(r, "days", d.cfg.queryDays)
	if days < 1 {
		days = d.cfg.queryDays
	}
	if days > d.cfg.maxWindowDays {
		return time.Time{}, time.Time{}, fmt.Errorf("requested window exceeds %d days", d.cfg.maxWindowDays)
	}
	return windowStart, windowStart.Add(time.Duration(days) * 24 * time.Hour), nil
}

func (d *daemon) snapshot() calendarState {
	d.mu.RLock()
	defer d.mu.RUnlock()

	events := make([]event, len(d.state.events))
	copy(events, d.state.events)

	state := d.state
	state.events = events
	return state
}

func parseICS(data []byte) ([]event, error) {
	cal, err := ics.ParseCalendar(bytes.NewReader(data))
	if err != nil {
		return nil, err
	}

	defaultLoc := calendarLocation(cal)

	events := make([]event, 0, len(cal.Events()))
	for _, component := range cal.Events() {
		expanded, err := expandEvent(component, defaultLoc)
		if err != nil {
			return nil, err
		}
		events = append(events, expanded...)
	}

	sort.Slice(events, func(i, j int) bool {
		if events[i].Start.Equal(events[j].Start) {
			return events[i].ID < events[j].ID
		}
		return events[i].Start.Before(events[j].Start)
	})

	return events, nil
}

func expandEvent(component *ics.VEvent, defaultLoc *time.Location) ([]event, error) {
	if propValue(component, ics.ComponentPropertyStatus) == "CANCELLED" {
		return nil, nil
	}
	if propValue(component, ics.ComponentPropertyRecurrenceId) != "" {
		return nil, nil
	}

	uid := propValue(component, ics.ComponentPropertyUniqueId)
	if uid == "" {
		uid = hashBytes([]byte(component.Serialize(nil)))
	}

	start, startAllDay, err := parseICalTime(component.GetProperty(ics.ComponentPropertyDtStart), defaultLoc)
	if err != nil {
		return nil, fmt.Errorf("parse DTSTART for %s: %w", uid, err)
	}

	end, err := parseEventEnd(component, start, startAllDay, defaultLoc)
	if err != nil {
		return nil, fmt.Errorf("parse end for %s: %w", uid, err)
	}

	base := event{
		UID:      uid,
		Title:    fallback(propValue(component, ics.ComponentPropertySummary), "(untitled)"),
		Start:    start.UTC(),
		End:      end.UTC(),
		Created:  eventCreatedTime(component, defaultLoc),
		AllDay:   startAllDay,
		Location: propValue(component, ics.ComponentPropertyLocation),
	}

	rruleValue := propValue(component, ics.ComponentPropertyRrule)
	if rruleValue == "" {
		base.ID = uid
		return []event{base}, nil
	}

	occurrences, err := expandRecurring(component, base, defaultLoc)
	if err != nil {
		return nil, err
	}
	return occurrences, nil
}

func expandRecurring(component *ics.VEvent, base event, defaultLoc *time.Location) ([]event, error) {
	ruleText := propValue(component, ics.ComponentPropertyRrule)
	options, err := rrule.StrToROption(ruleText)
	if err != nil {
		return nil, err
	}
	options.Dtstart = base.Start

	rule, err := rrule.NewRRule(*options)
	if err != nil {
		return nil, err
	}

	windowStart := time.Now().UTC().AddDate(-1, 0, 0)
	windowEnd := time.Now().UTC().AddDate(2, 0, 0)
	if !base.Start.Before(windowStart) {
		windowStart = base.Start.Add(-24 * time.Hour)
	}

	duration := base.End.Sub(base.Start)
	if duration <= 0 {
		duration = time.Minute
	}

	exclusions := parseExdates(component, defaultLoc)
	instances := rule.Between(windowStart, windowEnd, true)
	occurrences := make([]event, 0, len(instances))
	for idx, instanceStart := range instances {
		instanceStart = instanceStart.UTC()
		if exclusions[instanceStart.Unix()] {
			continue
		}

		occurrence := base
		occurrence.Start = instanceStart
		occurrence.End = instanceStart.Add(duration)
		occurrence.ID = fmt.Sprintf("%s#%d", base.UID, idx)
		occurrences = append(occurrences, occurrence)
	}

	return occurrences, nil
}

func parseExdates(component *ics.VEvent, defaultLoc *time.Location) map[int64]bool {
	result := make(map[int64]bool)
	for _, prop := range component.Properties {
		if prop.IANAToken != string(ics.ComponentPropertyExdate) {
			continue
		}
		for _, part := range strings.Split(prop.Value, ",") {
			trimmed := strings.TrimSpace(part)
			if trimmed == "" {
				continue
			}
			clone := prop
			clone.Value = trimmed
			parsed, _, err := parseICalTime(&clone, defaultLoc)
			if err == nil {
				result[parsed.UTC().Unix()] = true
			}
		}
	}
	return result
}

func parseEventEnd(component *ics.VEvent, start time.Time, allDay bool, defaultLoc *time.Location) (time.Time, error) {
	if prop := component.GetProperty(ics.ComponentPropertyDtEnd); prop != nil {
		end, _, err := parseICalTime(prop, defaultLoc)
		return end, err
	}
	if prop := component.GetProperty(ics.ComponentPropertyDuration); prop != nil {
		duration, err := time.ParseDuration(prop.Value)
		if err == nil {
			return start.Add(duration), nil
		}
	}
	if allDay {
		return start.Add(24 * time.Hour), nil
	}
	return start, nil
}

func parseICalTime(prop *ics.IANAProperty, defaultLoc *time.Location) (time.Time, bool, error) {
	if prop == nil {
		return time.Time{}, false, errors.New("missing property")
	}

	value := strings.TrimSpace(prop.Value)
	if value == "" {
		return time.Time{}, false, errors.New("empty value")
	}

	params := normalizeParams(prop.ICalParameters)
	if strings.EqualFold(params["VALUE"], "DATE") || len(value) == len("20060102") {
		loc := defaultLoc
		if loc == nil {
			loc = time.UTC
		}
		parsed, err := time.ParseInLocation("20060102", value, loc)
		if err != nil {
			return time.Time{}, true, err
		}
		return parsed.UTC(), true, nil
	}

	loc := defaultLoc
	if loc == nil {
		loc = time.UTC
	}
	if tzid := params["TZID"]; tzid != "" {
		loaded, err := time.LoadLocation(tzid)
		if err == nil {
			loc = loaded
		}
	}

	if strings.HasSuffix(value, "Z") {
		parsed, err := time.Parse("20060102T150405Z", value)
		if err != nil {
			return time.Time{}, false, err
		}
		return parsed.UTC(), false, nil
	}

	parsed, err := time.ParseInLocation("20060102T150405", value, loc)
	if err != nil {
		return time.Time{}, false, err
	}
	return parsed.UTC(), false, nil
}

func normalizeParams(params map[string][]string) map[string]string {
	result := make(map[string]string, len(params))
	for key, values := range params {
		if len(values) == 0 {
			continue
		}
		result[strings.ToUpper(key)] = values[0]
	}
	return result
}

func filterEvents(events []event, start, end time.Time) []event {
	filtered := make([]event, 0)
	for _, evt := range events {
		if evt.End.Before(start) || !evt.Start.Before(end) {
			continue
		}
		filtered = append(filtered, evt)
	}
	return filtered
}

func extendPageToDayBoundary(events []event, start, end int) int {
	if start >= end || end >= len(events) {
		return end
	}

	lastDayStart := dayStartUTC(events[end-1].Start)
	for end < len(events) {
		if !dayStartUTC(events[end].Start).Equal(lastDayStart) {
			break
		}
		end++
	}

	return end
}

func dayStartUTC(t time.Time) time.Time {
	year, month, day := t.UTC().Date()
	return time.Date(year, month, day, 0, 0, 0, 0, time.UTC)
}

func eventCreatedTime(component *ics.VEvent, defaultLoc *time.Location) time.Time {
	if prop := component.GetProperty(ics.ComponentPropertyCreated); prop != nil {
		if created, _, err := parseICalTime(prop, defaultLoc); err == nil {
			return created.UTC()
		}
	}
	if prop := component.GetProperty(ics.ComponentPropertyDtstamp); prop != nil {
		if stamp, _, err := parseICalTime(prop, defaultLoc); err == nil {
			return stamp.UTC()
		}
	}
	return time.Time{}
}

func calendarLocation(cal *ics.Calendar) *time.Location {
	if cal == nil {
		return time.UTC
	}

	for _, prop := range cal.CalendarProperties {
		if prop.IANAToken != string(ics.PropertyXWRTimezone) {
			continue
		}

		tzid := strings.TrimSpace(prop.Value)
		if tzid == "" {
			break
		}

		if loaded, err := time.LoadLocation(tzid); err == nil {
			return loaded
		}
		break
	}

	return time.UTC
}

func propValue(component *ics.VEvent, property ics.ComponentProperty) string {
	prop := component.GetProperty(property)
	if prop == nil {
		return ""
	}
	return strings.TrimSpace(prop.Value)
}

func hashBytes(data []byte) string {
	sum := sha1.Sum(data)
	return hex.EncodeToString(sum[:8])
}

func ageSeconds(t time.Time) int64 {
	if t.IsZero() {
		return 0
	}
	return int64(time.Since(t).Seconds())
}

func unixOrZero(t time.Time) int64 {
	if t.IsZero() {
		return 0
	}
	return t.Unix()
}

func parseCursor(raw string) (int, error) {
	if strings.TrimSpace(raw) == "" {
		return 0, nil
	}
	value, err := strconv.Atoi(raw)
	if err != nil || value < 0 {
		return 0, errors.New("invalid cursor")
	}
	return value, nil
}

func intEnvOrDefault(name string, fallback int) int {
	raw := strings.TrimSpace(os.Getenv(name))
	if raw == "" {
		return fallback
	}
	value, err := strconv.Atoi(raw)
	if err != nil {
		return fallback
	}
	return value
}

func intEnvOrDefaultFromQuery(r *http.Request, name string, fallback int) int {
	raw := strings.TrimSpace(r.URL.Query().Get(name))
	if raw == "" {
		return fallback
	}
	value, err := strconv.Atoi(raw)
	if err != nil {
		return fallback
	}
	return value
}

func durationEnvOrDefault(name string, fallback time.Duration) time.Duration {
	raw := strings.TrimSpace(os.Getenv(name))
	if raw == "" {
		return fallback
	}
	value, err := time.ParseDuration(raw)
	if err != nil {
		return fallback
	}
	return value
}

func envOrDefault(name, fallback string) string {
	if value := strings.TrimSpace(os.Getenv(name)); value != "" {
		return value
	}
	return fallback
}

func fallback(value, fallback string) string {
	if strings.TrimSpace(value) == "" {
		return fallback
	}
	return value
}

func loggingMiddleware(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		start := time.Now()
		next.ServeHTTP(w, r)
		log.Printf("%s %s %s", r.Method, r.URL.RequestURI(), time.Since(start).Round(time.Millisecond))
	})
}

func writeJSON(w http.ResponseWriter, status int, payload any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	if err := json.NewEncoder(w).Encode(payload); err != nil {
		log.Printf("write response failed: %v", err)
	}
}
