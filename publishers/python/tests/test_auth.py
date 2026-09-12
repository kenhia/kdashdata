"""CD-12's env-file route and CD-19's per-host file — mirrors
`publishers/rust/src/auth.rs`, test for test."""

import os
import stat
import tempfile
import unittest
from pathlib import Path

from kdash_pub.auth import (
    PER_HOST_FILE,
    AuthError,
    Candidate,
    NoValueError,
    Source,
    TooOpenError,
    candidates,
    from_file,
    parse_env_file,
    password,
    resolve,
    resolve_candidates,
)


class EnvFileShape(unittest.TestCase):
    def test_a_plain_assignment_is_read(self):
        self.assertEqual(parse_env_file("REDISCLI_AUTH=hunter2\n"), "hunter2")

    def test_quotes_comments_and_neighbours(self):
        text = (
            "# the fleet password (krot: rpi53-redis-password)\n"
            "OTHER=ignored\n"
            'REDISCLI_AUTH="quoted value"\n'
        )
        self.assertEqual(parse_env_file(text), "quoted value")
        self.assertEqual(parse_env_file("REDISCLI_AUTH='single'\n"), "single")

    def test_the_per_host_files_single_quoted_shape_parses(self):
        # k-homelab writes `KEY='value'` (handoff korg:2480), among nine keys.
        text = (
            "POSTGRES_PASSWORD='other'\n"
            "REDISCLI_AUTH='fleet password'\n"
            "HF_TOKEN='third'\n"
        )
        self.assertEqual(parse_env_file(text), "fleet password")

    def test_the_old_systemd_unit_spelling_still_parses(self):
        self.assertEqual(parse_env_file("Environment=REDISCLI_AUTH=fromunit\n"), "fromunit")

    def test_the_last_assignment_wins_as_it_does_for_systemd(self):
        self.assertEqual(parse_env_file("REDISCLI_AUTH=old\nREDISCLI_AUTH=new\n"), "new")

    def test_empty_or_absent_is_not_a_password(self):
        for text in ["", "REDISCLI_AUTH=\n", "REDISCLI_AUTH=   \n", "# REDISCLI_AUTH=x\n", "REDISCLI_AUTH_OLD=x\n"]:
            self.assertIsNone(parse_env_file(text), repr(text))


class CandidateList(unittest.TestCase):
    """CD-19's ordering. The environment is injected, never mutated."""

    def test_the_per_host_file_comes_before_the_per_user_files(self):
        found = candidates({"HOME": "/home/someone"})
        self.assertEqual(
            [str(c.path) for c in found],
            [
                PER_HOST_FILE,
                "/home/someone/.config/kdash/redis-auth.env",
                "/home/someone/.config/kpidash-client/redis-auth.env",
            ],
        )
        self.assertIs(found[0].source, Source.PER_HOST)
        self.assertTrue(found[1].source.deprecated)
        self.assertTrue(found[2].source.deprecated)

    def test_programdata_is_read_from_the_environment_and_never_hardcoded(self):
        found = candidates({"ProgramData": "D:/machine-data", "HOME": "/home/someone"})
        self.assertEqual(str(found[0].path), "D:/machine-data/khomelab/secrets.env")
        self.assertIs(found[0].source, Source.PER_HOST)

    def test_an_absent_programdata_skips_that_rung_rather_than_defaulting(self):
        found = candidates({"HOME": "/home/someone"})
        self.assertFalse(
            any("ProgramData" in str(c.path) for c in found),
            "an unset ProgramData must skip the rung, not guess C:\\ProgramData",
        )
        # And an empty one is unset, not the filesystem root.
        empty = candidates({"ProgramData": "", "HOME": "/home/someone"})
        self.assertEqual(str(empty[0].path), PER_HOST_FILE)

    def test_an_explicit_override_is_still_exclusive(self):
        found = candidates(
            {
                "KDASH_AUTH_FILE": "/tmp/named.env",
                "ProgramData": "D:/machine-data",
                "HOME": "/home/someone",
            }
        )
        self.assertEqual(len(found), 1)
        self.assertEqual(str(found[0].path), "/tmp/named.env")
        self.assertIs(found[0].source, Source.OVERRIDE)

    def test_xdg_config_home_still_wins_over_home(self):
        found = candidates({"XDG_CONFIG_HOME": "/elsewhere/config", "HOME": "/home/someone"})
        self.assertEqual(str(found[1].path), "/elsewhere/config/kdash/redis-auth.env")

    def test_a_bare_environment_still_offers_the_per_host_file(self):
        # No HOME at all — a systemd unit with an empty environment. The
        # per-host file is the whole point: it does not need one.
        found = candidates({})
        self.assertEqual([str(c.path) for c in found], [PER_HOST_FILE])


@unittest.skipUnless(os.name == "posix", "mode bits are a posix concept")
class FileMode(unittest.TestCase):
    def test_a_group_readable_per_user_file_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "redis-auth.env"
            path.write_text("REDISCLI_AUTH=secret\n")

            path.chmod(0o640)
            with self.assertRaises(TooOpenError):
                from_file(path, Source.PER_USER)

            path.chmod(stat.S_IRUSR | stat.S_IWUSR)
            self.assertEqual(from_file(path, Source.PER_USER), "secret")

    def test_a_group_readable_per_host_file_is_accepted_but_a_wider_one_is_not(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "secrets.env"
            path.write_text("REDISCLI_AUTH='shared'\n")

            # 0640 root:khomelab is the published contract — group read is how a
            # publisher reaches it at all.
            path.chmod(0o640)
            self.assertEqual(from_file(path, Source.PER_HOST), "shared")
            # Stricter is fine too.
            path.chmod(0o600)
            self.assertEqual(from_file(path, Source.PER_HOST), "shared")

            # World-readable defeats the group entirely.
            path.chmod(0o644)
            with self.assertRaises(TooOpenError):
                from_file(path, Source.PER_HOST)
            # Group-writable lets any member change the password every host reads.
            path.chmod(0o660)
            with self.assertRaises(TooOpenError):
                from_file(path, Source.PER_HOST)

    def test_a_0600_file_with_no_assignment_says_so(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "redis-auth.env"
            path.write_text("SOMETHING_ELSE=x\n")
            path.chmod(0o600)
            with self.assertRaises(NoValueError):
                from_file(path, Source.PER_USER)

    def test_a_too_open_per_host_file_stops_the_walk_rather_than_being_skipped(self):
        # The one fault that is NOT "keep looking": using a world-readable fleet
        # password silently is worse than failing loudly.
        with tempfile.TemporaryDirectory() as tmp:
            shared = Path(tmp) / "secrets.env"
            mine = Path(tmp) / "redis-auth.env"
            shared.write_text("REDISCLI_AUTH='shared'\n")
            mine.write_text("REDISCLI_AUTH=mine\n")
            shared.chmod(0o644)
            mine.chmod(0o600)

            with self.assertRaises(TooOpenError):
                resolve_candidates(
                    [
                        Candidate(shared, Source.PER_HOST),
                        Candidate(mine, Source.PER_USER),
                    ]
                )


class TheWalk(unittest.TestCase):
    """CD-19's two keep-looking outcomes, and what still stops."""

    @staticmethod
    def _pair(tmp, shared_text, mine_text):
        shared = Path(tmp) / "secrets.env"
        mine = Path(tmp) / "redis-auth.env"
        if shared_text is not None:
            shared.write_text(shared_text)
            if os.name == "posix":
                shared.chmod(0o640)
        mine.write_text(mine_text)
        if os.name == "posix":
            mine.chmod(0o600)
        return [Candidate(shared, Source.PER_HOST), Candidate(mine, Source.PER_USER)]

    def test_the_per_host_file_wins_when_both_exist(self):
        with tempfile.TemporaryDirectory() as tmp:
            walk = self._pair(
                tmp,
                "REDISCLI_AUTH='from the per-host file'\n",
                "REDISCLI_AUTH=from the per-user file\n",
            )
            found = resolve_candidates(walk)
            self.assertEqual(found.password, "from the per-host file")
            self.assertIs(found.source, Source.PER_HOST)
            self.assertFalse(found.source.deprecated)
            self.assertEqual(found.path, walk[0].path)

    def test_a_per_host_file_we_cannot_read_is_skipped_not_fatal(self):
        # The measured state on kai, 2026-09-12: the file is there and `ken` is
        # not in `khomelab`, so reading it fails EACCES. A directory stands in
        # for that here (EISDIR through the same OSError) because it is
        # deterministic whatever uid the gate runs as — the real EACCES is
        # verified live.
        with tempfile.TemporaryDirectory() as tmp:
            walk = self._pair(tmp, None, "REDISCLI_AUTH=fell through\n")
            walk[0].path.mkdir()
            if os.name == "posix":
                walk[0].path.chmod(0o750)
            found = resolve_candidates(walk)
            self.assertEqual(found.password, "fell through")
            self.assertTrue(found.source.deprecated)

    def test_a_per_host_file_without_our_key_is_skipped_not_fatal(self):
        # Nine keys across eight hosts; a host's manifest may grant a subset.
        with tempfile.TemporaryDirectory() as tmp:
            walk = self._pair(
                tmp,
                "POSTGRES_PASSWORD='not ours'\nHF_TOKEN='nor this'\n",
                "REDISCLI_AUTH=fell through\n",
            )
            self.assertEqual(resolve_candidates(walk).password, "fell through")

    def test_a_per_user_file_without_our_key_is_still_fatal(self):
        # That file exists for exactly one reason, so silence there would turn a
        # real fault into "the feed just stopped".
        with tempfile.TemporaryDirectory() as tmp:
            mine = Path(tmp) / "redis-auth.env"
            mine.write_text("# rotated away and never refilled\n")
            if os.name == "posix":
                mine.chmod(0o600)
            with self.assertRaises(NoValueError):
                resolve_candidates([Candidate(mine, Source.PER_USER)])

    def test_finding_nothing_is_still_a_valid_answer(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertIsNone(
                resolve_candidates(
                    [
                        Candidate(Path(tmp) / "secrets.env", Source.PER_HOST),
                        Candidate(Path(tmp) / "redis-auth.env", Source.PER_USER),
                    ]
                )
            )


class Resolving(unittest.TestCase):
    def test_the_environment_variable_still_wins_over_every_file(self):
        found = resolve({"REDISCLI_AUTH": "explicit", "KDASH_AUTH_FILE": "/does/not/exist.env"})
        self.assertEqual(found.password, "explicit")
        self.assertIs(found.source, Source.ENVIRONMENT)
        self.assertIsNone(found.path)
        self.assertFalse(found.source.deprecated)
        self.assertEqual(found.origin(), "REDISCLI_AUTH")

    def test_an_empty_environment_variable_is_not_a_password(self):
        with tempfile.TemporaryDirectory() as tmp:
            named = Path(tmp) / "named.env"
            named.write_text("REDISCLI_AUTH=from the file\n")
            if os.name == "posix":
                named.chmod(0o600)
            found = resolve({"REDISCLI_AUTH": "", "KDASH_AUTH_FILE": str(named)})
            self.assertEqual(found.password, "from the file")
            self.assertIs(found.source, Source.OVERRIDE)

    def test_password_still_returns_the_value_alone(self):
        self.assertEqual(password({"REDISCLI_AUTH": "explicit"}), "explicit")
        # An environment with nothing in it and no per-host file on this host is
        # the `None` answer, not an error.
        self.assertIsNone(password({"KDASH_AUTH_FILE": "/does/not/exist.env"}))

    def test_the_typed_errors_are_still_autherrors(self):
        # Callers written against sprint 003 catch `AuthError`; both refusals
        # must still reach them.
        self.assertTrue(issubclass(TooOpenError, AuthError))
        self.assertTrue(issubclass(NoValueError, AuthError))


if __name__ == "__main__":
    unittest.main()
