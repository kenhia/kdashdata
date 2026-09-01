"""CD-12's env-file route — mirrors `publishers/rust/src/auth.rs`."""

import os
import stat
import tempfile
import unittest
from pathlib import Path

from kdash_pub.auth import AuthError, from_file, parse_env_file


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

    def test_the_old_systemd_unit_spelling_still_parses(self):
        self.assertEqual(parse_env_file("Environment=REDISCLI_AUTH=fromunit\n"), "fromunit")

    def test_the_last_assignment_wins_as_it_does_for_systemd(self):
        self.assertEqual(parse_env_file("REDISCLI_AUTH=old\nREDISCLI_AUTH=new\n"), "new")

    def test_empty_or_absent_is_not_a_password(self):
        for text in ["", "REDISCLI_AUTH=\n", "REDISCLI_AUTH=   \n", "# REDISCLI_AUTH=x\n", "REDISCLI_AUTH_OLD=x\n"]:
            self.assertIsNone(parse_env_file(text), repr(text))


@unittest.skipUnless(os.name == "posix", "mode bits are a posix concept")
class FileMode(unittest.TestCase):
    def test_a_group_readable_secret_file_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "redis-auth.env"
            path.write_text("REDISCLI_AUTH=secret\n")

            path.chmod(0o640)
            with self.assertRaises(AuthError):
                from_file(path)

            path.chmod(stat.S_IRUSR | stat.S_IWUSR)
            self.assertEqual(from_file(path), "secret")

    def test_a_0600_file_with_no_assignment_says_so(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "redis-auth.env"
            path.write_text("SOMETHING_ELSE=x\n")
            path.chmod(0o600)
            with self.assertRaises(AuthError):
                from_file(path)


if __name__ == "__main__":
    unittest.main()
