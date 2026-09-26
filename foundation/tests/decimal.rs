use asterion_foundation::decimal::Decimal;
use serde_json::Value;
#[test]
fn explicit_decimal_context_matches_current_finite_arithmetic() {
    let cases: Value = serde_json::from_str(include_str!("fixtures/decimal.json")).unwrap();
    for case in cases.as_array().unwrap() {
        let a: Decimal = case["a"].as_str().unwrap().parse().unwrap();
        let b: Decimal = case["b"].as_str().unwrap().parse().unwrap();
        let precision = case["precision"].as_u64().unwrap() as u32;
        let result = match case["op"].as_str().unwrap() {
            "add" => a.add(&b, precision),
            "subtract" => a.subtract(&b, precision),
            "divide" => a.divide(&b, precision),
            _ => unreachable!(),
        };
        match case["result"].as_str() {
            Some(expected) => assert_eq!(result.unwrap().to_string(), expected, "{case}"),
            None => assert!(result.is_err(), "{case}"),
        }
    }
}
