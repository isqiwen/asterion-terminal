const path = require("node:path");
const fs = require("node:fs");
const build = process.env.ASTERION_CPP_BUILD || path.resolve("build/Release");
const product = JSON.parse(fs.readFileSync(path.join(build, "product.json"), "utf8"));
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
  extraMetadata: { version: product.version, buildCommit: product.commit },
  productName: "Asterion Terminal",
  executableName: "asterion_terminal",
  directories: {
    app: "apps/clients/terminal/electron",
    output: distribution ? "build/desktop" : "build/desktop-test",
    buildResources: "apps/clients/terminal/electron/assets",
  },
  files: [
    "main.cjs",
    "diagnostics.cjs",
    "preload.cjs",
    "lab-channel.cjs",
    "package.json",
    { from: "../dist", to: "dist" },
  ],
  extraResources: [
    { from: "build/electron-resources/crash", to: "crash" },
    ...(mac ? [{ from: "build/electron-resources/symbols", to: "symbols" }] : []),
    // The Python runtime is installed at the first launch from this manifest;
    // the application ships only its own Python code beside it.
    { from: "build/electron-resources/runtime", to: "runtime", filter: ["**/*"] },
    // The strategy directory itself; the installed Terminal researches with it.
    { from: "research", to: "research", filter: ["**/*.py"] },
    {
      from: "build/electron-resources/native",
      to: "native",
      filter: [...nativeFiles, ...native.configuration],
    },
  ],
  asar: true,
  // Updates come from a new package; no update feed.
  publish: null,
  electronDist,
  npmRebuild: false,
  nodeGypRebuild: false,
  artifactName: "Asterion-Terminal-${version}-${os}-${arch}.${ext}",
  forceCodeSigning: mac && distribution,
  mac: {
    target: ["dmg"],
    category: "public.app-category.finance",
    minimumSystemVersion: "13.0",
    type: "distribution",
    identity: distribution ? process.env.CSC_NAME : "-",
    notarize: distribution,
    binaries: [
      ...nativeFiles.map(name => "Contents/Resources/native/" + name),
      "Contents/Resources/crash/minidump-stackwalk",
    ],
    hardenedRuntime: true,
    entitlements: path.join(__dirname, "entitlements.mac.plist"),
    entitlementsInherit: path.join(__dirname, "entitlements.mac.plist"),
  },
  dmg: { sign: distribution },
  linux: {
    target: ["deb"],
    category: "Finance",
    synopsis: "Futures trading terminal",
    maintainer: "Asterion <build@example.invalid>",
    syncDesktopName: true,
  },
  deb: {
    // Electron's defaults name what it loads at run time, which no linker
    // record shows; desktop.py adds what the packaged programs link against.
    depends: [
      "libgtk-3-0",
      "libnotify4",
      "libnss3",
      "libxss1",
      "libxtst6",
      "xdg-utils",
      "libatspi2.0-0",
      "libuuid1",
      "libsecret-1-0",
      ...JSON.parse(process.env.ASTERION_DEB_DEPENDS || "[]"),
    ],
  },
};
