import type { TerminalContext, TerminalFeature } from "../../src/host/feature-registry";
import { type MessageValues, translate } from "../../src/i18n";
const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.data-workbench", key, values);
import zh from "./locales/zh-CN.json";
import en from "./locales/en-US.json";
import "./data.css";
import { Coverage } from "./Coverage";
// The data page reads the remote Warehouse dataset and its acquisitions, and the Market status.
export const feature: TerminalFeature = {
  id: "asterion.terminal.data-workbench",
  languageResources: { "zh-CN": zh, "en-US": en },
  workspaces: [
    {
      id: "workspace.coverage",
      get title() {
        return t("数据覆盖");
      },
      component: (context: TerminalContext) => <Coverage {...context} />,
    },
  ],
};
