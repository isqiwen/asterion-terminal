"""Build one desktop installer using platform-managed PostgreSQL."""

import argparse
import json
import os
import platform
import plistlib
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from desktop_dependencies import LINUX_PACKAGES, ensure_linux, ensure_macos

ROOT = Path(__file__).resolve().parents[1]


def dependency_hint(host: str) -> str:
    if host == "linux":
        return (
            "On Debian 13, install the system build dependencies once:\n"
            "  sudo apt update\n"
            f"  sudo apt install {' '.join(LINUX_PACKAGES)}\n"
            "libpq-dev provides pg_config. Other distributions: see README.md.\n"
            "Then rerun Asterion: Build as your normal user."
        )
    return "Install the build prerequisites listed in README.md, then rerun Asterion: Build."


def run(*args, **kwargs):
    subprocess.run(list(args), cwd=ROOT, check=True, **kwargs)


def bundle_diagnostics() -> str:
    """Report unreadable payload entries while preserving the failed bundle for inspection."""
    directory = ROOT / "apps/terminal/src-tauri/target/release/bundle/deb"
    problems = []
    for path in directory.rglob("*"):
        try:
            if not path.is_dir():
                with path.open("rb") as file:
                    file.read(1)
        except OSError as error:
            problems.append(f"  {path}: {error}")
            if len(problems) == 5:
                break
    if not directory.exists():
        problems.append(f"  Missing bundle directory: {directory}")
    return (
        "\n".join(problems)
        or f"No unreadable files remain. Inspect the build output and {directory}."
    )


def export_artifact(source: Path) -> Path:
    """Publish a completed bundle without copying compiler caches or stale files."""
    if not source.is_file():
        raise FileNotFoundError(f"Build artifact not found: {source}")
    release = ROOT / "release"
    release.mkdir(exist_ok=True)
    destination = release / "_".join(source.name.split())
    with tempfile.TemporaryDirectory(prefix=".export-", dir=release) as temporary:
        staged = Path(temporary) / source.name
        shutil.copy2(source, staged)
        staged.replace(destination)
    print(f"\nRelease artifact: {destination}", flush=True)
    return destination


def verify_dmg(image: Path, product_name: str) -> None:
    """Verify the signed application actually contained in the published image."""
    run("/usr/bin/hdiutil", "verify", str(image))
    mount = Path(tempfile.mkdtemp(prefix="asterion-dmg-verify-"))
    try:
        run(
            "/usr/bin/hdiutil",
            "attach",
            "-readonly",
            "-nobrowse",
            "-mountpoint",
            str(mount),
            str(image),
        )
    except BaseException:
        mount.rmdir()
        raise
    try:
        with tempfile.TemporaryDirectory(prefix="asterion-pkg-verify-") as temporary:
            expanded = Path(temporary) / "expanded"
            run(
                "/usr/sbin/pkgutil",
                "--expand-full",
                str(mount / f"{product_name}.pkg"),
                str(expanded),
            )
            run(
                "/usr/bin/codesign",
                "--verify",
                "--deep",
                "--strict",
                str(expanded / "application.pkg/Payload/Applications" / f"{product_name}.app"),
            )
    finally:
        # Leave the mount point intact if detach fails; never recurse into a mounted image.
        run("/usr/bin/hdiutil", "detach", str(mount))
        mount.rmdir()


def macos_installer(app: Path, output: Path, config: dict) -> Path:
    """A DMG containing a fixed-location Installer package, never a second app copy."""
    output.mkdir(parents=True, exist_ok=True)
    architecture = {"arm64": "aarch64", "x86_64": "x64"}[platform.machine()]
    image = output / f"{config['productName']}_{config['version']}_{architecture}.dmg"
    run("/usr/bin/codesign", "--verify", "--deep", "--strict", str(app))
    with tempfile.TemporaryDirectory(prefix="asterion-installer-") as temporary:
        stage = Path(temporary)
        root = stage / "root"
        destination = root / "Applications" / app.name
        destination.parent.mkdir(parents=True)
        shutil.copytree(app, destination, symlinks=True)
        components = stage / "components.plist"
        components.write_bytes(
            plistlib.dumps(
                [
                    {
                        "RootRelativeBundlePath": f"Applications/{app.name}",
                        "BundleIsRelocatable": False,
                        "BundleIsVersionChecked": False,
                        "BundleHasStrictIdentifier": True,
                        "BundleOverwriteAction": "upgrade",
                    }
                ]
            )
        )
        package_id = config["identifier"]
        run(
            "/usr/bin/pkgbuild",
            "--root",
            str(root),
            "--component-plist",
            str(components),
            "--identifier",
            package_id,
            "--version",
            config["version"],
            "--install-location",
            "/",
            "--scripts",
            str(ROOT / "scripts/macos-installer"),
            str(stage / "application.pkg"),
        )
        # Restrict Installer to the system volume; the payload always replaces the same bundle.
        from xml.sax.saxutils import escape

        distribution = stage / "distribution.xml"
        distribution.write_text(
            '<?xml version="1.0" encoding="utf-8"?>\n<installer-gui-script minSpecVersion="1">'
            f"<title>{escape(config['productName'])}</title>"
            '<options customize="never"/>'
            '<domains enable_anywhere="false" enable_currentUserHome="false" enable_localSystem="true"/>'
            '<choices-outline><line choice="application"/></choices-outline>'
            f'<choice id="application" visible="false"><pkg-ref id="{package_id}"/></choice>'
            f'<pkg-ref id="{package_id}" version="{config["version"]}" '
            'onConclusion="None">application.pkg</pkg-ref></installer-gui-script>\n'
        )
        contents = stage / "image"
        contents.mkdir()
        run(
            "/usr/bin/productbuild",
            "--distribution",
            str(distribution),
            "--package-path",
            str(stage),
            str(contents / f"{config['productName']}.pkg"),
        )
        run(
            "/usr/bin/hdiutil",
            "create",
            "-ov",
            "-format",
            "UDZO",
            "-volname",
            config["productName"],
            "-srcfolder",
            str(contents),
            str(image),
        )
    verify_dmg(image, config["productName"])
    return image


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--prepare-only", action="store_true")
    parser.add_argument("--reuse-setup", action="store_true")
    parser.add_argument("--platform", choices=["auto", "macos", "linux"], default="auto")
    parser.add_argument("--smoke-test", action="store_true")
    parser.add_argument(
        "--no-install", action="store_true", help="Check dependencies without offering installation"
    )
    args = parser.parse_args()
    host = {"darwin": "macos", "linux": "linux"}.get(sys.platform)
    if host is None or args.platform not in {"auto", host}:
        raise SystemExit(
            "Build on the target OS: macOS for .dmg, Linux for .deb. "
            "Windows .exe (NSIS) is specified but its runtime is not supported yet."
        )
    if os.geteuid() == 0:
        raise SystemExit("Run the build as a normal user. Only apt installation uses sudo.")
    toolchain = ROOT / ".state/toolchain"
    env = os.environ.copy()
    if (toolchain / "cargo/bin/cargo").exists():
        env.update(CARGO_HOME=str(toolchain / "cargo"), RUSTUP_HOME=str(toolchain / "rustup"))
        env["PATH"] = f"{toolchain}/cargo/bin:/usr/bin:/bin:" + env["PATH"]
    required = ["uv"]
    if not args.prepare_only:
        required += ["pnpm"]
        if not (toolchain / "cargo/bin/cargo").exists():
            required += ["cargo"]
    missing = [tool for tool in required if shutil.which(tool) is None]
    if missing:
        raise SystemExit(
            "Install the developer tools first: " + ", ".join(missing) + "; see README.md"
        )
    if not args.no_install or host == "macos":
        ensure = ensure_linux if host == "linux" else ensure_macos
        ensure(
            runtime=args.smoke_test,
            desktop=not args.prepare_only,
            interactive=not args.no_install,
        )
    if host == "linux" and not args.prepare_only:
        required += ["pkg-config", "dpkg-deb"]
    missing = [tool for tool in required if shutil.which(tool) is None]
    if missing:
        raise SystemExit(
            "Missing build tools: " + ", ".join(missing) + "\n" + dependency_hint(host)
        )
    if host == "linux" and not args.prepare_only:
        result = subprocess.run(
            ["pkg-config", "--exists", "webkit2gtk-4.1", "gtk+-3.0", "openssl"],
            check=False,
        )
        if result.returncode:
            raise SystemExit("Missing Linux development libraries.\n" + dependency_hint(host))
    from prepare_runtime import prepare

    bundle = ROOT / "apps/terminal/src-tauri/setup"
    if not args.reuse_setup:
        prepare()
    elif not (bundle / "manifest.json").is_file():
        raise SystemExit("Installer inputs missing; run without --reuse-setup")
    if args.smoke_test:
        manifest_path = ROOT / "apps/terminal/src-tauri/Cargo.toml"
        result = subprocess.run(
            [
                "cargo",
                "run",
                "--manifest-path",
                str(manifest_path),
                "--example",
                "runtime-setup",
                "--",
                str(bundle),
                str(ROOT / ".state/setup-smoke"),
            ],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            check=True,
            env=env,
        )
        installed = result.stdout.strip().splitlines()[-1]
        run(
            str(Path(installed) / "environment/bin/python"),
            "-I",
            "scripts/smoke_desktop_runtime.py",
            "--runtime",
            installed,
        )
    if not args.prepare_only:
        try:
            run(
                "pnpm",
                "--filter",
                "@asterion/terminal",
                "tauri",
                "build",
                "--bundles",
                "app" if host == "macos" else "deb",
                env=env,
            )
        except subprocess.CalledProcessError as error:
            detail = bundle_diagnostics() if host == "linux" else "See the build output above."
            raise SystemExit(
                f"Desktop build failed (exit {error.returncode}).\n{detail}\n"
                "Existing release artifacts were not updated."
            ) from None

        config = json.loads((ROOT / "apps/terminal/src-tauri/tauri.conf.json").read_text())
        output = ROOT / "apps/terminal/src-tauri/target/release/bundle"
        if host == "macos":
            bundle = macos_installer(
                output / "macos" / f"{config['productName']}.app", output / "dmg", config
            )
        else:
            architecture = subprocess.check_output(
                ["dpkg", "--print-architecture"], text=True
            ).strip()
            filename = f"{config['productName']}_{config['version']}_{architecture}.deb"
            bundle = output / "deb" / filename
        export_artifact(bundle)


if __name__ == "__main__":
    main()
