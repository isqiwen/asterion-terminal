use asterion_kernel::database::config_from_url;
use serde_json::json;

#[test]
fn sqlite_urls_follow_the_current_memory_relative_and_absolute_forms() {
    assert_eq!(
        config_from_url("sqlite://").unwrap(),
        json!({"backend": "sqlite", "path": ":memory:", "timeout_ms": 5000})
    );
    assert_eq!(
        config_from_url("sqlite:///state/db.sqlite").unwrap()["path"],
        "state/db.sqlite"
    );
    assert_eq!(
        config_from_url("sqlite:////tmp/db.sqlite").unwrap()["path"],
        "/tmp/db.sqlite"
    );
    assert_eq!(
        config_from_url("sqlite+asterion:////tmp/x.db").unwrap()["path"],
        "/tmp/x.db"
    );
}

#[test]
fn postgresql_urls_decode_credentials_and_apply_defaults() {
    assert_eq!(
        config_from_url("postgresql://asterion:p%40ss%3Aword%25@127.0.0.1:54321/asterion").unwrap(),
        json!({
            "backend": "postgresql",
            "host": "127.0.0.1",
            "port": 54321,
            "database": "asterion",
            "user": "asterion",
            "password": "p@ss:word%",
            "options": "",
            "connect_timeout_ms": 5000,
            "statement_timeout_ms": 30000,
        })
    );
    let defaults = config_from_url("postgresql://reader@db.local").unwrap();
    assert_eq!(defaults["port"], 5432);
    assert_eq!(defaults["database"], "postgres");
    assert_eq!(defaults["password"], "");
    assert_eq!(
        config_from_url("postgresql+asterion:///app").unwrap()["host"],
        "127.0.0.1"
    );
}

#[test]
fn unsupported_or_malformed_urls_are_rejected() {
    for url in [
        "mysql://user@host/db",
        "postgresql://user@host:notaport/db",
        "postgresql://user@host/db?sslmode=require",
        "sqlite:///db.sqlite?mode=ro",
        "postgresql://user:%zz@host/db",
        "postgresql://user@[::1]/db",
        "sqlite:/relative",
        "no scheme",
    ] {
        assert!(config_from_url(url).is_err(), "{url}");
    }
}
