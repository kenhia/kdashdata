"""CD-4's walk — mirrors `publishers/rust/src/endpoint.rs`.

No network and no process environment: both are supplied as callables, which
is the whole reason `resolve_with` is separate from `resolve`.
"""

import unittest

from kdash_pub.endpoint import (
    CENTRAL,
    CENTRAL_STEM,
    CENTRAL_STEM_LEGACY,
    CLAUDE,
    CLAUDE_STEM,
    NOWHERE,
    Answer,
    EndpointError,
    Lookup,
    Stem,
    named_stem,
    parse_hostport,
    resolve_with,
)


def no_env(_key):
    return None


class HostPort(unittest.TestCase):
    def test_splits_like_the_c_library(self):
        self.assertEqual(parse_hostport("rpi53"), ("rpi53", 6379))
        self.assertEqual(parse_hostport("rpidash2:6380"), ("rpidash2", 6380))
        self.assertEqual(parse_hostport("  rpi53:6379\n"), ("rpi53", 6379))
        self.assertEqual(parse_hostport("192.168.1.144:6380"), ("192.168.1.144", 6380))

    def test_refusals_match_the_c_library(self):
        for bad in ["", "   ", ":6379", "rpi53:", "rpi53:abc", "rpi53:0", "rpi53:65536", "::1"]:
            with self.assertRaises(EndpointError, msg=repr(bad)):
                parse_hostport(bad)


class Walk(unittest.TestCase):
    def test_an_env_override_wins_outright(self):
        def boom(_key):
            raise AssertionError("khlenv must not be asked when the env pins it")

        got = resolve_with(CENTRAL, lambda k: "elsewhere:6390" if k == CENTRAL_STEM else None, boom)
        self.assertEqual(got, ("elsewhere", 6390))

    def test_an_empty_env_override_is_not_an_override(self):
        got = resolve_with(CENTRAL, lambda _k: "   ", lambda _k: Lookup(Answer.VALUE, "rpi53:6379"))
        self.assertEqual(got, ("rpi53", 6379))

    def test_the_legacy_alias_is_tried_on_a_miss_only(self):
        asked = []

        def lookup(key):
            asked.append(key)
            return Lookup(Answer.MISS) if key == CENTRAL_STEM else Lookup(Answer.VALUE, "rpi53:6379")

        self.assertEqual(resolve_with(CENTRAL, no_env, lookup), ("rpi53", 6379))
        self.assertEqual(asked, [CENTRAL_STEM, CENTRAL_STEM_LEGACY])

    def test_an_explicit_null_stops_the_walk(self):
        asked = []

        def lookup(key):
            asked.append(key)
            return Lookup(Answer.NULL)

        # Not the alias, and not the default: a null is an answer.
        self.assertIs(resolve_with(CENTRAL, no_env, lookup), NOWHERE)
        self.assertEqual(asked, [CENTRAL_STEM])

    def test_unreachable_khlenv_takes_the_default_without_retrying_the_alias(self):
        asked = []

        def lookup(key):
            asked.append(key)
            return Lookup(Answer.UNAVAILABLE, "connection refused")

        self.assertEqual(resolve_with(CENTRAL, no_env, lookup), ("rpi53", 6379))
        self.assertEqual(asked, [CENTRAL_STEM])

    def test_the_claude_stem_refuses_to_guess(self):
        with self.assertRaises(EndpointError):
            resolve_with(CLAUDE, no_env, lambda _k: Lookup(Answer.UNAVAILABLE, "timed out"))
        with self.assertRaises(EndpointError):
            resolve_with(CLAUDE, no_env, lambda _k: Lookup(Answer.MISS))

    def test_the_claude_stem_still_takes_an_override_and_a_store_value(self):
        got = resolve_with(
            CLAUDE, lambda k: "rpidash2:6380" if k == CLAUDE_STEM else None, lambda _k: Lookup(Answer.MISS)
        )
        self.assertEqual(got, ("rpidash2", 6380))
        got = resolve_with(CLAUDE, no_env, lambda _k: Lookup(Answer.VALUE, "rpi53:6379"))
        self.assertEqual(got, ("rpi53", 6379))

    def test_a_rejected_key_is_never_papered_over_by_the_default(self):
        with self.assertRaises(EndpointError):
            resolve_with(CENTRAL, no_env, lambda _k: Lookup(Answer.REJECTED, "bad app name"))

    def test_a_store_value_that_is_not_an_endpoint_is_an_error(self):
        with self.assertRaises(EndpointError):
            resolve_with(CENTRAL, no_env, lambda _k: Lookup(Answer.VALUE, "rpi53:not-a-port"))


class NamedStems(unittest.TestCase):
    def test_the_well_known_stems_keep_their_walks(self):
        self.assertEqual(named_stem(CENTRAL_STEM), CENTRAL)
        self.assertEqual(named_stem(CLAUDE_STEM), CLAUDE)

    def test_an_unknown_stem_gets_no_alias_and_no_default(self):
        self.assertEqual(named_stem("SOMETHING_ELSE"), Stem("SOMETHING_ELSE"))


if __name__ == "__main__":
    unittest.main()
