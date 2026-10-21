// Internal recovery after the owned development Electron process exits.
const path = require("node:path");
process.env.ASTERION_ENVIRONMENT = "development";
if (process.env.ASTERION_NODE_DIRECTORY) process.exit(0);
const resources = path.resolve(__dirname, "../../build/electron-resources/native");
process.env.ASTERION_NODE_MANAGER_EXECUTABLE = path.join(resources, "asterion_node_manager");
process.env.ASTERION_NODE_AGENT_EXECUTABLE = path.join(resources, "asterion_node_agent");
const native = require(path.join(resources, "asterion_terminal.node"));
(async () => {
  const response = JSON.parse(
    await native.request(
      JSON.stringify({ version: 1, method: "development.shutdown", params: { recover: true } }),
    ),
  );
  if (response.error) throw new Error(response.error.message);
})().catch(error => {
  console.error(`Development cleanup incomplete; services may remain: ${error.message}`);
  process.exitCode = 1;
});
