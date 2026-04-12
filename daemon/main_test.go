package main

import (
	"testing"
	"time"
)

func TestParseICSExpandsRecurringEvents(t *testing.T) {
	icsData := []byte("BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\nUID:daily-1\r\nDTSTART:20260409T120000Z\r\nDTEND:20260409T123000Z\r\nRRULE:FREQ=DAILY;COUNT=3\r\nSUMMARY:Standup\r\nDESCRIPTION:Daily sync with the team\r\nLOCATION:Room A\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n")

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
	if events[0].Description != "Daily sync with the team" {
		t.Fatalf("unexpected description: %q", events[0].Description)
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

func TestParseICSRecurringAllDayUsesCalendarLocalDays(t *testing.T) {
	icsData := []byte("BEGIN:VCALENDAR\r\nVERSION:2.0\r\nX-WR-TIMEZONE:Europe/Copenhagen\r\nBEGIN:VEVENT\r\nUID:allday-1\r\nDTSTART;VALUE=DATE:20260412\r\nDTEND;VALUE=DATE:20260413\r\nRRULE:FREQ=DAILY;COUNT=2\r\nSUMMARY:All day test\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n")

	events, err := parseICS(icsData)
	if err != nil {
		t.Fatalf("parseICS returned error: %v", err)
	}
	if len(events) != 2 {
		t.Fatalf("expected 2 events, got %d", len(events))
	}

	loc, err := time.LoadLocation("Europe/Copenhagen")
	if err != nil {
		t.Fatalf("load location: %v", err)
	}

	for idx, evt := range events {
		if !evt.AllDay {
			t.Fatalf("event %d should be all-day", idx)
		}

		startLocal := evt.Start.In(loc)
		endLocal := evt.End.In(loc)
		if startLocal.Hour() != 0 || startLocal.Minute() != 0 || startLocal.Second() != 0 {
			t.Fatalf("event %d local start not at midnight: %s", idx, startLocal)
		}
		if endLocal.Hour() != 0 || endLocal.Minute() != 0 || endLocal.Second() != 0 {
			t.Fatalf("event %d local end not at midnight: %s", idx, endLocal)
		}
		if endLocal.Sub(startLocal) != 24*time.Hour {
			t.Fatalf("event %d expected one local day, got %s", idx, endLocal.Sub(startLocal))
		}
	}

	if got := events[0].Start.In(loc).Format("2006-01-02"); got != "2026-04-12" {
		t.Fatalf("unexpected first local day: %s", got)
	}
	if got := events[1].Start.In(loc).Format("2006-01-02"); got != "2026-04-13" {
		t.Fatalf("unexpected second local day: %s", got)
	}
}
