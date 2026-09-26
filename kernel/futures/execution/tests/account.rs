fn fixtures() -> Vec<Value> {
    let mut fixture: Value = serde_json::from_str(include_str!("fixtures/daily.json")).unwrap();
    let timing = fixture["trading_time"].take();
    let mut cases = fixture["cases"].as_array().unwrap().clone();
    for case in &mut cases {
        case["config"]["rules"]["spec"]["trading_time"] = timing.clone();
    }
    cases
}
use asterion_execution::{ExecutionConfig, ExecutionFactory};
use serde_json::Value;
use sha2::{Digest, Sha256};
#[test]
fn current_account_outputs_match_every_frozen_byte() {
    for case in fixtures() {
        let config: ExecutionConfig = serde_json::from_value(case["config"].clone()).unwrap();
        let owner = ExecutionFactory::default();
        let mut account = owner.open(config).unwrap();
        for (bar, intent) in case["bars"]
            .as_array()
            .unwrap()
            .iter()
            .zip(case["intents"].as_array().unwrap())
        {
            account
                .begin_day(serde_json::from_value(bar.clone()).unwrap())
                .unwrap();
            account.close_intent(intent.as_bool().unwrap()).unwrap();
        }
        let result = serde_json::to_value(account.finish().unwrap()).unwrap();
        assert_eq!(result, case["result"], "{}", case["name"]);
        let bytes = asterion_foundation::canonical(&result).unwrap();
        assert_eq!(
            hex::encode(Sha256::digest(bytes)),
            case["sha256"].as_str().unwrap(),
            "{}",
            case["name"]
        );
    }
}

#[test]
fn transitions_reject_partial_results_and_invalid_calls_poison_the_session() {
    let case = &fixtures()[0];
    let owner = ExecutionFactory::default();
    let config = || serde_json::from_value(case["config"].clone()).unwrap();
    let bar = || serde_json::from_value(case["bars"][0].clone()).unwrap();
    assert!(owner.open(config()).unwrap().finish().is_err());
    let mut account = owner.open(config()).unwrap();
    account.begin_day(bar()).unwrap();
    assert!(account.finish().is_err());
    let mut account = owner.open(config()).unwrap();
    assert!(account.close_intent(true).is_err());
    assert!(account.begin_day(bar()).is_err());
    let mut account = owner.open(config()).unwrap();
    account.begin_day(bar()).unwrap();
    account.close_intent(true).unwrap();
    assert!(account.begin_day(bar()).is_err());
    assert!(account.finish().is_err());
}
#[test]
fn owner_close_or_drop_revokes_existing_accounts() {
    let case = &fixtures()[0];
    let owner = ExecutionFactory::default();
    let config = || serde_json::from_value(case["config"].clone()).unwrap();
    let mut account = owner.open(config()).unwrap();
    account
        .begin_day(serde_json::from_value(case["bars"][0].clone()).unwrap())
        .unwrap();
    owner.close();
    assert!(owner.open(config()).is_err());
    assert!(account.close_intent(true).is_err());
    assert!(account.finish().is_err());
    let owner = ExecutionFactory::default();
    let mut account = owner.open(config()).unwrap();
    drop(owner);
    assert!(
        account
            .begin_day(serde_json::from_value(case["bars"][0].clone()).unwrap())
            .is_err()
    );
}
#[test]
fn foreign_contract_or_out_of_range_day_is_rejected_before_any_result() {
    let case = &fixtures()[0];
    let owner = ExecutionFactory::default();
    for (field, value) in [
        ("contract_id", "foreign"),
        ("trading_day", "2024-01-11"),
        ("settle", "0"),
        ("open", "-1"),
    ] {
        let mut bar = case["bars"][0].clone();
        bar[field] = Value::String(value.into());
        let mut account = owner
            .open(serde_json::from_value(case["config"].clone()).unwrap())
            .unwrap();
        assert!(
            account
                .begin_day(serde_json::from_value(bar).unwrap())
                .is_err()
        );
        assert!(account.finish().is_err());
    }
}
