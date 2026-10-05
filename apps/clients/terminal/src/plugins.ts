import { registerTerminalPlugins } from "./host/plugin-registry";
import {
  plugin as market,
  watchlistPlugin,
  contractPlugin,
} from "../plugins/futures-market/plugin";
import { plugin as data } from "../plugins/data-workbench/plugin";
import { plugin as backtestFactor } from "../plugins/backtest-factor/plugin";
import { plugin as trading } from "../plugins/trading/plugin";

// The product selects its plugins; each plugin owns its contributions.
export const terminalPlugins = registerTerminalPlugins([
  watchlistPlugin,
  contractPlugin,
  market,
  data,
  backtestFactor,
  trading,
]);
export const workspaces = terminalPlugins.map(plugin => plugin.workspace);

export const workspaceShortcuts = workspaces.map(workspace => workspace.id);
