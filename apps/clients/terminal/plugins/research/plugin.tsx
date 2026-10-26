import { WorkloadAccess } from "../contract";
import { translate, type MessageValues } from "../contract";
const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.research", key, values);
import zh from "./locales/zh-CN.json";
import en from "./locales/en-US.json";
import { lazy } from "react";
import "./research.css";
import type { TerminalPlugin } from "../contract";
const Backtest = lazy(() => import("./Backtest").then(module => ({ default: module.Backtest })));
const AccountReview = lazy(() =>
  import("./AccountReview").then(module => ({ default: module.AccountReview })),
);
const ResearchSamples = lazy(() =>
  import("./ResearchSamples").then(m => ({ default: m.ResearchSamples })),
);
const ResearchStatistics = lazy(() =>
  import("./ResearchStatistics").then(module => ({ default: module.ResearchStatistics })),
);
const ResearchLogic = lazy(() =>
  import("./ResearchLogic").then(m => ({ default: m.ResearchLogic })),
);
const StrategyPackages = lazy(() =>
  import("./StrategyPackages").then(module => ({ default: module.StrategyPackages })),
);
const ResearchVariables = lazy(() =>
  import("./ResearchVariables").then(module => ({ default: module.ResearchVariables })),
);
const ResearchProjects = lazy(() =>
  import("./ResearchProjects").then(module => ({ default: module.ResearchProjects })),
);
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
    "task.action",
    "resource.catalogue",
    "research.logic.list",
    "research.logic.read",
    "research.logic.register",
    "research.logic.deprecate",
    "research.logic.examples",
    "research.logic.example.read",
    "research.logic.example.save",
    "research.logic.trial",
    "research.strategy.list",
    "research.strategy.read",
    "research.strategy.freeze",
    "research.strategy.logic.freeze",
    "research.strategy.deprecate",
    "research.strategy.delivery.create",
    "research.strategy.delivery.read",
    "research.strategy.delivery.list",
    "research.conclusion.read",
    "research.conclusion.list",
    "research.conclusion.append",
    "research.variable.check",
    "research.variable.read",
    "research.variable.list",
    "research.variable.register",
    "research.project.list",
    "research.project.read",
    "research.project.save",
    "research.plan.list",
    "research.plan.read",
    "research.plan.fix",
    "research.project.tasks",
    "task.page",
    "node.research.local.open",
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
    "statistics.submit",
    "evaluation.submit",
    "research.sample.submit",
    "account.review.submit",
    "factor.reuse",
    "data.tables.coverage",
    "data.tables.products",
    "task.action",
    "task.retirement.preview",
    "task.retirement.execute",
    "task.result",
    "task.result.read",
    "task.metadata.read",
    "task.metadata.edit",
    "task.evidence.export",
    "evaluation.report.export",
    "task.comparison.export",
    "task.quantstats.analyze",
    "task.portfolio.analyze",
  ],
  languageResources: { "zh-CN": zh, "en-US": en },
  tasks: context =>
    (context.snapshot?.research_service?.tasks ?? [])
      .filter(
        task =>
          task.kind === "backtest" ||
          task.kind === "factor" ||
          task.kind === "account_review" ||
          task.kind === "statistics" ||
          task.kind === "performance_evaluation" ||
          task.kind === "sample",
      )
      .map(task => ({
        id: `research:${context.snapshot?.research_service?.node_id}:${context.snapshot?.research_service?.service}:${task.id}`,
        title: `${task.instrument} · ${task.kind === "performance_evaluation" ? t("固定绩效评价") : task.kind === "sample" ? t("样本") : task.kind === "statistics" ? t("统计") : task.kind === "account_review" ? t("账户核对") : task.kind === "factor" ? t("因子分析") : `${t("策略回测")} · ${task.trading_day}`}`,
        status: t(
          task.retired
            ? "已退役"
            : {
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
          context.navigate(
            task.kind === "performance_evaluation"
              ? "workspace.performance"
              : task.kind === "sample"
                ? "workspace.research-samples"
                : task.kind === "statistics"
                  ? "workspace.research-statistics"
                  : task.kind === "account_review"
                    ? "workspace.account-review"
                    : task.kind === "backtest"
                      ? "workspace.backtest"
                      : "workspace.factor",
            {
              page: "research",
              params: {
                id:
                  task.kind === "performance_evaluation"
                    ? task.id
                    : `research:${context.snapshot?.research_service?.node_id}:${context.snapshot?.research_service?.service}:${task.id}`,
                kind: task.kind,
                ...(task.kind === "performance_evaluation"
                  ? {
                      connection_id: context.snapshot?.research_service?.connection_id ?? "",
                      service: context.snapshot?.research_service?.service ?? "",
                      owner_node_id: context.snapshot?.research_service?.owner_node_id ?? "",
                    }
                  : {}),
              },
            },
          ),
      })),
  workspaces: [
    {
      id: "workspace.research-samples",
      get title() {
        return t("样本");
      },
      component: context => (
        <WorkloadAccess context={context}>{ready => <ResearchSamples {...ready} />}</WorkloadAccess>
      ),
    },
    {
      id: "workspace.research-statistics",
      get title() {
        return t("统计");
      },
      component: context => (
        <WorkloadAccess context={context}>
          {ready => <ResearchStatistics {...ready} />}
        </WorkloadAccess>
      ),
    },
    {
      id: "workspace.research-projects",
      get title() {
        return t("研究项目");
      },
      component: context => (
        <WorkloadAccess context={context}>
          {ready => <ResearchProjects {...ready} />}
        </WorkloadAccess>
      ),
    },
    {
      id: "workspace.account-review",
      get title() {
        return t("账户核对");
      },
      component: context => (
        <WorkloadAccess context={context}>{ready => <AccountReview {...ready} />}</WorkloadAccess>
      ),
    },
    {
      id: "workspace.backtest",
      get title() {
        return t("回测");
      },

      component: context => (
        <WorkloadAccess context={context}>{ready => <Backtest {...ready} />}</WorkloadAccess>
      ),
    },
    {
      id: "workspace.factor",
      get title() {
        return t("因子");
      },

      component: context => (
        <WorkloadAccess context={context}>{ready => <Factor {...ready} />}</WorkloadAccess>
      ),
    },
    {
      id: "workspace.research-logic",
      get title() {
        return t("规则");
      },
      component: context => (
        <WorkloadAccess context={context}>{ready => <ResearchLogic {...ready} />}</WorkloadAccess>
      ),
    },
    {
      id: "workspace.strategy-packages",
      get title() {
        return t("策略包");
      },
      component: context => (
        <WorkloadAccess context={context}>
          {ready => <StrategyPackages {...ready} />}
        </WorkloadAccess>
      ),
    },
    {
      id: "workspace.research-variables",
      get title() {
        return t("变量");
      },
      component: context => (
        <WorkloadAccess context={context}>
          {ready => <ResearchVariables {...ready} />}
        </WorkloadAccess>
      ),
    },
    {
      id: "workspace.factor-comparison",
      get title() {
        return t("因子比较");
      },

      component: context => (
        <WorkloadAccess context={context}>
          {ready => <FactorComparison {...ready} />}
        </WorkloadAccess>
      ),
    },
    {
      id: "workspace.backtest-comparison",
      get title() {
        return t("回测比较");
      },

      component: context => (
        <WorkloadAccess context={context}>
          {ready => <BacktestComparison {...ready} />}
        </WorkloadAccess>
      ),
    },
    {
      id: "workspace.risk",
      get title() {
        return t("风险");
      },
      component: context => (
        <WorkloadAccess context={context}>
          {ready => <Evaluation {...ready} focus="risk" />}
        </WorkloadAccess>
      ),
    },
    {
      id: "workspace.performance",
      get title() {
        return t("绩效");
      },

      component: context => (
        <WorkloadAccess context={context}>{ready => <Evaluation {...ready} />}</WorkloadAccess>
      ),
    },
  ],
};
