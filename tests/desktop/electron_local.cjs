// Where the Terminal keeps this user's local environments on each platform.
const os = require("node:os");
const path = require("node:path");
const execFile = require("node:util").promisify(require("node:child_process").execFile);
const mac = process.platform === "darwin";
const home = os.homedir();
const data = path.join(home, mac ? "Library/Application Support" : ".local/share");
module.exports = {
  library: mac ? ".dylib" : ".so",
  production: path.join(data, mac ? "Asterion/node" : "asterion/node"),
  development: path.join(data, mac ? "Asterion Development/node" : "asterion-development/node"),
  // Electron's own profile directory.
  profile: name => path.join(mac ? data : path.join(home, ".config"), name),
  // The definition the user's service manager reads for an Agent.
  definition: name =>
    mac
      ? path.join(home, "Library/LaunchAgents", name + ".plist")
      : path.join(home, ".config/systemd/user", name + ".service"),
  // The process the service manager currently runs for an Agent.
  async servicePid(name) {
    if (mac) {
      const { stdout } = await execFile("/bin/launchctl", [
        "print",
        `gui/${process.getuid()}/${name}`,
      ]);
      return Number(/^\s*pid = (\d+)$/m.exec(stdout)?.[1]);
    }
    const { stdout } = await execFile("/usr/bin/systemctl", [
      "--user",
      "show",
      "--property=MainPID",
      "--value",
      name + ".service",
    ]);
    return Number(stdout.trim());
  },
};
