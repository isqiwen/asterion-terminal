import { Icon } from "../contract";
import { translate, type MessageValues } from "../contract";
const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.trading", key, values);
import zh from "./locales/zh-CN.json";
import en from "./locales/en-US.json";
import { lazy } from "react";
import type { TerminalPlugin } from "../contract";
const LivePanel = lazy(() => import("./LivePanel").then(module => ({ default: module.LivePanel })));
export const plugin: TerminalPlugin = {
  id: "asterion.terminal.trading",
  apiVersion: 1,
  commands: [
    "market.catalog",
    "live.create",
    "live.open",
    "live.connect",
    "live.disconnect",
    "live.costs",
    "live.act",
    "live.close",
  ],
  languageResources: { "zh-CN": zh, "en-US": en },
  workspace: {
    id: "workspace.trading",
    get title() {
      return t("交易");
    },
    icon: <Icon name="trading" />,
    component: context => <LivePanel {...context} />,
  },
};
