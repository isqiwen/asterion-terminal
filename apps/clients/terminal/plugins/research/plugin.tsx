import { DataTaskAccess } from "../contract";
import { translate, type MessageValues } from "../contract";
const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.research", key, values);
import zh from "./locales/zh-CN.json";
import en from "./locales/en-US.json";
import { lazy } from "react";
import "./research.css";
import type { TerminalPlugin } from "../contract";
const Backtest = lazy(() => import("./Backtest").then(module => ({ default: module.Backtest })));
const Factor = lazy(() => import("./Factor").then(module => ({ default: module.Factor })));
const FactorComparison = lazy(() =>
  import("./FactorComparison").then(module => ({ default: module.FactorComparison })),
);
const BacktestComparison = lazy(() =>
  import("./BacktestComparison").then(module => ({ default: module.BacktestComparison })),
);
const Evaluation = lazy(() =>
  import("./Evaluation").then(module => ({ default: module.Evaluation })),
);
export const plugin: TerminalPlugin = {
  id: "asterion.terminal.research",
  commands: [
    "task.page",
    "node.data_tasks.local.open",
    "data.datasets",
    "data.dataset.saved",
    "data.dataset.save",
    "data.dataset.use",
    "data.dataset.select",
    "data.dataset.series",
    "data.dataset.daily",
    "data.dataset.product",
    "data.dataset.remove",
    "backtest.submit",
    "backtest.reuse",
    "backtest.budget.review",
    "factor.submit",
    "factor.reuse",
    "data.tables.coverage",
    "data.tables.products",
    "task.action",
    "task.result",
    "task.result.read",
    "task.metadata.read",
    "task.metadata.edit",
    "task.evidence.export",
    "task.comparison.export",
    "task.quantstats.analyze",
    "task.portfolio.analyze",
  ],
  languageResources: { "zh-CN": zh, "en-US": en },
  tasks: context =>
    (context.snapshot?.task_service?.tasks ?? [])
      .filter(task => task.kind === "backtest" || task.kind === "factor")
      .map(task => ({
        id: task.id,
        title: `${task.instrument} · ${task.kind === "factor" ? t("因子分析") : `${t("策略回测")} · ${task.trading_day}`}`,
        status: t(
          {
            queued: "排队中",
            running: "运行中",
            cancel_requested: "正在取消",
            publishing: "正在发布",
            succeeded: "已完成",
            failed: "失败",
            cancelled: "已取消",
            interrupted: "已中断",
          }[task.state],
        ),
        completed: task.completed,
        total: task.total,
        open: () =>
          context.navigate(task.kind === "backtest" ? "workspace.backtest" : "workspace.factor", {
            page: "task",
            params: { id: task.id, kind: task.kind },
          }),
      })),
  workspaces: [
    {
      id: "workspace.backtest",
      get title() {
        return t("回测");
      },

      component: context => (
        <DataTaskAccess context={context}>{ready => <Backtest {...ready} />}</DataTaskAccess>
      ),
    },
    {
      id: "workspace.factor",
      get title() {
        return t("因子");
      },

      component: context => (
        <DataTaskAccess context={context}>{ready => <Factor {...ready} />}</DataTaskAccess>
      ),
    },
    {
      id: "workspace.factor-comparison",
      get title() {
        return t("因子实验比较");
      },

      component: context => (
        <DataTaskAccess context={context}>
          {ready => <FactorComparison {...ready} />}
        </DataTaskAccess>
      ),
    },
    {
      id: "workspace.backtest-comparison",
      get title() {
        return t("回测实验比较");
      },

      component: context => (
        <DataTaskAccess context={context}>
          {ready => <BacktestComparison {...ready} />}
        </DataTaskAccess>
      ),
    },
    {
      id: "workspace.performance",
      get title() {
        return t("绩效");
      },

      component: context => (
        <DataTaskAccess context={context}>{ready => <Evaluation {...ready} />}</DataTaskAccess>
      ),
    },
  ],
};
