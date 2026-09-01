"""The `ts` rule — mirrors `publishers/rust/src/payload.rs`."""

import json
import unittest

from kdash_pub.payload import PayloadError, stamp


def field(text, name):
    return json.loads(text)[name]


class Stamping(unittest.TestCase):
    def test_a_missing_ts_is_stamped_with_now(self):
        out = stamp({"state": "ok", "text": "up"}, 1_756_000_000.5)
        self.assertEqual(field(out, "ts"), 1_756_000_000.5)
        self.assertEqual(field(out, "state"), "ok")

    def test_an_existing_ts_is_left_alone(self):
        out = stamp({"ts": 1_700_000_000, "state": "ok"}, 1_756_000_000.0)
        self.assertEqual(field(out, "ts"), 1_700_000_000)

    def test_sub_millisecond_noise_is_rounded_off(self):
        out = stamp({}, 1_756_000_000.1234567)
        self.assertEqual(field(out, "ts"), 1_756_000_000.123)

    def test_json_text_is_accepted_as_well_as_a_mapping(self):
        out = stamp('{"state":"ok"}', 1.0)
        self.assertEqual(field(out, "state"), "ok")

    def test_unknown_fields_ride_along_untouched(self):
        # Additive evolution cuts both ways: writers may add fields freely.
        out = stamp({"state": "ok", "invented_later": 42}, 1.0)
        self.assertEqual(field(out, "invented_later"), 42)

    def test_non_objects_and_bad_ts_are_refused(self):
        for bad in ["[1,2]", '"hi"', "{oops"]:
            with self.assertRaises(PayloadError, msg=bad):
                stamp(bad, 0.0)
        with self.assertRaises(PayloadError):
            stamp({"ts": "soon"}, 0.0)

    def test_a_boolean_ts_is_not_a_number(self):
        # bool is an int in Python; a `ts` of True must not sail through.
        with self.assertRaises(PayloadError):
            stamp({"ts": True}, 0.0)

    def test_an_explicit_null_ts_is_refused_rather_than_filled_in(self):
        # A caller who wrote `"ts": None` meant something, and it was not
        # "please use the wall clock".
        with self.assertRaises(PayloadError):
            stamp({"ts": None}, 0.0)

    def test_unserialisable_values_are_refused_before_redis(self):
        with self.assertRaises(PayloadError):
            stamp({"when": object()}, 0.0)


if __name__ == "__main__":
    unittest.main()
