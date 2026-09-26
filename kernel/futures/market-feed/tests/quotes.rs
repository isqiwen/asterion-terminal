use asterion_connections::Subscription;
use asterion_market_feed::{QuoteBook, QuoteEvent, invoke};
use serde_json::{Value, json};

fn event() -> QuoteEvent {
    serde_json::from_value(json!({"exchange":"SHFE","symbol":"au2612","last":101.2,"previous_settlement":100.,"high":110.,"low":90.,"volume":10,"open_interest":20.,"trading_day":"20260922","action_day":"20260922","source_time":"09:00:00","event_at":100.,"received_at":100.})).unwrap()
}
fn book() -> QuoteBook {
    let mut book = QuoteBook::default();
    book.bind(1).unwrap();
    book.subscriptions(vec![Subscription {
        exchange: "SHFE".into(),
        symbol: "au2612".into(),
    }])
    .unwrap();
    book
}
fn first(book: &QuoteBook, generation: u64, ready: bool, now: f64) -> Value {
    book.snapshot(generation, ready, now).unwrap()["quotes"][0].clone()
}

#[test]
fn decimal_change_and_current_schema_round_trip() {
    let mut book = book();
    book.ingest(1, 1, true, event(), 100.).unwrap();
    let quote = first(&book, 1, true, 100.);
    assert_eq!(quote["change"], 1.2);
    assert_eq!(quote["change_percent"], 1.2);
    assert_eq!(quote["status"], "current");
    invoke("validate", json!({"model":"Quote","value":quote})).unwrap();
    let mut invalid = event();
    invalid.last = Some(f64::NAN);
    assert!(book.ingest(1, 1, true, invalid, 100.).is_err());
    assert_eq!(first(&book, 1, true, 100.)["last"], 101.2);
}

#[test]
fn current_quote_contract_requires_explicit_nullable_fields() {
    let value = serde_json::to_value(event()).unwrap();
    for field in [
        "last",
        "previous_settlement",
        "high",
        "low",
        "volume",
        "open_interest",
        "event_at",
    ] {
        let mut missing = value.clone();
        missing.as_object_mut().unwrap().remove(field);
        assert!(invoke("validate", json!({"model":"QuoteEvent","value":missing})).is_err());
        let mut unknown = value.clone();
        unknown[field] = Value::Null;
        invoke("validate", json!({"model":"QuoteEvent","value":unknown})).unwrap();
    }
    assert!(invoke("schema", json!({"unsupported":true})).is_err());
}

#[test]
fn reconnect_never_presents_the_previous_generation_as_current() {
    let mut book = book();
    book.ingest(1, 1, true, event(), 100.).unwrap();
    assert_eq!(first(&book, 2, true, 100.)["status"], "not_updated");
    let mut next = event();
    next.event_at = Some(99.);
    next.last = Some(90.);
    book.ingest(1, 2, true, next.clone(), 100.).unwrap();
    assert_eq!(first(&book, 2, true, 100.)["last"], 101.2);
    book.ingest(2, 2, true, next, 100.).unwrap();
    assert_eq!(first(&book, 2, true, 100.)["last"], 90.);
    assert_eq!(first(&book, 2, true, 100.)["status"], "current");
}

#[test]
fn stale_future_and_unknown_timestamps_have_explicit_status() {
    for (stamp, received, ready, expected) in [
        (Some(100.), 100., true, "current"),
        (Some(100.), 69., true, "not_updated"),
        (Some(69.), 100., true, "delayed"),
        (Some(106.), 100., true, "time_ahead"),
        (None, 100., true, "time_unknown"),
        (Some(100.), 100., false, "disconnected"),
    ] {
        let mut book = book();
        let mut value = event();
        value.event_at = stamp;
        value.received_at = received;
        book.ingest(1, 1, true, value, 100.).unwrap();
        let quote = first(&book, 1, ready, 100.);
        assert_eq!(quote["status"], expected);
        assert_eq!(quote["stale"], expected != "current");
    }
}

#[test]
fn late_and_wrong_exchange_ticks_cannot_replace_accepted_values() {
    let mut book = book();
    book.ingest(1, 1, true, event(), 100.).unwrap();
    let mut late = event();
    late.event_at = Some(99.);
    late.last = Some(5.);
    book.ingest(1, 1, true, late, 100.).unwrap();
    assert_eq!(first(&book, 1, true, 100.)["last"], 101.2);
    let mut wrong = event();
    wrong.exchange = "DCE".into();
    book.ingest(1, 1, true, wrong, 100.).unwrap();
    assert!(
        book.snapshot(1, true, 100.).unwrap()["subscription_errors"]
            .get("au2612")
            .is_some()
    );
    let mut future = event();
    future.event_at = Some(110.);
    book.ingest(1, 1, true, future, 100.).unwrap();
    book.ingest(1, 1, true, event(), 100.).unwrap();
    assert_eq!(first(&book, 1, true, 100.)["event_at"], 100.);
}

#[test]
fn subscriptions_and_revisions_bound_state_and_apply_atomically() {
    let mut book = book();
    book.ingest(1, 1, true, event(), 100.).unwrap();
    assert!(
        book.subscriptions(vec![Subscription {
            exchange: "SHFE".into(),
            symbol: "au0000".into()
        }])
        .is_err()
    );
    assert_eq!(first(&book, 1, true, 100.)["last"], 101.2);
    book.error(1, 1, "not-subscribed", "failure").unwrap();
    assert_eq!(
        book.snapshot(1, true, 100.).unwrap()["subscription_errors"],
        json!({})
    );
    book.bind(2).unwrap();
    assert_eq!(book.snapshot(1, true, 100.).unwrap()["quotes"], json!([]));
    book.ingest(1, 1, true, event(), 100.).unwrap();
    book.subscriptions(vec![]).unwrap();
    assert_eq!(book.snapshot(1, true, 100.).unwrap()["quotes"], json!([]));
}

#[test]
fn same_session_reconnect_requires_a_new_quote_and_clears_previous_errors() {
    let mut book = book();
    book.ingest(1, 1, true, event(), 100.).unwrap();
    book.error(1, 1, "au2612", "previous failure").unwrap();
    assert_eq!(
        book.snapshot(2, true, 100.).unwrap()["subscription_errors"],
        json!({})
    );
    book.connection_event(1, 1, "reconnecting");
    book.connection_event(1, 1, "connected");
    assert_eq!(first(&book, 1, true, 100.)["status"], "not_updated");
    assert_eq!(
        book.snapshot(1, true, 100.).unwrap()["subscription_errors"],
        json!({})
    );
    let mut fresh = event();
    fresh.event_at = Some(99.);
    fresh.last = Some(99.);
    book.ingest(1, 1, true, fresh, 100.).unwrap();
    assert_eq!(first(&book, 1, true, 100.)["status"], "current");
    assert_eq!(first(&book, 1, true, 100.)["last"], 99.);
}
