"""Required macOS distribution checks; credentials stay in the local Keychain."""
import hashlib
import json
from pathlib import Path
import platform
import re
import subprocess


def output(arguments):
    return subprocess.check_output(arguments, text=True, stderr=subprocess.STDOUT)


def developer_id_team(description):
    lines = description.splitlines()
    teams = [line.removeprefix("TeamIdentifier=") for line in lines if line.startswith("TeamIdentifier=")]
    authorities = [line.removeprefix("Authority=") for line in lines if line.startswith("Authority=")]
    if (len(teams) != 1 or not teams[0].strip() or teams[0] == "not set"
            or not authorities or not authorities[0].startswith("Developer ID Application: ")
            or "Signature=adhoc" in lines):
        return None
    return teams[0]


def has_hardened_runtime(description):
    directories = [line for line in description.splitlines() if line.startswith("CodeDirectory ")]
    if len(directories) != 1:
        return False
    flags = [field.removeprefix("flags=") for field in directories[0].split() if field.startswith("flags=")]
    if len(flags) != 1:
        return False
    match = re.fullmatch(r"0x([0-9a-fA-F]+)(?:\([^)]*\))?", flags[0])
    if match is None:
        return False
    value = int(match[1], 16)
    # CS_RUNTIME must be present and CS_ADHOC must be absent in the signed flags.
    return bool(value & 0x10000) and not bool(value & 0x2)


def signing_preflight(environment):
    identity = environment.get("CSC_NAME", "").strip()
    if not identity or identity == "-" or identity.startswith("Developer ID Application:"):
        raise ValueError("Distribution requires CSC_NAME (Developer ID certificate hash or name without its prefix)")
    if not environment.get("APPLE_KEYCHAIN_PROFILE", "").strip():
        raise ValueError("Distribution requires APPLE_KEYCHAIN_PROFILE stored locally by notarytool")
    identities = output(["security", "find-identity", "-v", "-p", "codesigning"])
    matches = [line for line in identities.splitlines()
               if '"Developer ID Application:' in line and identity in line]
    if len(matches) != 1:
        raise ValueError("Distribution requires exactly one available Developer ID Application identity matching CSC_NAME")
    # Only the named local profile supplies notarization credentials.
    for name in ("APPLE_ID", "APPLE_APP_SPECIFIC_PASSWORD", "APPLE_TEAM_ID", "APPLE_API_KEY",
                 "APPLE_API_KEY_ID", "APPLE_API_ISSUER"):
        environment.pop(name, None)


def notarize_installer(installer, environment):
    command = ["xcrun", "notarytool", "submit", str(installer), "--wait", "--output-format", "json",
               "--keychain-profile", environment["APPLE_KEYCHAIN_PROFILE"]]
    if environment.get("APPLE_KEYCHAIN"):
        command += ["--keychain", environment["APPLE_KEYCHAIN"]]
    result = json.loads(output(command))
    if result.get("status") != "Accepted":
        raise ValueError("Installer notarization was not accepted")
    subprocess.run(["xcrun", "stapler", "staple", str(installer)], check=True)
    verify_installer(installer)


def verify_installer(installer):
    subprocess.run(["codesign", "--verify", "--strict", str(installer)], check=True)
    description = output(["codesign", "--display", "--verbose=4", str(installer)])
    if developer_id_team(description) is None:
        raise ValueError("Distribution installer lacks a Developer ID signature")
    subprocess.run(["xcrun", "stapler", "validate", str(installer)], check=True)
    subprocess.run(["spctl", "--assess", "--type", "open", "--context",
                    "context:primary-signature", str(installer)], check=True)


def verify_application(app, native_files):
    subprocess.run(["codesign", "--verify", "--deep", "--strict", str(app)], check=True)
    description = output(["codesign", "--display", "--verbose=4", str(app)])
    team = developer_id_team(description)
    if team is None:
        raise ValueError("Distribution application lacks a Developer ID signature")
    if not has_hardened_runtime(description):
        raise ValueError("Distribution application lacks hardened runtime signing")
    arch = {"arm64": "arm64", "x86_64": "x86_64"}[platform.machine()]
    if arch not in output(["lipo", "-archs", str(app / "Contents/MacOS/asterion-terminal")]).split():
        raise ValueError("Application architecture differs from its acceptance host")
    for name in sorted(native_files):
        binary = app / "Contents/Resources/native" / name
        subprocess.run(["codesign", "--verify", "--strict", str(binary)], check=True)
        signature = output(["codesign", "--display", "--verbose=4", str(binary)])
        if developer_id_team(signature) != team:
            raise ValueError("Native resource does not carry the application's Developer ID: " + name)
        if arch not in output(["lipo", "-archs", str(binary)]).split():
            raise ValueError("Native resource has no executable architecture for this host: " + name)
    subprocess.run(["xcrun", "stapler", "validate", str(app)], check=True)
    subprocess.run(["spctl", "--assess", "--type", "execute", str(app)], check=True)


def write_evidence(installer, installed_report):
    acceptance = json.loads(installed_report.read_text())
    digest = hashlib.sha256(installer.read_bytes()).hexdigest()
    if acceptance.get("status") != "passed" or acceptance.get("sha256") != digest:
        raise ValueError("Installed acceptance does not prove this exact notarized installer")
    report = {"status": "passed", "architecture": platform.machine(), "sha256": digest,
              "installer": installer.name, "installed_acceptance": str(installed_report),
              "checks": ["Developer ID application and DMG", "matching native signing team",
                         "native architecture", "app and DMG notarization tickets", "Gatekeeper",
                         "isolated installed application acceptance"]}
    (installer.parent / "distribution-acceptance.json").write_text(json.dumps(report, indent=2) + "\n")
