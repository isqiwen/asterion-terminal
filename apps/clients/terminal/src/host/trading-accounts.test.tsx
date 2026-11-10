import { create } from "@bufbuild/protobuf";
import { render, waitFor } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { tradingRequest } from "../bridge/trading";
import { HealthSchema, ResponseSchema } from "../generated/asterion/trading_pb";
import { notify } from "./notifications";
import { TradingAccountsProvider } from "./trading-accounts";

vi.mock("../bridge/trading", () => ({ tradingRequest: vi.fn() }));
vi.mock("./notifications", () => ({ notify: vi.fn() }));

test("a Trading service built from another protocol is reported and stays usable", async () => {
  vi.mocked(tradingRequest).mockResolvedValue(
    create(ResponseSchema, {
      result: { case: "getHealth", value: create(HealthSchema, { protocol: "theirs" }) },
    }),
  );
  render(
    <TradingAccountsProvider ids={["sim"]} protocol="ours">
      <span />
    </TradingAccountsProvider>,
  );
  await waitFor(() => expect(notify).toHaveBeenCalledOnce());
  expect(vi.mocked(notify).mock.calls[0]![0]).toMatchObject({ code: "trading_protocol" });
});
