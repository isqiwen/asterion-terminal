import { registerTerminalPlugins } from "./host/plugin-registry";
import { plugin as market } from "../plugins/futures-market/plugin";
import { plugin as data } from "../plugins/data-workbench/plugin";
import { plugin as research } from "../plugins/research/plugin";
import { plugin as intelligence } from "../plugins/intelligence/plugin";
import { plugin as trading } from "../plugins/trading/plugin";

// The product selects its plugins; each plugin owns its contributions.
export const terminalPlugins = registerTerminalPlugins([
  market,
  data,
  intelligence,
  research,
  trading,
]);
export const workspaces = terminalPlugins.flatMap(plugin => plugin.workspaces);
