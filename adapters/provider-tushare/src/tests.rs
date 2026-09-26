use super::*;
use asterion_data_store::provider::checked_plan;
use std::io::{BufRead, BufReader, Read, Write};
use std::net::TcpListener;
use std::sync::{Arc, Mutex};

const SECRET: &str = "synthetic-provider-token-never-log";

fn request(dataset: &str, changes: Value) -> SyncRequest {
    let mut values = json!({
        "command_id": "sync", "provider": "tushare", "dataset": dataset, "exchange": "SHFE",
        "symbol": if dataset == "daily" { "RB2610.SHF" } else { "" },
        "start": if dataset == "contracts" { Value::Null } else { json!("2024-01-02") },
        "end": if dataset == "contracts" { Value::Null } else { json!("2024-01-02") },
    });
    for (key, value) in changes.as_object().expect("changes") {
        values[key] = value.clone();
    }
    serde_json::from_value(values).expect("request")
}

fn raw(partition: &Partition) -> Value {
    let values = json!({
        "ts_code": "RB2610.SHF", "trade_date": "20240102", "open": 3200, "high": 3220,
        "low": 3190, "close": 3210, "vol": 100, "amount": 321, "oi": 200, "settle": 3205,
        "pre_settle": 3200, "pre_close": 3201, "oi_chg": 1, "exchange": "SHFE",
        "cal_date": "20240102", "is_open": 1, "pretrade_date": "20231229", "symbol": "RB2610",
        "name": "测试螺纹", "fut_code": "RB", "d_month": "202610", "last_ddate": "20261020",
        "list_date": "20230101", "delist_date": "20261015", "per_unit": 10,
        "trade_unit": "吨", "quote_unit": "元/吨",
    });
    Value::Object(
        partition
            .fields
            .iter()
            .map(|f| (f.clone(), values.get(f).cloned().unwrap_or(Value::Null)))
            .collect(),
    )
}

fn normalized(dataset: &str, mutate: impl FnOnce(&mut Value)) -> Result<Vec<Row>> {
    let tushare = Tushare::default();
    let request = request(dataset, json!({}));
    let mut row = raw(&tushare.plan(&request).expect("plan")[0]);
    mutate(&mut row);
    tushare.normalize(&request, &[row])
}

#[test]
fn bounded_plans_cover_the_request() {
    let tushare = Tushare::default();
    let plan = tushare
        .plan(&request(
            "daily",
            json!({"start": "2024-01-01", "end": "2024-03-15"}),
        ))
        .expect("plan");
    assert_eq!(plan.len(), 3);
    assert_eq!(plan[0].params["end_date"], "20240131");
    assert_eq!(plan[1].params["start_date"], "20240201");
    assert_eq!(plan[2].params["end_date"], "20240315");
    assert!(
        checked_plan(
            &request("daily", json!({"start": "2024-01-01", "end": "2024-03-15"})),
            &plan
        )
        .is_ok()
    );
    let contracts = tushare
        .plan(&request("contracts", json!({})))
        .expect("contracts");
    assert_eq!(contracts[0].params["fut_type"], "1");
    assert_eq!(contracts[0].limit, 10000);
    for (dataset, changes) in [
        ("daily", json!({"symbol": "RB.SHF"})),
        ("daily", json!({"symbol": "RB2610.DCE"})),
        ("calendar", json!({"exchange": "GFEX"})),
        ("contracts", json!({"symbol": "RB2610.SHF"})),
        ("mapping", json!({"symbol": "RB2610.SHF"})),
        ("calendar", json!({"symbol": "RB2610.SHF"})),
        ("daily", json!({"frequency": "1m"})),
    ] {
        assert!(
            tushare.plan(&request(dataset, changes.clone())).is_err(),
            "{dataset} {changes}"
        );
    }
}

#[test]
fn minute_plan_uses_exchange_wall_time() {
    let tushare = Tushare::default();
    let request = request(
        "minute",
        json!({"symbol": "RB2610.SHF", "frequency": "5m",
               "window": {"start": "2024-01-01T13:00:00Z", "end": "2024-01-02T07:00:00Z"}}),
    );
    let plan = tushare.plan(&request).expect("plan");
    assert_eq!(plan[0].api, "ft_mins");
    assert_eq!(plan[0].params["freq"], "5min");
    assert_eq!(plan[0].params["start_date"], "2024-01-01 21:00:00");
    assert_eq!(plan[0].params["end_date"], "2024-01-02 15:00:00");
    let rows = [
        json!({"ts_code": "RB2610.SHF", "trade_time": "2024-01-02 09:05:00", "open": 1, "close": 2,
               "high": 2, "low": 1, "vol": 3, "amount": 4.5, "oi": 6}),
        json!({"ts_code": "RB2610.SHF", "trade_time": "2024-01-01 21:05:00", "open": 1, "close": 2,
               "high": 2, "low": 1, "vol": 3, "amount": 4.5, "oi": 6}),
    ];
    let normalized = tushare.normalize(&request, &rows).expect("rows");
    assert_eq!(
        normalized[0].get("event_time"),
        Some(&json!("2024-01-01T21:05:00+08:00"))
    );
    assert_eq!(normalized[1].get("amount"), Some(&json!("4.5")));
    let late = json!({"ts_code": "RB2610.SHF", "trade_time": "2024-01-02 15:05:00", "open": 1,
                      "close": 2, "high": 2, "low": 1, "vol": 3, "amount": 4, "oi": 6});
    for bad in [vec![rows[0].clone(), rows[0].clone()], vec![late]] {
        assert_eq!(
            tushare.normalize(&request, &bad).unwrap_err(),
            "分钟来源记录缺失、重复或不符合请求"
        );
    }
}

#[test]
fn normalized_rows_keep_provider_semantics() {
    let daily = normalized("daily", |_| {}).expect("daily");
    assert_eq!(
        serde_json::to_value(&daily[0]).expect("json"),
        json!({
        "symbol": "RB2610.SHF", "contract": "SHFE.RB2610", "exchange": "SHFE",
        "trading_day": "2024-01-02", "open": "3200", "high": "3220", "low": "3190",
        "close": "3210", "settle": "3205", "pre_settle": "3200", "pre_close": "3201",
        "oi_chg": "1", "vol": "100", "amount": "321", "oi": "200"})
    );
    let contract = normalized("contracts", |_| {}).expect("contracts");
    assert_eq!(contract[0].get("delivery_month"), Some(&json!("2026-10")));
    assert_eq!(contract[0].get("suggested_multiplier"), Some(&json!("10")));
    assert_eq!(contract[0].get("rules_status"), Some(&json!("INCOMPLETE")));
    let calendar = normalized("calendar", |_| {}).expect("calendar");
    assert_eq!(
        calendar[0].get("previous_trading_day"),
        Some(&json!("2023-12-29"))
    );
    assert_eq!(calendar[0].get("is_open"), Some(&json!(1)));
    type Case = (&'static str, fn(&mut Value), &'static str);
    let cases: [Case; 9] = [
        (
            "daily",
            |r| r["high"] = json!(2),
            "日线 OHLC 价格边界不一致",
        ),
        (
            "daily",
            |r| r["ts_code"] = json!("CU2610.SHF"),
            "返回行情的合约与请求不一致",
        ),
        (
            "daily",
            |r| r["trade_date"] = json!("20240103"),
            "返回日期超出请求范围",
        ),
        (
            "daily",
            |r| r["vol"] = json!(-1),
            "数据校验失败：数值缺失或非法",
        ),
        (
            "daily",
            |r| {
                r.as_object_mut().expect("row").remove("close");
            },
            "数据校验失败：缺失必需字段",
        ),
        (
            "contracts",
            |r| r["d_month"] = json!("2610"),
            "交割月份必须提供完整 YYYYMM，不能从代码推断年份",
        ),
        (
            "contracts",
            |r| r["d_month"] = json!("202710"),
            "合约代码与来源完整交割年月不一致",
        ),
        (
            "contracts",
            |r| r["fut_code"] = json!("HC"),
            "合约代码与来源品种不一致",
        ),
        (
            "calendar",
            |r| r["is_open"] = json!(2),
            "交易日历字段不符合请求",
        ),
    ];
    for (dataset, mutate, message) in cases {
        assert_eq!(normalized(dataset, mutate).unwrap_err(), message);
    }
    let tushare = Tushare::default();
    let request = request("daily", json!({}));
    let row = raw(&tushare.plan(&request).expect("plan")[0]);
    assert_eq!(
        tushare
            .normalize(&request, &[row.clone(), row])
            .unwrap_err(),
        "返回了重复数据键，未发布"
    );
}

#[test]
fn delivery_year_is_explicit_or_unknown() {
    for value in [
        json!("605"),
        json!("2610"),
        json!("202613"),
        json!("000005"),
        json!("2026-10"),
        json!(202610),
    ] {
        assert!(delivery_month(&value).is_err(), "{value}");
    }
    assert_eq!(delivery_month(&json!("203605")), Ok(json!("2036-05")));
    assert_eq!(delivery_month(&Value::Null), Ok(Value::Null));
    assert_eq!(delivery_month(&json!("")), Ok(Value::Null));
}

#[test]
fn quotation_units_decide_the_suggested_multiplier() {
    // Treasury futures quote in hundred-yuan price terms: never guess a conversion.
    for (symbol, expected) in [("IF2610.CFX", json!("300")), ("T2612.CFX", Value::Null)] {
        let row = row(vec![
            ("exchange", json!("CFFEX")),
            ("symbol", json!(symbol)),
            ("multiplier", json!("300")),
            ("trade_unit", json!("吨")),
            ("per_unit", json!("1000000")),
            ("quote_unit", json!("百元报价")),
        ]);
        assert_eq!(contract_multiplier(&row).expect("multiplier").0, expected);
    }
    for (units, quantity, expected) in [
        ("元/吨", json!("10"), json!("10")),
        ("人民币元/吨", json!("10"), json!("10")),
        ("美元/吨", json!("10"), Value::Null),
        ("元/吨", Value::Null, Value::Null),
        ("元/吨", json!("0"), Value::Null),
        ("元/吨", json!("NaN"), Value::Null),
    ] {
        let row = row(vec![
            ("exchange", json!("SHFE")),
            ("symbol", json!("RB2610.SHF")),
            ("trade_unit", json!("吨")),
            ("quote_unit", json!(units)),
            ("per_unit", quantity),
            ("multiplier", Value::Null),
        ]);
        assert_eq!(
            contract_multiplier(&row).expect("multiplier").0,
            expected,
            "{units}"
        );
    }
}

/// A local vendor endpoint answering each request with the next response.
fn serve(responses: Vec<(u16, String)>) -> (String, Arc<Mutex<Vec<Value>>>) {
    let listener = TcpListener::bind("127.0.0.1:0").expect("bind");
    let endpoint = format!("http://{}", listener.local_addr().expect("address"));
    let seen = Arc::new(Mutex::new(Vec::new()));
    let log = seen.clone();
    std::thread::spawn(move || {
        for (status, body) in responses {
            let (stream, _) = listener.accept().expect("accept");
            let mut reader = BufReader::new(stream);
            let mut length = 0;
            loop {
                let mut line = String::new();
                reader.read_line(&mut line).expect("header");
                if let Some(value) = line.to_lowercase().strip_prefix("content-length:") {
                    length = value.trim().parse().expect("length");
                }
                if line == "\r\n" {
                    break;
                }
            }
            let mut content = vec![0; length];
            reader.read_exact(&mut content).expect("body");
            log.lock()
                .expect("log")
                .push(serde_json::from_slice(&content).expect("json"));
            let reply = format!(
                "HTTP/1.1 {status} X\r\ncontent-length: {}\r\nconnection: close\r\n\r\n{body}",
                body.len()
            );
            reader.get_mut().write_all(reply.as_bytes()).expect("reply");
        }
    });
    (endpoint, seen)
}

fn at(endpoint: String) -> Tushare {
    Tushare {
        endpoint,
        backoff: std::time::Duration::ZERO,
    }
}

#[test]
fn transport_retries_and_never_echoes_credentials() {
    let partition = Tushare::default()
        .plan(&request("daily", json!({})))
        .expect("plan")
        .remove(0);
    let values: Vec<Value> = partition
        .fields
        .iter()
        .map(|f| raw(&partition)[f].clone())
        .collect();
    let success =
        json!({"code": 0, "data": {"fields": partition.fields, "items": [values]}}).to_string();
    let configuration: Map<String, Value> =
        [("token".to_string(), json!(SECRET))].into_iter().collect();

    let (endpoint, seen) = serve(vec![
        (503, String::new()),
        (503, String::new()),
        (200, success.clone()),
    ]);
    let rows = at(endpoint)
        .fetch(&partition, &configuration)
        .expect("rows");
    assert_eq!(rows[0]["close"], json!(3210));
    let seen = seen.lock().expect("seen");
    assert_eq!(seen.len(), 3);
    assert_eq!(seen[0]["token"], json!(SECRET));
    assert_eq!(seen[0]["api_name"], json!("fut_daily"));
    assert_eq!(seen[0]["fields"], json!(partition.fields.join(",")));

    let denied = json!({"code": -1, "msg": format!("token {SECRET} 无效")}).to_string();
    let (endpoint, _) = serve(vec![(200, denied)]);
    let error = at(endpoint).fetch(&partition, &configuration).unwrap_err();
    assert_eq!(error, "AUTH_FAILED：Tushare Token 无效或已失效");

    let (endpoint, _) = serve(vec![(503, String::new()); 3]);
    assert_eq!(
        at(endpoint).fetch(&partition, &configuration).unwrap_err(),
        "Tushare 限流或服务暂不可用，请稍后重试"
    );
    for (status, body, message) in [
        (
            403,
            String::new(),
            "Tushare 请求被拒绝，请检查 Token 和接口权限",
        ),
        (200, "not json".to_string(), "Tushare 返回格式异常"),
        (
            200,
            json!({"code": 0, "data": {"fields": ["ts_code"], "items": []}}).to_string(),
            "Tushare 返回字段结构异常",
        ),
    ] {
        let (endpoint, _) = serve(vec![(status, body)]);
        assert_eq!(
            at(endpoint).fetch(&partition, &configuration).unwrap_err(),
            message
        );
    }
    let mut limited = partition.clone();
    limited.limit = 1;
    let (endpoint, _) = serve(vec![(200, success)]);
    assert_eq!(
        at(endpoint).fetch(&limited, &configuration).unwrap_err(),
        "返回数据触及接口上限，无法确认完整性；未发布"
    );
    for token in ["", "with space"] {
        let configuration = [("token".to_string(), json!(token))].into_iter().collect();
        assert!(
            Tushare::default()
                .fetch(&partition, &configuration)
                .is_err()
        );
    }
}
