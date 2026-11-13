import { fireEvent, render, screen, within } from "@testing-library/react";
import { create } from "@bufbuild/protobuf";
import { expect, test, vi } from "vitest";
import { lab } from "../../src/bridge/lab";
import {
  FactorResultsSchema,
  SourceSchema,
  StrategiesSchema,
} from "../../src/generated/asterion/lab_pb";
import type { TerminalContext } from "../../src/host/feature-registry";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "../../src/host/workspace/drafts";
import { FactorLab } from "./FactorLab";

vi.mock("../../src/bridge/lab", () => ({ lab: vi.fn() }));
vi.mock("../../src/i18n", async original => ({
  ...(await original<typeof import("../../src/i18n")>()),
  translate: (_namespace: string, key: string) => key,
}));

test("the factor library ranks factors by their latest IC and opens one's code", async () => {
  vi.mocked(lab).mockImplementation(async operation => {
    switch (operation?.case) {
      case "listStrategies":
        return create(StrategiesSchema, {
          factors: [
            { id: "price.momentum", inputs: ["close"] },
            { id: "price.reversal", inputs: ["close"] },
            { id: "price.volatility", inputs: ["close"] },
          ],
        }) as never;
      case "listFactorResults":
        return create(FactorResultsSchema, {
          results: [
            {
              id: "b",
              evaluation: { factor: "price.reversal" },
              horizons: [{ horizon: 1, mean: 0.3 }],
            },
            {
              id: "a",
              evaluation: { factor: "price.momentum" },
              horizons: [{ horizon: 1, mean: 0.1 }],
            },
          ],
        }) as never;
      case "readSource":
        return create(SourceSchema, { content: "def momentum(close): ..." }) as never;
    }
    return new Promise(() => {}) as never;
  });
  const context = { snapshot: { research_directory: "/research" } } as unknown as TerminalContext;
  render(
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="workspace.lab-factors">
      <FactorLab {...context} />
    </WorkspaceDraftScope>,
  );
  fireEvent.click(await screen.findByRole("button", { name: "因子库" }));
  const library = await screen.findByRole("table", { name: "因子库" });
  const order = () =>
    within(library)
      .getAllByRole("row")
      .slice(1)
      .map(row => within(row).getAllByRole("cell")[0]!.textContent);
  await within(library).findByText("0.300");
  // Never evaluated goes last.
  expect(order()).toEqual(["reversal", "momentum", "volatility"]);
  fireEvent.click(within(library).getByRole("button", { name: "momentum" }));
  expect(screen.queryByRole("table", { name: "因子库" })).toBeNull();
  await screen.findByText("factors/price.py");
});
