use super::*;
use serde_json::json;
fn fixture() -> Value {
    serde_json::from_str(include_str!("../tests/fixtures/valid.json")).unwrap()
}
fn date(value: &str) -> NaiveDate {
    value.parse().unwrap()
}
fn dt(value: &str) -> DateTime<FixedOffset> {
    DateTime::parse_from_rfc3339(value).unwrap()
}
const CONTRACT: &str = "SHFE.au2506";
#[test]
fn wire_roundtrip_and_python_fingerprint() {
    let calendar = Calendar::from_value(fixture()).unwrap();
    assert_eq!(output(calendar.spec()).unwrap(), fixture());
    assert_eq!(
        calendar.id(),
        "b5b5c5113c42aed3aec9c86b24ae19c1fe17c639e3990e4c2d12e6eb04e75939"
    );
    let snapshot = invoke("snapshot", json!({"spec":fixture()})).unwrap();
    invoke(
        "validate",
        json!({"model":"TimeVersion","value":snapshot.clone()}),
    )
    .unwrap();
    let mut damaged = snapshot;
    damaged["spec"]["title"] = json!("changed");
    assert!(invoke("validate", json!({"model":"TimeVersion","value":damaged})).is_err());
}
#[test]
fn actual_calendar_controls_night_sessions_and_target_trading_days() {
    let calendar = Calendar::from_value(fixture()).unwrap();
    for (stamp, day) in [
        ("2025-04-11T21:00:00+08:00", "2025-04-14"),
        ("2025-04-12T01:15:00+08:00", "2025-04-14"),
        ("2025-04-11T17:15:00Z", "2025-04-14"),
        ("2025-04-14T09:00:00+08:00", "2025-04-14"),
        ("2025-04-07T09:00:00+08:00", "2025-04-07"),
        ("2025-04-07T21:00:00+08:00", "2025-04-08"),
    ] {
        assert_eq!(
            calendar
                .resolve(CONTRACT, dt(stamp), Boundary::Event)
                .unwrap()
                .trading_day,
            date(day),
            "{stamp}"
        );
    }
    for stamp in [
        "2025-04-03T21:00:00+08:00",
        "2025-04-06T21:00:00+08:00",
        "2025-04-13T01:00:00+08:00",
        "2025-04-07T10:20:00+08:00",
        "2025-04-07T12:00:00+08:00",
        "2025-04-15T09:00:00+08:00",
    ] {
        assert!(
            calendar
                .resolve(CONTRACT, dt(stamp), Boundary::Event)
                .is_err(),
            "{stamp}"
        );
    }
    assert_eq!(
        calendar.daily(CONTRACT, date("2025-04-07")).unwrap().len(),
        3
    );
    let monday = calendar.daily(CONTRACT, date("2025-04-14")).unwrap();
    assert_eq!(monday.len(), 4);
    assert_eq!(monday[0].start.date_naive(), date("2025-04-11"));
}
#[test]
fn boundaries_distinguish_auction_events_and_continuous_bars() {
    let calendar = Calendar::from_value(fixture()).unwrap();
    let auction = dt("2025-04-11T20:58:00+08:00");
    assert_eq!(
        calendar
            .resolve(CONTRACT, auction, Boundary::Event)
            .unwrap()
            .phase,
        Phase::Auction
    );
    assert!(
        calendar
            .resolve(CONTRACT, auction, Boundary::BarEnd)
            .is_err()
    );
    let boundary = dt("2025-04-11T21:00:00+08:00");
    assert_eq!(
        calendar
            .resolve(CONTRACT, boundary, Boundary::Event)
            .unwrap()
            .phase,
        Phase::Continuous
    );
    assert!(
        calendar
            .resolve(CONTRACT, boundary, Boundary::BarEnd)
            .is_err()
    );
    let end = dt("2025-04-12T02:30:00+08:00");
    assert!(calendar.resolve(CONTRACT, end, Boundary::Event).is_err());
    assert_eq!(
        calendar
            .resolve(CONTRACT, end, Boundary::BarEnd)
            .unwrap()
            .session,
        Session::Night
    );
}
#[test]
fn valid_bar_must_fit_one_continuous_session_and_exchange_day() {
    let calendar = Calendar::from_value(fixture()).unwrap();
    calendar
        .validate_bar(
            CONTRACT,
            dt("2025-04-12T02:30:00+08:00"),
            date("2025-04-14"),
            60,
            BarBoundary::BarEnd,
        )
        .unwrap();
    for seconds in [0, -60, i64::MAX] {
        assert!(
            calendar
                .validate_bar(
                    CONTRACT,
                    dt("2025-04-12T02:30:00+08:00"),
                    date("2025-04-14"),
                    seconds,
                    BarBoundary::BarEnd
                )
                .is_err()
        );
    }
    calendar
        .validate_bar(
            CONTRACT,
            dt("2025-04-11T21:00:00+08:00"),
            date("2025-04-14"),
            60,
            BarBoundary::BarStart,
        )
        .unwrap();
    for (stamp, day, seconds, boundary) in [
        (
            "2025-04-12T02:30:00+08:00",
            "2025-04-12",
            60,
            BarBoundary::BarEnd,
        ),
        (
            "2025-04-07T10:31:00+08:00",
            "2025-04-07",
            120,
            BarBoundary::BarEnd,
        ),
        (
            "2025-04-07T10:14:00+08:00",
            "2025-04-07",
            120,
            BarBoundary::BarStart,
        ),
        (
            "2025-04-11T20:59:00+08:00",
            "2025-04-14",
            60,
            BarBoundary::BarStart,
        ),
    ] {
        assert!(
            calendar
                .validate_bar(CONTRACT, dt(stamp), date(day), seconds, boundary)
                .is_err(),
            "{stamp}"
        );
    }
}
#[test]
fn rule_changes_apply_to_target_day_and_exceptions_replace_both_sessions() {
    let mut changed = fixture();
    let mut period = changed["periods"][0].clone();
    changed["periods"][0]["end"] = json!("2025-04-13");
    period["start"] = json!("2025-04-14");
    period["night"][1]["end"] = json!("23:00:00");
    period["night"][1]["end_offset"] = json!(0);
    changed["periods"].as_array_mut().unwrap().push(period);
    let calendar = Calendar::from_value(changed).unwrap();
    assert!(
        calendar
            .resolve(CONTRACT, dt("2025-04-11T22:00:00+08:00"), Boundary::Event)
            .is_ok()
    );
    assert!(
        calendar
            .resolve(CONTRACT, dt("2025-04-12T01:00:00+08:00"), Boundary::Event)
            .is_err()
    );
    let mut suspended = fixture();
    suspended["exceptions"] =
        json!([{"trading_day":"2025-04-14","source":"verified suspension","day":[],"night":[]}]);
    let calendar = Calendar::from_value(suspended).unwrap();
    assert!(calendar.daily(CONTRACT, date("2025-04-14")).is_err());
    assert!(
        calendar
            .resolve(CONTRACT, dt("2025-04-11T22:00:00+08:00"), Boundary::Event)
            .is_err()
    );
    let mut day_only = fixture();
    day_only["periods"][0]["night"] = json!([]);
    let calendar = Calendar::from_value(day_only).unwrap();
    assert_eq!(
        calendar.daily(CONTRACT, date("2025-04-14")).unwrap().len(),
        3
    );
}
#[test]
fn reject_calendar_gaps_rule_gaps_duplicate_exceptions_and_overlaps() {
    let mut gap = fixture();
    gap["calendar"].as_array_mut().unwrap().remove(2);
    assert!(Calendar::from_value(gap).is_err());
    let mut duplicate = fixture();
    duplicate["calendar"][2] = duplicate["calendar"][1].clone();
    assert!(Calendar::from_value(duplicate).is_err());
    let mut bad_night = fixture();
    bad_night["calendar"][2]["night_open"] = json!(true);
    assert!(Calendar::from_value(bad_night).is_err());
    let mut overlap = fixture();
    overlap["periods"][0]["day"][1]["start"] = json!("10:00:00");
    assert!(Calendar::from_value(overlap).is_err());
    let mut crossing = fixture();
    crossing["periods"][0]["night"][1]["end"] = json!("10:00:00");
    assert!(Calendar::from_value(crossing).is_err());
    let mut gap = fixture();
    let mut period = gap["periods"][0].clone();
    gap["periods"][0]["end"] = json!("2025-04-11");
    period["start"] = json!("2025-04-13");
    gap["periods"].as_array_mut().unwrap().push(period);
    assert!(Calendar::from_value(gap).is_err());
    let e = json!({"trading_day":"2025-04-14","source":"suspension","day":[],"night":[]});
    let mut duplicate = fixture();
    duplicate["exceptions"] = json!([e.clone(), e]);
    assert!(Calendar::from_value(duplicate).is_err());
    let mut closed = fixture();
    closed["exceptions"] =
        json!([{"trading_day":"2025-04-13","source":"closed","day":[],"night":[]}]);
    assert!(Calendar::from_value(closed).is_err());
}
#[test]
fn strict_fields_no_missing_night_flag_no_naive_timestamp() {
    for pointer in ["/schema_version", "/exceptions", "/calendar/0/night_open"] {
        let mut value = fixture();
        let (parent, key) = pointer.rsplit_once('/').unwrap();
        value
            .pointer_mut(parent)
            .unwrap()
            .as_object_mut()
            .unwrap()
            .remove(key);
        assert!(Calendar::from_value(value).is_err(), "{pointer}");
    }
    let mut extra = fixture();
    extra["periods"][0]["day"][0]["extra"] = json!(1);
    assert!(Calendar::from_value(extra).is_err());
    for flag in [json!(1), json!("true"), json!(null)] {
        let mut value = fixture();
        value["calendar"][0]["night_open"] = flag;
        assert!(Calendar::from_value(value).is_err());
    }
    let calendar = Calendar::from_value(fixture()).unwrap();
    for contract in ["DCE.au2506", "SHFE.rb2506", "SHFE.AU", "shfe.au2506"] {
        assert!(calendar.check_contract(contract).is_err());
    }
    assert!(invoke("resolve",json!({"spec":fixture(),"contract":CONTRACT,"stamp":"2025-04-14T09:00:00","boundary":"event"})).is_err());
    assert!(invoke("resolve",json!({"spec":fixture(),"contract":CONTRACT,"stamp":"2025-04-14T09:00:00Z","boundary":"guess"})).is_err());
}
#[test]
fn cached_lookup_matches_an_exhaustive_oracle_at_every_session_edge() {
    let calendar = Calendar::from_value(fixture()).unwrap();
    for span in calendar.spans() {
        for edge in [span.start, span.end] {
            for delta in [-1, 0, 1] {
                let stamp = edge + Duration::seconds(delta);
                let wall = stamp.with_timezone(&Shanghai).naive_local();
                for boundary in [Boundary::Event, Boundary::BarEnd] {
                    let expected: Vec<_> = calendar
                        .spans()
                        .iter()
                        .filter(|s| match boundary {
                            Boundary::Event => {
                                s.start.naive_local() <= wall && wall < s.end.naive_local()
                            }
                            Boundary::BarEnd => {
                                s.start.naive_local() < wall
                                    && wall <= s.end.naive_local()
                                    && s.phase == Phase::Continuous
                            }
                        })
                        .collect();
                    let actual = calendar.resolve(CONTRACT, stamp, boundary);
                    assert_eq!(
                        actual.ok(),
                        expected.first().copied(),
                        "{stamp:?} {boundary:?}"
                    );
                }
            }
        }
    }
}
#[test]
fn historical_zone_uses_actual_shanghai_rules_and_fold_zero() {
    assert_eq!(
        local(date("1991-07-01").and_hms_opt(9, 0, 0).unwrap())
            .unwrap()
            .offset()
            .local_minus_utc(),
        9 * 3600
    );
    assert_eq!(
        local(date("2025-07-01").and_hms_opt(9, 0, 0).unwrap())
            .unwrap()
            .offset()
            .local_minus_utc(),
        8 * 3600
    );
    assert_eq!(
        local(date("1991-04-14").and_hms_opt(2, 30, 0).unwrap())
            .unwrap()
            .offset()
            .local_minus_utc(),
        8 * 3600
    );
    assert_eq!(
        local(date("1991-09-15").and_hms_opt(1, 30, 0).unwrap())
            .unwrap()
            .offset()
            .local_minus_utc(),
        9 * 3600
    );
}
#[test]
fn schema_includes_strict_domain_objects_and_temporal_formats() {
    let schema = schema();
    for name in [
        "CalendarDay",
        "Slot",
        "Period",
        "ExceptionDay",
        "TimeSpec",
        "Span",
        "TimeVersion",
    ] {
        assert_eq!(
            schema["$defs"][name]["additionalProperties"], false,
            "{name}"
        );
    }
    assert_eq!(
        schema["$defs"]["Span"]["properties"]["start"]["format"],
        "date-time"
    );
}

#[test]
fn typed_and_json_temporal_boundaries_reject_invalid_precision_and_clock() {
    let calendar = Calendar::from_value(fixture()).unwrap();
    let nanos = dt("2025-04-14T09:00:00.000000001+08:00");
    assert!(calendar.resolve(CONTRACT, nanos, Boundary::Event).is_err());
    for value in ["09:00:60", "24:00:00", "9:00:00"] {
        let mut spec = fixture();
        spec["periods"][0]["day"][0]["start"] = json!(value);
        assert!(Calendar::from_value(spec).is_err(), "{value}");
    }
    let legacy_offset = local(date("1800-01-01").and_hms_opt(9, 0, 0).unwrap()).unwrap();
    let span = Span {
        start: legacy_offset,
        end: legacy_offset + Duration::hours(1),
        trading_day: date("1800-01-01"),
        session: Session::Day,
        phase: Phase::Continuous,
    };
    assert_eq!(output(span).unwrap()["start"], "1800-01-01T09:00:00+08:05");
}
