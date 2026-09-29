import { registerTerminalPlugins } from "./host/plugin-registry";
import { plugin as overview } from "../plugins/overview/plugin";
import {
  plugin as market,
  watchlistPlugin,
  contractPlugin,
} from "../plugins/futures-market/plugin";
import { plugin as data } from "../plugins/data-workbench/plugin";
import { plugin as research } from "../plugins/research/plugin";
import { plugin as trading } from "../plugins/trading/plugin";

// The product selects its plugins; each plugin owns its contributions.
export const terminalPlugins = registerTerminalPlugins([
  watchlistPlugin,
  contractPlugin,
  market,
  overview,
  data,
  research,
  trading,
]);
export const workspaces = terminalPlugins.map(plugin => plugin.workspace);

export const workspaceShortcuts = [
  overview,
  market,
  data,
  research,
  trading,
  watchlistPlugin,
  contractPlugin,
].map(plugin => plugin.workspace.id);
