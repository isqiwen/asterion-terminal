import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "../../src/host/workspace/drafts";
import { readySnapshot } from "../../src/testing/core";
import { registerTerminalFeatures } from "../../src/host/feature-registry";
import type { ResearchStrategyPackage, ResearchStrategyDelivery } from "../../src/bridge/client";
import type { CommandParams } from "../../src/bridge/commands";
import type { TerminalContext } from "../contract";
import { StrategyPackages } from "./StrategyPackages";
import { feature } from "./feature";
registerTerminalFeatures([feature]);
test.each(["single", "portfolio"] as const)(
  "%s: an uncertain durable draft retries the same target and never submits without fixed assumptions and explicit consent",
  async kind => {
    const snapshot = readySnapshot();
    if (!snapshot.research_service) throw new Error("missing fixture service");
    snapshot.research_service.owner_node_id = "fixture-node";
    const object = {
      node: "fixture-node",
      service: snapshot.research_service.service,
      domain: "research" as const,
      kind: "strategy_package",
      id: "candidate",
      version: "a".repeat(64),
    };
    const candidate: ResearchStrategyPackage = {
      object,
      name: "Synthetic candidate",
      purpose: "Research only",
      limitations: "No predictive validation or live permission",
      definition: {
        strategy: { quantity: "1", sides: "both", rule: { kind: "momentum", lookback: 2 } },
        contract: {
          venue: "SHFE",
          symbol: "rb2610",
          product: "rb",
          delivery_month: "2026-10",
          currency: "CNY",
          price_increment: "1",
          quantity_increment: "1",
          multiplier: "10",
        },
        interval_minutes: 1,
        program_artifact: "b".repeat(64),
      },
      reference_result: {
        ...object,
        kind: "backtest_result",
        id: "reference",
        version: "d".repeat(64),
      },
      evidence_relation: "reference_only",
      source_input_sha256: "e".repeat(64),
      source_attempt: 1,
      recorded_at_ms: "1",
      retirement_reason: "",
    };
    if (kind === "portfolio") {
      const contract = candidate.definition.contract;
      if (!contract) throw new Error("missing ordinary contract fixture");
      candidate.definition = {
        interval_minutes: 0,
        program_artifact: candidate.definition.program_artifact,
        portfolio: {
          contracts: [contract, { ...contract, symbol: "hc2610", product: "hc" }],
          strategy: {
            sides: "both",
            rule: {
              kind: "fixed_weights",
              rebalance: 2,
              gross_weight_limit: "0.8",
              net_weight_limit: "0.3",
              members: [
                { venue: "SHFE", symbol: "rb2610", weight: "0.4" },
                { venue: "SHFE", symbol: "hc2610", weight: "-0.2" },
              ],
            },
          },
        },
      };
    }
    const original = JSON.stringify(candidate);
    let saved: ResearchStrategyDelivery | undefined;
    const trade = vi.fn(async (...[method, params]: Parameters<TerminalContext["trade"]>) => {
      if (method !== "research.strategy.delivery.create")
        throw new Error("unexpected execution or mutation");
      const body = params as CommandParams["research.strategy.delivery.create"];
      saved = {
        object: {
          ...object,
          kind: "strategy_delivery",
          id: body.delivery.id,
          version: "f".repeat(64),
        },
        package: body.delivery.package,
        reference_result: candidate.reference_result,
        target_task_id: body.delivery.target_task_id,
        source_input_sha256: candidate.source_input_sha256,
        recorded_at_ms: "2",
      };
      if (trade.mock.calls.length === 1) throw new Error("reply lost after durable draft");
    });
    const query: TerminalContext["query"] = async (...[method]) => ({
      ...snapshot,
      research_strategy: {
        packages: method === "research.strategy.list" ? [candidate] : [],
        deliveries: saved ? [saved] : [],
        next_after: "",
        ...(method === "research.strategy.delivery.read"
          ? { delivery: saved, target_accepted: false }
          : {}),
      },
    });
    const context: TerminalContext = {
      snapshot,
      query,
      trade,
      busy: false,
      error: "",
      navigate: vi.fn(),
      openSettings: vi.fn(),
    };
    render(
      <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="strategies">
        <StrategyPackages {...context} />
      </WorkspaceDraftScope>,
    );
    fireEvent.click(await screen.findByRole("button", { name: "Synthetic candidate · candidate" }));
    await waitFor(() => expect(screen.getByRole("region", { name: "固定候选定义" })).toBeTruthy());
    if (kind === "portfolio") {
      const content = screen.getByRole("region", { name: "固定候选定义" }).textContent;
      expect(content).toContain("SHFE/rb2610 · SHFE/hc2610");
      expect(content).toContain("0.4");
      expect(content).toContain("-0.2");
      expect(content).toContain("0.8");
      expect(content).toContain("0.3");
    }
    fireEvent.click(screen.getByRole("button", { name: "保存交付草稿，不运行" }));
    await waitFor(() => expect(screen.getByRole("alert")).toBeTruthy());
    fireEvent.click(screen.getByRole("button", { name: "保存交付草稿，不运行" }));
    await screen.findByText("草稿已保存，尚无正式接纳");
    expect(trade).toHaveBeenCalledTimes(2);
    expect(trade.mock.calls[1]).toEqual(trade.mock.calls[0]);
    expect(
      (screen.getByRole("button", { name: "显式提交固定回测" }) as HTMLButtonElement).disabled,
    ).toBe(true);
    expect(
      (
        screen.getByRole("checkbox", {
          name: "确认原固定回测假设，显式提交研究任务",
        }) as HTMLInputElement
      ).disabled,
    ).toBe(true);
    expect(screen.getByText("原回测仅作参考，不是此包的已使用记录。")).toBeTruthy();
    expect(JSON.stringify(candidate)).toBe(original);
  },
);
