"""Key grammar — the same accepted/refused table the Rust wrapper pins.

Three implementations of one contract only stay one contract if they are tested
against the same cases, so these mirror `publishers/rust/src/keys.rs`.
"""

import unittest

from kdash_pub.keys import (
    KeyError_,
    TOKEN_MAX,
    check_key,
    check_pattern,
    token_ok,
)


class TokenContract(unittest.TestCase):
    def test_accepted_charset(self):
        for good in ["rpidash2", "a", "dev_telemetry", "host.name-1", "MixedCase"]:
            self.assertTrue(token_ok(good), good)
        self.assertTrue(token_ok("x" * TOKEN_MAX))

    def test_refused(self):
        for bad in ["", "x" * (TOKEN_MAX + 1), "has space", "has:colon", "sl/ash", 'qu"ote', "new\nline"]:
            self.assertFalse(token_ok(bad), repr(bad))

    def test_non_ascii_is_not_alnum_here(self):
        # str.isalnum() is True for 'é' and '٣'; the contract is ASCII only.
        self.assertFalse(token_ok("café"))
        self.assertFalse(token_ok("٣"))


class GovernedKeys(unittest.TestCase):
    def test_registry_families_pass(self):
        for key in [
            "kdash:selftest:kai",
            "kpidash:services:kdashdata-demo:kai",
            "kpidash:services:sonarr:_",
            "claude:session:kai:abc-123",
            "claude:limits",
            # The key sprint 012 legalised and this side refused (WI 2781).
            "ghcp:session:kai:abc-123",
            "kvscf:instances:cleo",
            "kdeskdash:active_mode",
        ]:
            check_key(key)

    def test_unknown_namespace_is_off_contract(self):
        with self.assertRaises(KeyError_):
            check_key("weather:now")

    def test_namespace_case_matters(self):
        # Redis would treat this as a separate family.
        with self.assertRaises(KeyError_):
            check_key("Claude:limits")

    def test_malformed_keys_are_refused_not_trimmed(self):
        for bad in ["", "kdash::thing", "kdash:", ":kdash", "kdash:has space"]:
            with self.assertRaises(KeyError_, msg=repr(bad)):
                check_key(bad)

    def test_over_long_key(self):
        with self.assertRaises(KeyError_):
            check_key("kdash:" + "x" * 600)


class ScanPatternGrammar(unittest.TestCase):
    """`check_pattern` — the same table `publishers/rust/src/keys.rs` pins.

    Three implementations of one contract only stay one contract if they are
    tested against the same cases.
    """

    def test_globs_inside_a_segment(self):
        for good in [
            "kdash:stale:komarchy:*",   # kmon's read (korg:2213)
            "kdash:stale:*:*",
            "claude:session:kai:*",
            "ghcp:session:*",
            "kdash:panel?:kai",
            "claude:limits",            # no glob at all: a key is a pattern
        ]:
            check_pattern(good)

    def test_the_family_being_read_may_not_be_globbed(self):
        # The one that matters: `*:*` on a Redis shared with kvscf and the
        # dashboards is not a publisher's read of its own feed.
        for bad in ["*:*", "*", "kdas?:stale:*"]:
            with self.assertRaises(KeyError_, msg=bad) as caught:
                check_pattern(bad)
            self.assertIn("globbed", str(caught.exception))
        with self.assertRaises(KeyError_):
            check_pattern("weather:*")

    def test_otherwise_held_to_the_key_grammar(self):
        for bad in [
            "",
            "kdash::*",
            "kdash:stale:",
            "kdash:has space:*",
            "kdash:stale:[ab]:*",   # Redis understands it; this grammar does not
            "kdash:stale:a\\*:*",
            "kdash:" + "x" * (TOKEN_MAX + 1),
        ]:
            with self.assertRaises(KeyError_, msg=repr(bad)):
                check_pattern(bad)

    def test_a_write_never_takes_a_pattern(self):
        # The two functions stay separate so that this stays true.
        for globbed in ["kdash:stale:*", "kdash:panel?:kai", "*:*"]:
            with self.assertRaises(KeyError_, msg=globbed):
                check_key(globbed)


if __name__ == "__main__":
    unittest.main()
