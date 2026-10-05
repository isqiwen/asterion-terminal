"""Distribution guards reject missing identities, credentials and mismatched evidence."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("release_checks", Path(__file__).resolve().parents[1] / "scripts/macos_release.py")
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)

for environment in ({}, {"CSC_NAME": "-", "APPLE_KEYCHAIN_PROFILE": "fixture"}, {"CSC_NAME": "Fixture"}):
    try:
        release.signing_preflight(environment)
    except ValueError:
        pass
    else:
        raise AssertionError("incomplete distribution identity accepted")
for description in ("0 valid identities found", '1) AA "Apple Development: Fixture"',
                    '1) AA "Developer ID Application: Fixture"\n2) BB "Developer ID Application: Fixture"'):
    with patch.object(release, "output", return_value=description):
        try:
            release.signing_preflight({"CSC_NAME": "Fixture", "APPLE_KEYCHAIN_PROFILE": "fixture"})
        except ValueError:
            pass
        else:
            raise AssertionError("missing, wrong type or ambiguous identity accepted")
with patch.object(release, "output", return_value='1) AA "Developer ID Application: Fixture"'):
    environment = {"CSC_NAME": "Fixture", "APPLE_KEYCHAIN_PROFILE": "fixture", "APPLE_ID": "unused"}
    release.signing_preflight(environment)
    assert "APPLE_ID" not in environment
with patch.object(release, "output", return_value=json.dumps({"status": "Invalid"})), patch.object(release.subprocess, "run") as run:
    try:
        release.notarize_installer(Path("fixture.dmg"), {"APPLE_KEYCHAIN_PROFILE": "fixture"})
    except ValueError:
        pass
    else:
        raise AssertionError("rejected notarization accepted")
    run.assert_not_called()
with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    installer = root / "fixture.dmg"
    installer.write_bytes(b"artifact under test")
    report = root / "installed.json"
    report.write_text(json.dumps({"status": "passed", "sha256": "0" * 64}))
    try:
        release.write_evidence(installer, report)
    except ValueError:
        pass
    else:
        raise AssertionError("acceptance for another installer accepted")
    assert not (root / "distribution-acceptance.json").exists()
print("Distribution refuses absent/ambiguous signing identity, rejected notarization and unrelated installed evidence")


def signature(team="FIXTURETEAM", flags="0x10000(runtime)", extra=""):
    return (f"Executable=/tmp/runtime-fixture.app\n"
            f"CodeDirectory v=20500 size=123 flags={flags} hashes=1+7 location=embedded\n"
            f"Authority=Developer ID Application: Fixture\n"
            f"Authority=Developer ID Certification Authority\n"
            f"TeamIdentifier={team}\n{extra}")


class SignatureGuards(unittest.TestCase):
    app = Path("/tmp/runtime-fixture.app")
    native_name = "asterion-node-agent"

    def verify(self, description, native_description=None, archs="arm64"):
        def output(command):
            if command[0] == "lipo":
                return archs + "\n"
            self.assertEqual(command[:3], ["codesign", "--display", "--verbose=4"])
            return description if command[-1] == str(self.app) else native_description

        with patch.object(release, "output", side_effect=output), \
                patch.object(release.subprocess, "run") as run, \
                patch.object(release.platform, "machine", return_value="arm64"):
            release.verify_application(self.app, {self.native_name} if native_description is not None else set())
            return run

    def test_runtime_path_or_label_cannot_replace_signed_runtime_flag(self):
        for flags in ("0x0(none)", "0x0(runtime)", "0x1(runtime)", "runtime", "0x10000garbage(runtime)"):
            with self.subTest(flags=flags), self.assertRaisesRegex(ValueError, "hardened runtime"):
                self.verify(signature(flags=flags))

    def test_runtime_flags_must_be_present_and_unambiguous(self):
        valid = signature()
        descriptions = (valid.replace("flags=0x10000(runtime)", ""),
                        valid.replace("CodeDirectory ", "Unrelated="),
                        valid + "CodeDirectory v=20500 flags=0x0(none)\n",
                        valid.replace("flags=0x10000(runtime)", "flags=0x10000(runtime) flags=0x0(none)"))
        for description in descriptions:
            with self.subTest(description=description), self.assertRaisesRegex(ValueError, "hardened runtime"):
                self.verify(description)

    def test_native_team_must_equal_application_team(self):
        for team in ("FIXTURETEAMOTHER", "OTHERFIXTURETEAM", "OTHERTEAM", "", "not set"):
            with self.subTest(team=team), self.assertRaisesRegex(ValueError, "application's Developer ID"):
                self.verify(signature(), signature(team=team))

    def test_ambiguous_or_decoy_team_is_rejected(self):
        descriptions = (signature(extra="TeamIdentifier=FIXTURETEAM\n"),
                        signature().replace("\nTeamIdentifier=", "\nExecutable=/tmp/TeamIdentifier="))
        for description in descriptions:
            with self.subTest(description=description):
                with self.assertRaisesRegex(ValueError, "application lacks a Developer ID"):
                    self.verify(description)
                with self.assertRaisesRegex(ValueError, "application's Developer ID"):
                    self.verify(signature(), description)

    def test_authority_must_be_the_developer_id_leaf(self):
        descriptions = (signature().replace("Authority=Developer ID Application:",
                                             "Executable=/tmp/Authority=Developer ID Application:"),
                        signature().replace("Developer ID Application: Fixture", "Apple Development: Fixture"),
                        "Authority=Apple Development: Fixture\n" + signature())
        for description in descriptions:
            with self.subTest(description=description):
                with self.assertRaisesRegex(ValueError, "application lacks a Developer ID"):
                    self.verify(description)
                with self.assertRaisesRegex(ValueError, "application's Developer ID"):
                    self.verify(signature(), description)

    def test_ad_hoc_signatures_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "hardened runtime"):
            self.verify(signature(flags="0x10002(adhoc,runtime)"))
        with self.assertRaisesRegex(ValueError, "application lacks a Developer ID"):
            self.verify(signature(extra="Signature=adhoc\n"))
        with self.assertRaisesRegex(ValueError, "application's Developer ID"):
            self.verify(signature(), signature(extra="Signature=adhoc\n"))

    def test_valid_runtime_and_exact_team_require_ticket_and_gatekeeper(self):
        for flags in ("0x10000(runtime)", "0x10001(host,runtime)", "0x10000"):
            with self.subTest(flags=flags):
                run = self.verify(signature(flags=flags), signature(), archs="x86_64 arm64")
                run.assert_any_call(["codesign", "--verify", "--deep", "--strict", str(self.app)], check=True)
                run.assert_any_call(["codesign", "--verify", "--strict",
                                     str(self.app / "Contents/Resources/native" / self.native_name)], check=True)
                run.assert_any_call(["xcrun", "stapler", "validate", str(self.app)], check=True)
                run.assert_any_call(["spctl", "--assess", "--type", "execute", str(self.app)], check=True)

    def test_host_architecture_is_still_required(self):
        with self.assertRaisesRegex(ValueError, "architecture differs"):
            self.verify(signature(), archs="x86_64")

    def test_installer_rejects_decoy_authority_and_ad_hoc_signature(self):
        descriptions = (signature().replace("Authority=Developer ID Application:",
                                             "Executable=/tmp/Authority=Developer ID Application:"),
                        signature(team=""), signature(team="not set"),
                        signature(extra="Signature=adhoc\n"), signature(extra="TeamIdentifier=OTHERTEAM\n"))
        for description in descriptions:
            with self.subTest(description=description), \
                    patch.object(release, "output", return_value=description), \
                    patch.object(release.subprocess, "run") as run:
                with self.assertRaisesRegex(ValueError, "installer lacks a Developer ID"):
                    release.verify_installer(Path("fixture.dmg"))
                self.assertEqual(run.call_count, 1)

    def test_installer_does_not_require_an_executable_runtime_flag(self):
        description = "Authority=Developer ID Application: Fixture\nTeamIdentifier=FIXTURETEAM\n"
        with patch.object(release, "output", return_value=description), \
                patch.object(release.subprocess, "run") as run:
            release.verify_installer(Path("fixture.dmg"))
            run.assert_any_call(["xcrun", "stapler", "validate", "fixture.dmg"], check=True)
            run.assert_any_call(["spctl", "--assess", "--type", "open", "--context",
                                 "context:primary-signature", "fixture.dmg"], check=True)


if __name__ == "__main__":
    unittest.main(verbosity=2)
