import { create } from "@bufbuild/protobuf";
import { render, screen, within } from "@testing-library/react";
import { expect, test } from "vitest";
import { AccountSummary } from "./AccountSummary";
import { feature } from "./feature";
import { registerTerminalFeatures } from "../../src/host/feature-registry";
import { Offset, Side } from "../../src/generated/asterion/common_pb";
import { SessionSchema } from "../../src/generated/asterion/trading_pb";

registerTerminalFeatures([feature]);

const rb = { exchange: "SHFE", symbol: "rb2610" };

test("the account shows today's result and how much of each limit it uses", () => {
  const live = create(SessionSchema, {
    policy: {
      limits: {
        maxOrderLots: { text: "5" },
        maxGrossLots: { text: "10" },
        maxContractLots: { text: "4" },
        maxWorkingOrders: 2,
      },
      maxPriceDeviation: { text: "0.02" },
      maxDailyLoss: { text: "5000" },
      maxDailyCancelsPerContract: 400,
    },
    cancels: [{ instrument: rb, cancels: 380 }],
    funds: {
      dynamicEquity: { text: "996000" },
      preBalance: { text: "1000000" },
      deposit: { text: "0" },
      withdraw: { text: "0" },
      available: { text: "900000" },
      margin: { text: "49800" },
    },
    positions: [{ instrument: rb, side: Side.BUY, todayLots: { text: "2" } }],
    workingOrders: [
      {
        id: "resting",
        instrument: rb,
        offset: Offset.OPEN,
        lots: { text: "3" },
        filledLots: { text: "1" },
      },
    ],
  });
  render(<AccountSummary live={live} />);
  expect(screen.getByText("-4,000.00").className).toBe("ctp-down");
  expect(screen.getByText("5.0%")).toBeTruthy();
  const usage = within(screen.getByRole("list", { name: "风控占用" }));
  const meter = (name: string) => usage.getByRole("meter", { name }).closest("li")!;
  // Positions and opening orders count both sides, as the risk rules do.
  expect(meter("账户总持仓").textContent).toContain("4 / 10");
  expect(meter("单合约持仓").dataset.tone).toBe("danger");
  expect(meter("在途委托").textContent).toContain("1 / 2");
  expect(meter("日内亏损").dataset.tone).toBe("attention");
  expect(meter("单合约日撤单").textContent).toContain("380 / 400");
  // The broker's other figures stay folded until asked for.
  expect(screen.getByText("资金明细").closest("details")!.open).toBe(false);
});

test("positive daily P&L includes an explicit plus sign", () => {
  const live = create(SessionSchema, {
    funds: {
      dynamicEquity: { text: "1002520" },
      preBalance: { text: "1000000" },
      deposit: { text: "0" },
      withdraw: { text: "0" },
      available: { text: "950000" },
      margin: { text: "50000" },
    },
  });
  render(<AccountSummary live={live} />);
  expect(screen.getByText("+2,520.00").className).toBe("ctp-up");
});
