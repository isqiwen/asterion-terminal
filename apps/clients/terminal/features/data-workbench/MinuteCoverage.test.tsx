import { fireEvent, render, screen, within } from "@testing-library/react";
import { create } from "@bufbuild/protobuf";
import { expect, test } from "vitest";
import { DatasetCoverageSchema } from "../../src/generated/asterion/market_pb";
import { registerTerminalFeatures } from "../../src/host/feature-registry";
import { MinuteCoverage } from "./MinuteCoverage";
import { feature } from "./feature";

registerTerminalFeatures([feature]);

const { contracts } = create(DatasetCoverageSchema, {
  contracts: [
    { contract: "SHFE/cu/2026-12", months: [{ month: "2026-10", days: 4 }] },
    {
      contract: "SHFE/rb/2027-01",
      months: [
        { month: "2026-09", days: 21 },
        { month: "2026-10", days: 18, recordedDays: 2 },
      ],
    },
  ],
});

test("a chosen product's contracts show their minute days, and recorded months are marked", () => {
  render(<MinuteCoverage contracts={contracts} />);
  const table = screen.getByRole("table", { name: "分钟线覆盖" });
  expect(within(table).getByRole("rowheader", { name: "SHFE/cu/2026-12" })).toBeTruthy();
  expect(within(table).queryByRole("rowheader", { name: "SHFE/rb/2027-01" })).toBeNull();
  fireEvent.change(screen.getByLabelText("品种"), { target: { value: "SHFE/rb" } });
  const row = within(table).getByRole("row", { name: /SHFE\/rb\/2027-01/ });
  const [september, october] = within(row).getAllByRole("cell");
  expect(september!.textContent).toBe("21");
  expect(september!.dataset.recorded).toBeUndefined();
  expect(october!.textContent).toBe("18");
  expect(october!.dataset.recorded).toBe("true");
  expect(october!.title).toBe("18 天，其中自录 2 天");
});
