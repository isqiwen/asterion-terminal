import {
  strategiesOf,
  strategyDefaults,
  useWorkspaceDraft,
  type TerminalContext,
} from "../contract";
import type { ResearchStrategyPackage } from "../../src/bridge/client";
import { portfolioEligible, portfolioPolicy, portfolioPolicyDefaults } from "./PortfolioPolicy";

// The strategies a backtest runs: the drafted rule with every value of its
// windows, a drafted fixed-weight portfolio, or the one a fixed package holds.
export function useBacktestCandidates(
  snapshot: TerminalContext["snapshot"],
  definition: ResearchStrategyPackage["definition"] | null,
) {
  const datasets = snapshot?.datasets ?? [];
  const [strategy, setStrategy] = useWorkspaceDraft("backtest-strategy", strategyDefaults);
  const [portfolioDraft, setPortfolioDraft] = useWorkspaceDraft(
    "backtest-portfolio-policy",
    portfolioPolicyDefaults,
  );
  const eligiblePortfolio = portfolioEligible(datasets, Boolean(snapshot?.dataset_series?.length));
  const lockedPortfolio = definition?.portfolio?.strategy;
  const portfolioMode = Boolean(lockedPortfolio) || (!definition && portfolioDraft.enabled);
  const fixedPortfolio = definition
    ? (lockedPortfolio ?? null)
    : portfolioPolicy(portfolioDraft, datasets, eligiblePortfolio);
  const portfolioEditorDraft = lockedPortfolio
    ? {
        enabled: true,
        weights: Object.fromEntries(
          lockedPortfolio.rule.members.map(m => [`${m.venue}/${m.symbol}`, m.weight]),
        ),
        gross: lockedPortfolio.rule.gross_weight_limit,
        net: lockedPortfolio.rule.net_weight_limit,
        rebalance: String(lockedPortfolio.rule.rebalance),
      }
    : portfolioDraft;
  const portfolioReady = Boolean(definition) || !portfolioDraft.enabled || fixedPortfolio !== null;
  const fixed = definition?.portfolio?.strategy ?? definition?.strategy;
  const shownStrategy =
    fixed && fixed.rule.kind === "moving_average" && "quantity" in fixed
      ? {
          ...strategy,
          kind: "moving_average" as const,
          quantity: fixed.quantity,
          sides: fixed.sides,
          fast: String(fixed.rule.fast),
          slow: String(fixed.rule.slow),
        }
      : strategy;
  const candidates = definition
    ? fixed
      ? [fixed]
      : []
    : portfolioMode
      ? fixedPortfolio
        ? [fixedPortfolio]
        : []
      : strategiesOf(strategy);
  return {
    strategy,
    setStrategy,
    portfolioDraft,
    setPortfolioDraft,
    eligiblePortfolio,
    lockedPortfolio,
    portfolioMode,
    fixedPortfolio,
    portfolioEditorDraft,
    portfolioReady,
    shownStrategy,
    candidates,
  };
}
