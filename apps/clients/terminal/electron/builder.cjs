const path = require("node:path");
const packageMode = process.env.ASTERION_PACKAGE_MODE;
if (!["release", "test"].includes(packageMode))
  throw new Error("Use desktop.py build or package-test to select a package purpose");
const distribution = packageMode === "release";
const mac = process.platform === "darwin";
if (
  mac &&
  distribution &&
  (!process.env.CSC_NAME || process.env.CSC_NAME === "-" || !process.env.APPLE_KEYCHAIN_PROFILE)
)
  throw new Error("Distribution requires Developer ID signing and a local notarization profile");
const electronExecutable = require("../../../../scripts/desktop/electron-path.cjs")();
const electronDist = mac
  ? electronExecutable.slice(0, electronExecutable.indexOf("/Electron.app/"))
  : path.dirname(electronExecutable);
const native = require("../../../../scripts/desktop/native-resources.json");
const nativeFiles = [
  ...native.programs,
  ...native.libraries.map(name => name + (mac ? ".dylib" : ".so")),
];
module.exports = {
  appId: "me.asterion.terminal",
  productName: "Asterion Terminal",
  executableName: "asterion_terminal",
  directories: {
    app: "apps/clients/terminal/electron",
    output: "build/terminal-package",
    buildResources: "apps/clients/terminal/electron/assets",
  },
  files: [
    "main.cjs",
    "preload.cjs",
    "lab-channel.cjs",
    "package.json",
    { from: "../dist", to: "dist" },
  ],
  extraResources: [
    { from: "build/electron-resources/python", to: "python", filter: ["**/*"] },
    {
      from: "build/electron-resources/native",
      to: "native",
      filter: [
        ...nativeFiles,
        ...native.configuration,
        ...(!mac ? ["runtime-requirements.json"] : []),
      ],
    },
  ],
  asar: true,
  electronDist,
  npmRebuild: false,
  nodeGypRebuild: false,
  artifactName: distribution
    ? "Asterion-Terminal-${version}-${os}-${arch}.${ext}"
    : "Asterion-Terminal-TEST-${version}-${os}-${arch}.${ext}",
  forceCodeSigning: mac && distribution,
  mac: {
    target: ["dir"],
    category: "public.app-category.finance",
    minimumSystemVersion: "13.0",
    type: "distribution",
    identity: distribution ? process.env.CSC_NAME : "-",
    notarize: distribution,
    binaries: [
      ...nativeFiles.map(name => "Contents/Resources/native/" + name),
      "Contents/Resources/python/bin/python3",
    ],
    hardenedRuntime: true,
    entitlements: path.join(__dirname, "entitlements.mac.plist"),
    entitlementsInherit: path.join(__dirname, "entitlements.mac.plist"),
  },
  dmg: { sign: distribution },
  linux: {
    target: ["dir"],
    category: "Finance",
    synopsis: "Futures trading terminal",
    maintainer: "Asterion <build@example.invalid>",
    syncDesktopName: true,
  },
};
