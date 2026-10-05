const path = require("node:path");
const packageMode = process.env.ASTERION_PACKAGE_MODE;
if (!["release", "test"].includes(packageMode))
  throw new Error("Use desktop.py build or package-test to select a package purpose");
const distribution = packageMode === "release";
if (
  distribution &&
  (!process.env.CSC_NAME || process.env.CSC_NAME === "-" || !process.env.APPLE_KEYCHAIN_PROFILE)
)
  throw new Error("Distribution requires Developer ID signing and a local notarization profile");
const electronExecutable = require("../../../../scripts/electron-path.cjs")();
const electronDist = electronExecutable.slice(0, electronExecutable.indexOf("/Electron.app/"));
const nativeFiles = require("../../../../scripts/native-resources.json").files;
module.exports = {
  appId: "me.asterion.terminal",
  productName: "Asterion Terminal",
  executableName: "asterion-terminal",
  directories: {
    app: "apps/clients/terminal/electron",
    output: distribution ? "build/desktop" : "build/desktop-test",
    buildResources: "apps/clients/terminal/electron/assets",
  },
  files: ["main.cjs", "preload.cjs", "package.json", { from: "../dist", to: "dist" }],
  extraResources: [
    { from: "build/electron-resources/native", to: "native", filter: nativeFiles },
    { from: "build/electron-resources/remote-linux", to: "remote-linux", filter: ["**/*"] },
  ],
  asar: true,
  electronDist,
  npmRebuild: false,
  nodeGypRebuild: false,
  artifactName: distribution
    ? "Asterion-Terminal-${version}-${os}-${arch}.${ext}"
    : "Asterion-Terminal-TEST-${version}-${os}-${arch}.${ext}",
  forceCodeSigning: distribution,
  mac: {
    target: ["dmg"],
    category: "public.app-category.finance",
    minimumSystemVersion: "13.0",
    type: "distribution",
    identity: distribution ? process.env.CSC_NAME : "-",
    notarize: distribution,
    binaries: nativeFiles.map(name => "Contents/Resources/native/" + name),
    hardenedRuntime: true,
    entitlements: path.join(__dirname, "entitlements.mac.plist"),
    entitlementsInherit: path.join(__dirname, "entitlements.mac.plist"),
  },
  dmg: { sign: distribution },
};
