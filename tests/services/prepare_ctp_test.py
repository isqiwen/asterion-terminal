"""Unit tests for scripts/prepare_ctp.py official SDK import and profile switching."""
import importlib.util
import json
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("prepare_ctp", ROOT / "scripts/prepare_ctp.py")
prepare_ctp = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(prepare_ctp)


def _write_official_linux_sdk(directory: Path, tag: bytes) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    for name in (
        "ThostFtdcMdApi.h",
        "ThostFtdcUserApiDataType.h",
        "ThostFtdcUserApiStruct.h",
        "ThostFtdcTraderApi.h",
        "thostmduserapi_se.so",
        "thosttraderapi_se.so",
    ):
        (directory / name).write_bytes(name.encode() + b":" + tag)


def _write_official_macos_sdk(directory: Path, tag: bytes) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    for name in (
        "ThostFtdcMdApi.h",
        "ThostFtdcUserApiDataType.h",
        "ThostFtdcUserApiStruct.h",
        "ThostFtdcTraderApi.h",
    ):
        (directory / name).write_bytes(name.encode() + b":" + tag)
    for fw in ("thostmduserapi_se", "thosttraderapi_se"):
        target = directory / f"{fw}.framework/Versions/A/{fw}"
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(fw.encode() + b":" + tag)


def _base_manifest(linux_mirror_data: bytes = b"mirror") -> dict:
    digest = prepare_ctp.sha256_bytes(linux_mirror_data)
    return {
        "prod": {
            "macos": {"origin": "third-party-mirror", "official": False, "files": {}},
            "linux": {
                "origin": "third-party-mirror",
                "official": False,
                "files": {
                    name: {"url": f"https://mirror.example/{name}", "sha256": digest}
                    for name in prepare_ctp.REQUIRED_FILES["linux"]
                },
            },
        },
        "eval": {
            "macos": {"origin": "", "official": True, "files": {}},
            "linux": {"origin": "", "official": True, "files": {}},
        },
    }


class PrepareCtpTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.manifest = self.root / "sources.json"
        self.stage = self.root / "stage"

    def tearDown(self):
        self.tmp.cleanup()

    def test_record_official_local_dir_and_switch_between_prod_and_eval(self):
        self.manifest.write_text(json.dumps(_base_manifest()), encoding="utf-8")
        prod_dir = self.root / "sfit-prod" / "v6.7.7_20240607_api_traderapi_se_linux64"
        eval_dir = self.root / "broker-eval" / "v6.7.7_cp_linux64"
        _write_official_linux_sdk(prod_dir, b"official-prod")
        _write_official_linux_sdk(eval_dir, b"official-eval")

        # Non-official mirror fails under --require-official before recording an official package.
        with self.assertRaises(SystemExit) as ctx:
            prepare_ctp.stage_sdk(
                target_os="linux",
                target_arch="x86_64",
                profile="prod",
                manifest_path=self.manifest,
                stage_dir=self.stage,
                require_official=True,
            )
        self.assertIn("non-official", str(ctx.exception))

        # Record official prod SDK from local directory: replaces third-party URLs and marks official.
        prepare_ctp.stage_sdk(
            target_os="linux",
            target_arch="x86_64",
            profile="prod",
            manifest_path=self.manifest,
            stage_dir=self.stage,
            local_dir=prod_dir.parent,
            record_origin="SFIT official v6.7.7 prod",
            require_official=True,
        )
        updated = json.loads(self.manifest.read_text(encoding="utf-8"))
        self.assertTrue(updated["prod"]["linux"]["official"])
        self.assertEqual(updated["prod"]["linux"]["origin"], "SFIT official v6.7.7 prod")
        for item in updated["prod"]["linux"]["files"].values():
            self.assertNotIn("url", item)
            self.assertEqual(len(item["sha256"]), 64)
        self.assertEqual((self.stage / "ctp-trader.so").read_bytes(), b"thosttraderapi_se.so:official-prod")
        prov = json.loads((self.stage / "provenance.json").read_text(encoding="utf-8"))
        self.assertEqual((prov["profile"], prov["official"], prov["source_mode"]), ("prod", True, "local"))

        # Record official eval SDK and verify switching replaces staged binaries and provenance.
        prepare_ctp.stage_sdk(
            target_os="linux",
            target_arch="x86_64",
            profile="eval",
            manifest_path=self.manifest,
            stage_dir=self.stage,
            local_dir=eval_dir.parent,
            record_origin="Broker official v6.7.7 eval",
            require_official=True,
        )
        self.assertEqual((self.stage / "ctp-trader.so").read_bytes(), b"thosttraderapi_se.so:official-eval")
        prov = json.loads((self.stage / "provenance.json").read_text(encoding="utf-8"))
        self.assertEqual(prov["profile"], "eval")
        self.assertEqual(prov["origin"], "Broker official v6.7.7 eval")

        # Subsequent switch back to prod via --local-dir verifies pinned SHA-256 without network.
        prepare_ctp.stage_sdk(
            target_os="linux",
            target_arch="x86_64",
            profile="prod",
            manifest_path=self.manifest,
            stage_dir=self.stage,
            local_dir=prod_dir,
            require_official=True,
        )
        self.assertEqual((self.stage / "ctp-trader.so").read_bytes(), b"thosttraderapi_se.so:official-prod")

        # Tampered local file fails checksum verification without corrupting staged directory.
        (prod_dir / "thosttraderapi_se.so").write_bytes(b"tampered")
        with self.assertRaises(SystemExit) as mismatch:
            prepare_ctp.stage_sdk(
                target_os="linux",
                target_arch="x86_64",
                profile="prod",
                manifest_path=self.manifest,
                stage_dir=self.stage,
                local_dir=prod_dir,
            )
        self.assertIn("checksum mismatch", str(mismatch.exception))
        self.assertEqual((self.stage / "ctp-trader.so").read_bytes(), b"thosttraderapi_se.so:official-prod")

        # Without --local-dir and with an empty stage, an official URL-less manifest asks for --local-dir.
        with self.assertRaises(SystemExit) as need_local:
            prepare_ctp.stage_sdk(
                target_os="linux",
                target_arch="x86_64",
                profile="prod",
                manifest_path=self.manifest,
                stage_dir=self.root / "fresh-stage",
                require_official=True,
            )
        self.assertIn("--local-dir", str(need_local.exception))

    def test_tar_and_zip_archives_and_macos_framework_resolution(self):
        self.manifest.write_text(json.dumps(_base_manifest()), encoding="utf-8")
        # 1) Linux official .tar.gz archive
        linux_src = self.root / "linux-src"
        _write_official_linux_sdk(linux_src, b"tar-prod")
        tar_path = self.root / "ctp-linux.tar.gz"
        with tarfile.open(tar_path, "w:gz") as tf:
            for item in sorted(linux_src.rglob("*")):
                tf.add(item, arcname=f"v6.7.7/{item.relative_to(linux_src)}")
        prepare_ctp.stage_sdk(
            target_os="linux",
            target_arch="x86_64",
            profile="prod",
            manifest_path=self.manifest,
            stage_dir=self.stage / "linux",
            local_dir=tar_path,
            record_origin="SFIT tar.gz",
        )
        self.assertEqual((self.stage / "linux/ctp-md.so").read_bytes(), b"thostmduserapi_se.so:tar-prod")

        # 2) macOS official .zip archive with .framework/Versions/A layout
        mac_src = self.root / "mac-src"
        _write_official_macos_sdk(mac_src, b"mac-eval")
        zip_path = self.root / "ctp-mac.zip"
        with zipfile.ZipFile(zip_path, "w") as zf:
            for item in sorted(mac_src.rglob("*")):
                if item.is_file():
                    zf.write(item, arcname=f"v6.7.7_MacOS/{item.relative_to(mac_src)}")
        prepare_ctp.stage_sdk(
            target_os="macos",
            target_arch="armv8",
            profile="eval",
            manifest_path=self.manifest,
            stage_dir=self.stage / "mac",
            local_dir=zip_path,
            record_origin="SFIT macOS eval zip",
        )
        self.assertEqual((self.stage / "mac/ctp-md.dylib").read_bytes(), b"thostmduserapi_se:mac-eval")
        self.assertEqual((self.stage / "mac/ctp-trader.dylib").read_bytes(), b"thosttraderapi_se:mac-eval")

        # 3) Path traversal inside archive is rejected
        evil_zip = self.root / "evil.zip"
        with zipfile.ZipFile(evil_zip, "w") as zf:
            zf.writestr("../escape.so", b"bad")
        with self.assertRaises(SystemExit) as traversal:
            prepare_ctp.resolve_local_files(evil_zip, "linux")
        self.assertIn("leaves target directory", str(traversal.exception))

    def test_main_invokes_conan_export_pkg_with_staged_directory(self):
        self.manifest.write_text(json.dumps(_base_manifest()), encoding="utf-8")
        linux_src = self.root / "cli-sdk"
        _write_official_linux_sdk(linux_src, b"cli-eval")
        with (
            patch.object(prepare_ctp, "MANIFEST_PATH", self.manifest),
            patch.object(prepare_ctp, "ROOT", self.root),
            patch.object(prepare_ctp.subprocess, "run") as run_mock,
        ):
            rc = prepare_ctp.main([
                "--os", "linux", "--arch", "x86_64", "--profile", "eval",
                "--local-dir", str(linux_src), "--record-origin", "Broker CLI eval",
                "--require-official",
            ])
        self.assertEqual(rc, 0)
        self.assertEqual(run_mock.call_count, 2)
        export_cmd = run_mock.call_args_list[0].args[0]
        self.assertEqual(export_cmd[:3], ["conan", "export-pkg", str(self.root / "conan/ctp")])
        self.assertIn(f"user.ctp:sdk_root={self.root / 'build/ctp-sdk/linux-x86_64'}", export_cmd)

    def test_download_bytes_retries_transient_http_errors_only(self):
        import io
        import urllib.error

        calls = 0

        def flaky(url, timeout=60):
            nonlocal calls
            calls += 1
            if calls < 3:
                raise urllib.error.HTTPError(url, 503, "Service Unavailable", {}, None)
            return io.BytesIO(b"ctp-binary")

        with (
            patch.object(prepare_ctp.urllib.request, "urlopen", side_effect=flaky),
            patch.object(prepare_ctp.time, "sleep") as sleep,
        ):
            self.assertEqual(prepare_ctp.download_bytes("https://example.invalid/ctp.so"), b"ctp-binary")
        self.assertEqual(calls, 3)
        self.assertEqual([call.args[0] for call in sleep.call_args_list], [2.0, 4.0])

        not_found = urllib.error.HTTPError("https://example.invalid/missing", 404, "Not Found", {}, None)
        with (
            patch.object(prepare_ctp.urllib.request, "urlopen", side_effect=not_found) as opener,
            patch.object(prepare_ctp.time, "sleep") as sleep,
        ):
            with self.assertRaises(urllib.error.HTTPError):
                prepare_ctp.download_bytes("https://example.invalid/missing")
        self.assertEqual(opener.call_count, 1)
        sleep.assert_not_called()


if __name__ == "__main__":
    unittest.main()
