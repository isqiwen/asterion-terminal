const fs = require("node:fs");
const path = require("node:path");
const terminal = require("../../terminal/electron/builder.cjs");
const mac = process.platform === "darwin";
const distribution = process.env.ASTERION_PACKAGE_MODE === "release";
// The Terminal package just built: on macOS the one application bundle there.
// A missing payload fails here rather than ship an installer without it.
const packaged = path.resolve("build/terminal-package", mac ? "mac-arm64" : "linux-unpacked");
const bundles = mac ? fs.readdirSync(packaged).filter(name => name.endsWith(".app")) : [];
if (mac && bundles.length !== 1)
  throw new Error(`Expected one Terminal application in ${packaged}, found ${bundles.length}`);
const payload = mac ? path.join(packaged, bundles[0]) : packaged;
if (!fs.existsSync(path.join(payload, mac ? "Contents/Resources/native" : "resources/native")))
  throw new Error("The Terminal package has no native resources: " + payload);
module.exports = {
  appId: "me.asterion.installer",
  productName: "Asterion Installer",
  executableName: "asterion_installer",
  directories: {
    app: "apps/clients/installer/electron",
    buildResources: "apps/clients/terminal/electron/assets",
    output: distribution ? "build/desktop" : "build/desktop-test",
  },
  files: ["main.cjs", "preload.cjs", "package.json", { from: "../../terminal/dist", to: "dist" }],
  extraResources: [{ from: payload, to: mac ? "terminal.app" : "terminal", filter: ["**/*"] }],
  asar: true,
  electronDist: terminal.electronDist,
  npmRebuild: false,
  nodeGypRebuild: false,
  artifactName: distribution
    ? "Asterion-Installer-${version}-${os}-${arch}.${ext}"
    : "Asterion-Installer-TEST-${version}-${os}-${arch}.${ext}",
  forceCodeSigning: terminal.forceCodeSigning,
  mac: { ...terminal.mac, binaries: [], target: ["dmg"] },
  dmg: { sign: distribution, contents: [{ x: 270, y: 180, type: "file" }] },
  linux: {
    target: ["tar.gz"],
    category: "Utility",
    synopsis: "Install and maintain Asterion Terminal",
  },
};
