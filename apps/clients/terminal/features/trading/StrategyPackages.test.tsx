import { act, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { beforeEach, expect, test, vi } from "vitest";
import { create } from "@bufbuild/protobuf";
import {
  InstanceSchema,
  PackageSchema,
  StateSchema,
  type State,
} from "../../src/generated/asterion/strategy_pb";
import { registerTerminalFeatures } from "../../src/host/feature-registry";
import { lab } from "../../src/bridge/lab";
import { strategyRequest } from "../../src/bridge/strategy";
import { StrategyPackages } from "./StrategyPackages";
import { feature } from "./feature";

vi.mock("../../src/bridge/lab", () => ({ lab: vi.fn() }));
vi.mock("../../src/bridge/strategy", () => ({ strategyRequest: vi.fn() }));
registerTerminalFeatures([feature]);
beforeEach(() => {
  vi.resetAllMocks();
  vi.mocked(strategyRequest).mockResolvedValue(installed());
});

const code = create(PackageSchema, {
  manifest: { name: "example", version: "one", entry: "rule:Example", files: ["rule.py"] },
  files: [{ path: "rule.py", content: new Uint8Array([35, 10]) }],
});
const installed = () => create(StateSchema, { packages: [code.manifest!] });
const configuration = create(InstanceSchema, {
  id: "run-a",
  package: { name: "example", version: "one" },
  contracts: ["SHFE/rb2610"],
  period: "1m",
});
const runtime = (state: string) =>
  create(StateSchema, {
    packages: [code.manifest!],
    instance: { configuration, state, completedSteps: 2n },
  });
const writes = () =>
  vi.mocked(strategyRequest).mock.calls.filter(([, operation]) => operation?.case !== "inspect");
const click = async (name: string) =>
  act(async () => fireEvent.click(screen.getByRole("button", { name })));
const live = {
  environment: "simulation",
  broker: { broker_id: "9999", user_id: "000001", front: "tcp://test", app_id: "test" },
  authorization: null,
  phase: "ready",
  positions_reconciled: true,
  storage_state: "ready",
  policy_revision: "policy-a",
  contracts: [
    {
      venue: "SHFE",
      symbol: "rb2610",
      product: "rb",
      delivery_month: "2026-10",
      currency: "CNY",
      price_increment: "1",
      quantity_increment: "1",
      multiplier: "10",
    },
  ],
} as const;

test("the prepared files are reviewed and published only to the selected account, without starting", async () => {
  vi.mocked(lab).mockResolvedValue(code);
  vi.mocked(strategyRequest).mockResolvedValue(create(StateSchema, { packages: [code.manifest!] }));
  render(<StrategyPackages account="account-a" repository="/strategies" />);
  await act(async () => fireEvent.click(screen.getByText("准备策略包")));
  expect(lab).toHaveBeenCalledWith(
    { case: "strategyPackage", value: { repository: "/strategies", manifest: "strategy.json" } },
    "strategyPackage",
  );
  expect(screen.getByText("rule.py")).toBeDefined();
  expect(writes()).toEqual([]);
  await act(async () => fireEvent.click(screen.getByText("发布到此交易主机")));
  expect(writes()).toEqual([
    [
      "account-a",
      {
        case: "publish",
        value: code,
      },
    ],
  ]);
  expect(screen.queryByText("发布到此交易主机")).toBeNull();
});

test("a repository change discards a late preparation; publishing an uncertain outcome is not retried", async () => {
  let finish: (value: typeof code) => void = () => {};
  vi.mocked(lab).mockReturnValue(
    new Promise(resolve => {
      finish = resolve;
    }),
  );
  const view = render(<StrategyPackages account="account-a" repository="/old" />);
  fireEvent.click(screen.getByText("准备策略包"));
  view.rerender(<StrategyPackages account="account-a" repository="/new" />);
  await act(async () => finish(code));
  expect(screen.queryByText("发布到此交易主机")).toBeNull();
  vi.mocked(lab).mockResolvedValue(code);
  await act(async () => fireEvent.click(screen.getByText("准备策略包")));
  vi.mocked(strategyRequest).mockRejectedValue(new Error("connection lost"));
  await act(async () => fireEvent.click(screen.getByText("发布到此交易主机")));
  expect(writes()).toHaveLength(1);
  expect(screen.getByRole("alert").textContent).toContain("connection lost");
});

test("review sends explicit configuration and exact values only on confirmation, without authorizing", async () => {
  vi.mocked(strategyRequest).mockImplementation(async (_account, operation) =>
    operation?.case === "start"
      ? create(StateSchema, {
          packages: [code.manifest!],
          instance: { configuration: operation.value, state: "warming" },
        })
      : installed(),
  );
  render(
    <StrategyPackages
      account="account-a"
      repository=""
      live={{ ...live, contracts: [...live.contracts] }}
      connected
    />,
  );
  await waitFor(() => expect(screen.getByLabelText("实例 ID")).toBeDefined());
  fireEvent.change(screen.getByLabelText("实例 ID"), { target: { value: "new-run" } });
  fireEvent.change(screen.getByLabelText("已发布版本"), { target: { value: "example/one" } });
  fireEvent.click(screen.getByLabelText("SHFE/rb2610"));
  fireEvent.change(screen.getByLabelText("K 线周期"), { target: { value: "5m" } });
  await click("添加参数");
  fireEvent.change(screen.getByLabelText("参数名 1"), { target: { value: "window" } });
  fireEvent.change(screen.getByLabelText("参数值 1"), { target: { value: "9007199254740993" } });
  fireEvent.click(screen.getByLabelText("策略输出权重，设置权重预算"));
  fireEvent.change(screen.getByLabelText("总绝对权重预算"), { target: { value: "1.00000001" } });
  fireEvent.change(screen.getByLabelText("净权重绝对值预算"), { target: { value: "0" } });
  await click("核对启动配置");
  expect(writes()).toEqual([]);
  await click("确认启动");
  expect(writes()).toHaveLength(1);
  const [account, operation] = writes()[0];
  expect(account).toBe("account-a");
  expect(operation?.case).toBe("start");
  if (operation?.case !== "start") throw new Error("missing start");
  expect(operation.value).toMatchObject({
    id: "new-run",
    contracts: ["SHFE/rb2610"],
    period: "5m",
    weightBudget: { grossUnits: 100000001n, netUnits: 0n },
    params: { window: { value: { case: "integer", value: 9007199254740993n } } },
  });
  expect(screen.getByText("正在现场预热")).toBeDefined();
});

test("an ended instance resumes from its record under a new id after review", async () => {
  vi.mocked(strategyRequest).mockResolvedValue(runtime("interrupted"));
  render(
    <StrategyPackages
      account="account-a"
      repository=""
      live={{ ...live, contracts: [...live.contracts] }}
      connected
    />,
  );
  await waitFor(() => expect(screen.getByLabelText("实例 ID")).toBeDefined());
  fireEvent.change(screen.getByLabelText("实例 ID"), { target: { value: "run-b" } });
  await click("从实例 run-a 的记录恢复");
  expect(screen.getByText("实例 run-a 的记录")).toBeDefined();
  expect(writes()).toEqual([]);
  await click("确认启动");
  const [, operation] = writes()[0];
  if (operation?.case !== "start") throw new Error("missing start");
  expect(operation.value).toMatchObject({ ...configuration, id: "run-b", resumeFrom: "run-a" });
  expect(configuration.resumeFrom).toBe("");
});

test("pause and flatten act on the inspected instance; a failure raises a desktop notification", async () => {
  const shown = vi.fn();
  vi.stubGlobal(
    "Notification",
    class {
      constructor(title: string, options: { body: string }) {
        shown(title, options.body);
      }
    },
  );
  const ready = create(StateSchema, {
    packages: [code.manifest!],
    instance: {
      configuration,
      state: "ready",
      events: [{ timeMs: 1n, text: "sent SHFE/rb2610 buy open 1 @ 3501" }],
    },
  });
  vi.mocked(strategyRequest).mockImplementation(async (_account, operation) =>
    operation?.case === "flatten"
      ? create(StateSchema, {
          packages: [code.manifest!],
          instance: { configuration, state: "failed", error: "broker rejected a strategy order" },
        })
      : ready,
  );
  render(
    <StrategyPackages
      account="account-a"
      repository=""
      live={{ ...live, contracts: [...live.contracts] }}
      connected
    />,
  );
  await waitFor(() => expect(screen.getByText(/sent SHFE\/rb2610/)).toBeDefined());
  await click("暂停新增");
  expect(writes()[0][1]).toEqual({ case: "pause", value: { instanceId: "run-a", paused: true } });
  await click("平仓");
  expect(writes()).toHaveLength(1);
  await click("确认平仓");
  expect(writes()[1][1]).toEqual({ case: "flatten", value: { instanceId: "run-a" } });
  expect(shown).toHaveBeenCalledWith("策略实例 run-a", "broker rejected a strategy order");
  vi.unstubAllGlobals();
});

test("stop remains available during an inspection and a late reply cannot resurrect the stopped instance", async () => {
  vi.mocked(strategyRequest).mockResolvedValue(runtime("warming"));
  render(<StrategyPackages account="account-a" repository="" />);
  await waitFor(() => expect(screen.getByText("正在现场预热")).toBeDefined());
  let reply!: (value: State) => void;
  vi.mocked(strategyRequest).mockReturnValueOnce(
    new Promise(resolve => {
      reply = resolve;
    }),
  );
  fireEvent.click(screen.getByRole("button", { name: "读取主机策略包" }));
  await click("停止策略并撤单");
  expect(writes()).toEqual([]);
  vi.mocked(strategyRequest).mockResolvedValueOnce(runtime("stopped"));
  await click("确认停止并撤单");
  expect(writes()).toEqual([["account-a", { case: "stop", value: { instanceId: "run-a" } }]]);
  await act(async () => reply(runtime("warming")));
  expect(screen.getByText("已停止")).toBeDefined();
  expect(screen.queryByText("正在现场预热")).toBeNull();
});

test("a changed account policy requires reviewing the configuration again before starting", async () => {
  const props = { account: "account-a", repository: "", connected: true };
  const view = render(
    <StrategyPackages {...props} live={{ ...live, contracts: [...live.contracts] }} />,
  );
  await waitFor(() => expect(screen.getByLabelText("实例 ID")).toBeDefined());
  fireEvent.change(screen.getByLabelText("实例 ID"), { target: { value: "new-run" } });
  fireEvent.change(screen.getByLabelText("已发布版本"), { target: { value: "example/one" } });
  fireEvent.click(screen.getByLabelText("SHFE/rb2610"));
  await click("核对启动配置");
  view.rerender(
    <StrategyPackages
      {...props}
      live={{ ...live, contracts: [...live.contracts], policy_revision: "policy-b" }}
    />,
  );
  expect(screen.getByRole("alert").textContent).toContain("账户政策已变化");
  await click("确认启动");
  expect(writes()).toEqual([]);
  await click("返回修改");
  await click("核对启动配置");
  expect((screen.getByRole("button", { name: "确认启动" }) as HTMLButtonElement).disabled).toBe(
    false,
  );
});

test("polling is read-only; failures preserve stale state and unmount never stops trading", async () => {
  vi.useFakeTimers();
  vi.mocked(strategyRequest).mockResolvedValue(runtime("ready"));
  const view = render(<StrategyPackages account="account-a" repository="" />);
  await act(async () => {});
  vi.mocked(strategyRequest).mockRejectedValue(new Error("connection lost"));
  await act(async () => {
    await vi.advanceTimersByTimeAsync(3000);
  });
  expect(screen.getByText("策略已就绪 · 上次已知状态")).toBeDefined();
  expect(
    (screen.getByRole("button", { name: "停止策略并撤单" }) as HTMLButtonElement).disabled,
  ).toBe(false);
  expect(writes()).toEqual([]);
  view.unmount();
  const count = vi.mocked(strategyRequest).mock.calls.length;
  await vi.advanceTimersByTimeAsync(6000);
  expect(strategyRequest).toHaveBeenCalledTimes(count);
  expect(writes()).toEqual([]);
});

test("switching accounts discards a previous host's late response", async () => {
  let reply!: (value: State) => void;
  vi.mocked(strategyRequest).mockReturnValueOnce(
    new Promise(resolve => {
      reply = resolve;
    }),
  );
  const view = render(<StrategyPackages account="account-a" repository="" />);
  view.rerender(<StrategyPackages account="account-b" repository="" />);
  await act(async () => reply(runtime("ready")));
  expect(screen.queryByText("策略已就绪")).toBeNull();
  expect(strategyRequest).toHaveBeenLastCalledWith("account-b", { case: "inspect", value: {} });
});
