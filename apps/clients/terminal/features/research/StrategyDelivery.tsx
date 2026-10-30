import { useEffect, useRef, useState } from "react";
import {
  isPortfolioStrategyPackage,
  type LogicStrategyDelivery,
  type LogicStrategyPackage,
  type PortfolioStrategyPackage,
  type ReferenceStrategyDelivery,
} from "../../src/bridge/client";
import { asDisplayError, type DisplayError, type TerminalContext } from "../contract";
import { t } from "./shared";

// The fixed strategy delivery a backtest was opened for: its package and
// draft, read and checked against each other, and whether the selected data
// is the data the package was fixed on. `opened` receives the draft's target
// task once it is bound.
export function useStrategyDelivery({
  query,
  snapshot,
  requestedDelivery,
  connection,
  opened: onOpened,
  failed: onFailed,
}: Pick<TerminalContext, "query" | "snapshot"> & {
  requestedDelivery: string | undefined;
  connection: string | undefined;
  opened: (task: string) => void;
  failed: (error: DisplayError) => void;
}) {
  const ask = useRef(query);
  ask.current = query;
  const opened = useRef(onOpened);
  opened.current = onOpened;
  const failed = useRef(onFailed);
  failed.current = onFailed;
  const datasets = snapshot?.datasets ?? [];
  const [binding, setBinding] = useState<{
    package: LogicStrategyPackage | PortfolioStrategyPackage;
    delivery: LogicStrategyDelivery | ReferenceStrategyDelivery;
  } | null>(null);
  useEffect(() => {
    let active = true;
    setBinding(null);
    if (!requestedDelivery) return;
    void (async () => {
      const draft = (
        await ask.current("research.strategy.delivery.read", { id: requestedDelivery })
      ).research_strategy?.delivery;
      if (!draft) throw new Error(t("固定策略交付草稿不一致"));
      const candidate = (await ask.current("research.strategy.read", { id: draft.package.id }))
        .research_strategy?.package;
      if (
        !candidate ||
        candidate.object.id !== draft.package.id ||
        candidate.object.version !== draft.package.version
      )
        throw new Error(t("固定策略交付草稿不一致"));
      if (!active) return;
      if (
        candidate.evidence_relation === "logic_trial" &&
        draft.reference_result === null &&
        candidate.logic.result.id === draft.logic_trial.id &&
        candidate.logic.result.version === draft.logic_trial.version
      ) {
        setBinding({ package: candidate, delivery: draft });
      } else if (
        isPortfolioStrategyPackage(candidate) &&
        draft.reference_result !== null &&
        candidate.reference_result.id === draft.reference_result.id &&
        candidate.reference_result.version === draft.reference_result.version &&
        candidate.source_input_sha256 === draft.source_input_sha256
      ) {
        setBinding({ package: candidate, delivery: draft });
      } else {
        throw new Error(t("固定策略交付草稿不一致"));
      }
      opened.current(draft.target_task_id);
    })().catch(reason => {
      if (active) failed.current(asDisplayError(reason));
    });
    return () => {
      active = false;
    };
  }, [requestedDelivery, connection]);
  const boundDefinition = binding?.package.definition;
  const boundContracts =
    boundDefinition?.portfolio?.contracts ??
    (boundDefinition?.contract ? [boundDefinition.contract] : []);
  const boundDataMatches =
    !requestedDelivery ||
    Boolean(
      binding &&
      boundContracts.length > 0 &&
      datasets.length === boundContracts.length &&
      !snapshot?.dataset_series?.length &&
      boundContracts.every((contract, index) => {
        const actual = datasets[index];
        return (
          actual.venue === contract.venue &&
          actual.symbol === contract.symbol &&
          actual.interval_minutes === binding.package.definition.interval_minutes &&
          (!boundDefinition?.portfolio ||
            (
              [
                "currency",
                "multiplier",
                "price_increment",
                "quantity_increment",
                "delivery_month",
                "product",
              ] as const
            ).every(key => actual.contract[key] === contract[key]))
        );
      }),
    );
  return { binding, boundDataMatches };
}

// Where a backtest opened for a fixed delivery says what it fixes.
export function DeliveryNotice({
  binding,
  portfolio,
  boundDataMatches,
}: {
  binding: ReturnType<typeof useStrategyDelivery>["binding"];
  portfolio: boolean;
  boundDataMatches: boolean;
}) {
  return (
    <section aria-label={t(portfolio ? "固定组合交付回测" : "固定逻辑交付回测")}>
      <p>
        {binding
          ? `${binding.package.name} · ${binding.delivery.target_task_id}`
          : t("正在读取固定策略交付")}
      </p>
      <p>
        {t(
          portfolio
            ? "全部合约、权重、预算和调仓间隔来自固定包；请显式确认本次Data版本、资金、费用与风险。"
            : "规则、数量、方向和周期来自固定包；请显式确认本次数据、资金、费用与风险。",
        )}
      </p>
      {!boundDataMatches && (
        <p role="alert">
          {t(
            portfolio
              ? "所选数据需按原顺序匹配固定包的全部合约、单位和周期"
              : "所选数据需匹配固定包的单合约和周期",
          )}
        </p>
      )}
    </section>
  );
}
