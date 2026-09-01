//! Pure payload handling: the `ts` rule from contracts/rules.md, applied once
//! so no publisher has to remember it.
//!
//! > Every payload carries `ts` (unix seconds, float) — even when the key also
//! > has a TTL. Timestamps are the writer's clock.
//!
//! `now` is always a parameter here (CD-10's no-ambient-clock rule); the I/O
//! shell is what reads the wall clock.

use serde_json::{Map, Value};
use std::fmt;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum PayloadError {
    NotJson(String),
    NotAnObject,
    BadTs,
}

impl fmt::Display for PayloadError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            PayloadError::NotJson(e) => write!(f, "payload is not JSON: {e}"),
            PayloadError::NotAnObject => {
                write!(f, "payload is not a JSON object (contracts/rules.md)")
            }
            PayloadError::BadTs => write!(f, "payload carries a non-numeric `ts`"),
        }
    }
}

impl std::error::Error for PayloadError {}

/// Parse a payload and stamp it, returning the bytes to write.
///
/// A payload that already carries `ts` keeps it — a publisher replaying an
/// observation knows better than the wall clock does, and poll-mode writers
/// depend on that (kdeskdash's `claude:limits` refuses to publish over a newer
/// *observation*, not a newer write). A `ts` that is present but not a number
/// is a bug, not a missing field, so it is refused rather than overwritten.
pub fn stamp(text: &str, now: f64) -> Result<String, PayloadError> {
    let value: Value =
        serde_json::from_str(text).map_err(|e| PayloadError::NotJson(e.to_string()))?;
    let mut object: Map<String, Value> = match value {
        Value::Object(map) => map,
        _ => return Err(PayloadError::NotAnObject),
    };

    match object.get("ts") {
        Some(Value::Number(_)) => {}
        Some(_) => return Err(PayloadError::BadTs),
        None => {
            object.insert("ts".into(), stamp_value(now));
        }
    }

    Ok(Value::Object(object).to_string())
}

/// `now` as the JSON number a `ts` field holds. Split out so the rounding is
/// stated once: milliseconds are the resolution every freshness window in the
/// registry is measured against, and a full f64 of `SystemTime` noise only
/// makes payloads harder to read.
fn stamp_value(now: f64) -> Value {
    let rounded = (now * 1000.0).round() / 1000.0;
    serde_json::Number::from_f64(rounded)
        .map(Value::Number)
        .unwrap_or(Value::Number(0.into()))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn field(json: &str, name: &str) -> Value {
        serde_json::from_str::<Value>(json).unwrap()[name].clone()
    }

    #[test]
    fn a_missing_ts_is_stamped_with_now() {
        let out = stamp(r#"{"state":"ok","text":"up"}"#, 1_756_000_000.5).unwrap();
        assert_eq!(field(&out, "ts"), serde_json::json!(1_756_000_000.5));
        assert_eq!(field(&out, "state"), serde_json::json!("ok"));
    }

    #[test]
    fn an_existing_ts_is_left_alone() {
        let out = stamp(r#"{"ts":1700000000,"state":"ok"}"#, 1_756_000_000.0).unwrap();
        assert_eq!(field(&out, "ts"), serde_json::json!(1_700_000_000));
    }

    #[test]
    fn sub_millisecond_noise_is_rounded_off() {
        let out = stamp("{}", 1_756_000_000.123_456_7).unwrap();
        assert_eq!(field(&out, "ts"), serde_json::json!(1_756_000_000.123));
    }

    #[test]
    fn non_objects_and_bad_ts_are_refused() {
        assert_eq!(stamp("[1,2]", 0.0), Err(PayloadError::NotAnObject));
        assert_eq!(stamp("\"hi\"", 0.0), Err(PayloadError::NotAnObject));
        assert_eq!(stamp(r#"{"ts":"soon"}"#, 0.0), Err(PayloadError::BadTs));
        assert!(matches!(stamp("{oops", 0.0), Err(PayloadError::NotJson(_))));
    }

    #[test]
    fn unknown_fields_ride_along_untouched() {
        // Additive evolution cuts both ways: writers may add fields freely.
        let out = stamp(r#"{"state":"ok","invented_later":42}"#, 1.0).unwrap();
        assert_eq!(field(&out, "invented_later"), serde_json::json!(42));
    }
}
