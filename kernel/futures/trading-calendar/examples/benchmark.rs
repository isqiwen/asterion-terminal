//! Synthetic in-process lookup benchmark, not an end-to-end market workload.
use asterion_trading_calendar::{Boundary, Calendar};
use chrono::{Duration, NaiveDate};
use serde_json::{Value, json};
use std::{hint::black_box, time::Instant};

fn main() {
    let start = NaiveDate::from_ymd_opt(2010, 1, 1).unwrap();
    let end = start + Duration::days(3699);
    let mut value: Value =
        serde_json::from_str(include_str!("../tests/fixtures/valid.json")).unwrap();
    value["title"] = json!("Synthetic 3700-day lookup benchmark");
    value["calendar_source"] =
        json!("Synthetic explicit all-open calendar, no real-market evidence");
    value["night_source"] = json!("Synthetic all-open nights");
    value["calendar"] = Value::Array(
        (0..3700)
            .map(|i| json!({"date":start+Duration::days(i),"is_open":true,"night_open":true}))
            .collect(),
    );
    value["periods"][0]["start"] = json!(start + Duration::days(1));
    value["periods"][0]["end"] = json!(end);
    let setup = Instant::now();
    let calendar = Calendar::from_value(value).unwrap();
    let build = setup.elapsed();
    let days: Vec<_> = (1..3700).map(|i| start + Duration::days(i)).collect();
    let stamps: Vec<_> = days
        .iter()
        .map(|day| chrono::DateTime::parse_from_rfc3339(&format!("{day}T09:01:00+08:00")).unwrap())
        .collect();
    // Warm one call to exclude lazy regular-expression compilation.
    black_box(calendar.daily("SHFE.au2506", days[0]).unwrap());
    black_box(
        calendar
            .resolve("SHFE.au2506", stamps[0], Boundary::Event)
            .unwrap(),
    );
    let n = 10_000;
    let start = Instant::now();
    for i in 0..n {
        black_box(
            calendar
                .daily(black_box("SHFE.au2506"), black_box(days[i % days.len()]))
                .unwrap(),
        );
    }
    let daily = start.elapsed();
    let start = Instant::now();
    for i in 0..n {
        black_box(
            calendar
                .resolve(
                    black_box("SHFE.au2506"),
                    black_box(stamps[i % stamps.len()]),
                    Boundary::Event,
                )
                .unwrap(),
        );
    }
    let resolve = start.elapsed();
    println!(
        "calendar_days=3700 spans={} iterations={n} construction_ms={:.3} daily_total_ms={:.3} daily_ns_per_call={:.1} resolve_total_ms={:.3} resolve_ns_per_call={:.1}",
        calendar.spans().len(),
        build.as_secs_f64() * 1000.,
        daily.as_secs_f64() * 1000.,
        daily.as_nanos() as f64 / n as f64,
        resolve.as_secs_f64() * 1000.,
        resolve.as_nanos() as f64 / n as f64
    );
}
