const path = require("node:path");
const terminal = require("../../terminal/electron/builder.cjs");
const mac = process.platform === "darwin";
const distribution = process.env.ASTERION_PACKAGE_MODE === "release";
const payload = path.resolve(
  "build/terminal-package",
  mac ? "mac-arm64/Asterion Terminal.app" : "linux-unpacked",
);
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
