import { registerTerminalFeatures } from "./host/feature-registry";
import { feature as market } from "../features/futures-market/feature";
import { feature as data } from "../features/data-workbench/feature";
import { feature as research } from "../features/research/feature";
import { feature as trading } from "../features/trading/feature";

// Built-in application features; there is no third-party UI loader.
export const terminalFeatures = registerTerminalFeatures([market, data, research, trading]);
export const workspaces = terminalFeatures.flatMap(feature => feature.workspaces);
