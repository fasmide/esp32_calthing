package main

import (
	"testing"
	"time"
)

func TestParseICSExpandsRecurringEvents(t *testing.T) {
	icsData := []byte("BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\nUID:daily-1\r\nDTSTART:20260409T120000Z\r\nDTEND:20260409T123000Z\r\nRRULE:FREQ=DAILY;COUNT=3\r\nSUMMARY:Standup\r\nLOCATION:Room A\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n")

	events, err := parseICS(icsData)
	if err != nil {
		t.Fatalf("parseICS returned error: %v", err)
	}
	if len(events) != 3 {
		t.Fatalf("expected 3 events, got %d", len(events))
	}
	if events[0].Title != "Standup" {
		t.Fatalf("unexpected title: %q", events[0].Title)
	}
	if events[1].Start.Sub(events[0].Start) != 24*time.Hour {
		t.Fatalf("expected daily recurrence spacing, got %s", events[1].Start.Sub(events[0].Start))
	}
	if events[0].Location != "Room A" {
		t.Fatalf("unexpected location: %q", events[0].Location)
	}
}

func TestFilterEventsReturnsOverlappingRange(t *testing.T) {
	base := time.Date(2026, 4, 9, 12, 0, 0, 0, time.UTC)
	events := []event{
		{ID: "1", Start: base, End: base.Add(time.Hour)},
		{ID: "2", Start: base.Add(2 * time.Hour), End: base.Add(3 * time.Hour)},
		{ID: "3", Start: base.Add(4 * time.Hour), End: base.Add(5 * time.Hour)},
	}

	filtered := filterEvents(events, base.Add(30*time.Minute), base.Add(150*time.Minute))
	if len(filtered) != 2 {
		t.Fatalf("expected 2 events, got %d", len(filtered))
	}
	if filtered[0].ID != "1" || filtered[1].ID != "2" {
		t.Fatalf("unexpected events returned: %+v", filtered)
	}
}
