import { Icon } from "../contract";
import { translate, type MessageValues } from "../contract";
const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.trading", key, values);
import { AccountSummary } from "./AccountSummary";
import zh from "./locales/zh-CN.json";
import en from "./locales/en-US.json";
import { lazy } from "react";
import type { TerminalPlugin } from "../contract";
const Workspace = lazy(() => import("./Workspace").then(module => ({ default: module.Workspace })));
export const plugin: TerminalPlugin = {
  id: "asterion.terminal.trading",
  apiVersion: 1,
  commands: [
    "paper.create",
    "paper.open",
    "paper.close",
    "paper.act",
    "live.create",
    "live.open",
    "live.connect",
    "live.disconnect",
    "live.costs",
    "live.act",
    "live.close",
    "research.dataset.select",
    "research.dataset.remove",
    "strategy.run",
    "strategy.revoke",
  ],
  languageResources: { "zh-CN": zh, "en-US": en },
  workspace: {
    id: "workspace.trading",
    get title() {
      return t("交易");
    },
    icon: <Icon name="trading" />,
    component: context => <Workspace {...context} />,
  },
  widgets: context => {
    const paper = context.snapshot?.paper;
    const recovery = paper?.storage_state === "recovery_required";
    return [
      {
        id: "trading.portfolio",
        title: t("账户与持仓"),
        category: t("交易"),
        description: t("期货账户连接状态"),
        width: 1,
        column: "secondary",
        defaultVisible: true,
        render: () => <AccountSummary context={context} />,
      },
      {
        id: "trading.risk-metrics",
        title: t("风险概览"),
        category: t("交易"),
        description: t("期货风险与执行状态"),
        width: 1,
        column: "secondary",
        defaultVisible: false,
        hasContent: !!paper,
        render: () => (
          <div className="dashboard-empty">
            <strong>
              {recovery
                ? t("风险状态待恢复")
                : paper
                  ? t("可用资金 {p0}", { p0: paper.available })
                  : t("暂无模拟账户")}
            </strong>
            <p>
              {paper
                ? t("保证金 {p0} · 委托冻结 {p1}", { p0: paper.margin, p1: paper.frozen })
                : t("在交易工作区创建模拟或实盘会话。")}
            </p>
          </div>
        ),
      },
    ];
  },
};
