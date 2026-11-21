import { act, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { beforeEach, expect, test, vi } from "vitest";
import { create } from "@bufbuild/protobuf";
import { Period } from "../../src/generated/asterion/common_pb";
import {
  InstanceSchema,
  PackageSchema,
  ResearchRunSchema,
} from "../../src/generated/asterion/strategy_pb";
import {
  AccountSchema,
  BrokerState,
  Environment,
  InstanceState,
  StrategiesSchema as HostStrategiesSchema,
  type Strategies,
} from "../../src/generated/asterion/trading_pb";
import { StrategiesSchema } from "../../src/generated/asterion/lab_pb";
import { registerTerminalFeatures } from "../../src/host/feature-registry";
import { lab } from "../../src/bridge/lab";
import { strategyRequest } from "../../src/bridge/strategy";
import { StrategyPackages } from "./StrategyPackages";
import { StrategyMonitor } from "./StrategyMonitor";
import { StrategyInstance } from "./StrategyInstance";
import { useStrategyHost } from "../../src/host/trading-accounts";
import type { Account } from "../../src/generated/asterion/trading_pb";
import { NotificationCenter } from "../../src/host/notifications";
import { feature } from "./feature";

vi.mock("../../src/bridge/lab", () => ({ lab: vi.fn() }));
vi.mock("../../src/bridge/strategy", () => ({ strategyRequest: vi.fn() }));
registerTerminalFeatures([feature]);

type Props = {
  account: string;
  repository: string;
  record?: Account;
  connected?: boolean;
  initialRun?: string;
  // Lets the start form ask Lab what the strategy declares.
  listParameters?: boolean;
};
// The strategy view's parts, sharing one host as the view does.
function Packages(props: Props) {
  return <AccountPackages key={props.account} {...props} />;
}
function AccountPackages({ account, ...rest }: Props) {
  const host = useStrategyHost(account);
  return (
    <>
      {host.state && (
        <StrategyMonitor
          instance={host.state.instance}
          observedAt={host.observedAt}
          fresh={host.fresh}
          stopping={host.stopping}
          busy={host.busy}
          stop={id => host.mutate({ case: "stopStrategy", value: { id } })}
          pause={(id, paused) => host.mutate({ case: "pauseStrategy", value: { id, paused } })}
          flatten={id => host.mutate({ case: "flattenStrategy", value: { id } })}
        />
      )}
      {host.state && (
        <StrategyInstance
          repository={rest.listParameters ? rest.repository : undefined}
          state={host.state}
          account={rest.record}
          available={host.fresh && !!rest.connected}
          busy={host.busy || host.stopping}
          start={configuration => host.mutate({ case: "startStrategy", value: configuration })}
        />
      )}
      <StrategyPackages host={host} repository={rest.repository} initialRun={rest.initialRun} />
    </>
  );
}
beforeEach(() => {
  vi.resetAllMocks();
  vi.mocked(strategyRequest).mockResolvedValue(installed());
});

const code = create(PackageSchema, {
  manifest: { name: "example", version: "one", entry: "rule:Example", files: ["rule.py"] },
  files: [{ path: "rule.py", content: new Uint8Array([35, 10]) }],
});
// Lab lists one strategy and builds `built` for it.
const labAnswers = (built: () => Promise<typeof code>) =>
  vi
    .mocked(lab)
    .mockImplementation((async (operation: { case: string }) =>
      operation.case === "listStrategies"
        ? create(StrategiesSchema, { strategies: [{ id: "rule.Example" }] })
        : built()) as never);
const choose = async () => {
  await screen.findByRole("option", { name: "rule.Example" });
  fireEvent.change(screen.getByRole("combobox", { name: "策略" }), {
    target: { value: "rule.Example" },
  });
};
const installed = () => create(HostStrategiesSchema, { packages: [code.manifest!] });
const rb = { exchange: "SHFE", symbol: "rb2610" };
const configuration = create(InstanceSchema, {
  id: "run-a",
  packageName: "example",
  packageVersion: "one",
  instruments: [rb],
  period: Period.MINUTES_1,
});
const runtime = (state: InstanceState) =>
  create(HostStrategiesSchema, {
    packages: [code.manifest!],
    instance: { instance: configuration, state, completedSteps: 2n },
  });
const writes = () =>
  vi
    .mocked(strategyRequest)
    .mock.calls.filter(([, operation]) => operation.case !== "getStrategies");
const click = async (name: string) =>
  act(async () => fireEvent.click(screen.getByRole("button", { name })));
// The account the strategy would trade, as Trading reports it.
const record = create(AccountSchema, {
  id: "record",
  environment: Environment.SIMULATION,
  broker: { brokerId: "9999", userId: "000001", front: "tcp://test", appId: "test" },
  session: {
    brokerState: BrokerState.READY,
    positionsReconciled: true,
    policyRevision: "policy-a",
    policy: { contracts: [{ instrument: rb, product: "rb", deliveryMonth: "2026-10" }] },
  },
});

test("deploying builds and installs on the selected account, without starting", async () => {
  labAnswers(async () => code);
  vi.mocked(strategyRequest).mockResolvedValue(
    create(HostStrategiesSchema, { packages: [code.manifest!] }),
  );
  render(<Packages account="account-a" repository="/strategies" />);
  await choose();
  await click("部署");
  expect(lab).toHaveBeenCalledWith({
    case: "buildPackage",
    value: { repository: "/strategies", strategy: "rule.Example" },
  });
  expect(writes()).toEqual([
    [
      "account-a",
      {
        case: "publishStrategy",
        value: code,
      },
    ],
  ]);
});

test("a repository change discards a late build; an uncertain install is not retried", async () => {
  let finish: (value: typeof code) => void = () => {};
  labAnswers(
    () =>
      new Promise(resolve => {
        finish = resolve;
      }),
  );
  const view = render(
    <>
      <Packages account="account-a" repository="/old" />
      <NotificationCenter />
    </>,
  );
  await choose();
  fireEvent.click(screen.getByRole("button", { name: "部署" }));
  view.rerender(
    <>
      <Packages account="account-a" repository="/new" />
      <NotificationCenter />
    </>,
  );
  await act(async () => finish(code));
  expect(writes()).toEqual([]);
  labAnswers(async () => code);
  vi.mocked(strategyRequest).mockRejectedValue(new Error("connection lost"));
  await click("部署");
  expect(writes()).toHaveLength(1);
  expect(screen.getByText(/connection lost/)).toBeDefined();
});

test("deploying a backtest installs its frozen package and loads its settings before contract confirmation", async () => {
  const frozen = create(PackageSchema, {
    ...code,
    manifest: {
      ...code.manifest!,
      research: create(ResearchRunSchema, {
        runId: "chosen",
        datasetVersion: "fixed-data",
        symbols: ["SHFE/rb"],
        period: Period.MINUTES_5,
        params: { window: { value: { case: "integer", value: 9007199254740993n } } },
        weightLimits: { gross: { text: "0.8" }, net: { text: "0.4" } },
      }),
    },
  });
  labAnswers(async () => frozen);
  vi.mocked(strategyRequest).mockImplementation(async (_account, operation) =>
    create(HostStrategiesSchema, {
      packages: [operation.case === "publishStrategy" ? frozen.manifest! : code.manifest!],
    }),
  );
  render(
    <Packages
      account="account-a"
      repository="/edited"
      initialRun="chosen"
      record={record}
      connected
    />,
  );
  await waitFor(() =>
    expect((screen.getByRole("button", { name: "部署" }) as HTMLButtonElement).disabled).toBe(
      false,
    ),
  );
  await click("部署");
  expect(lab).toHaveBeenCalledWith({ case: "buildPackage", value: { runId: "chosen" } });
  expect(vi.mocked(lab).mock.calls.some(([operation]) => operation.case === "listStrategies")).toBe(
    false,
  );
  expect((screen.getByRole("button", { name: "已部署" }) as HTMLButtonElement).disabled).toBe(true);
  fireEvent.change(screen.getByLabelText("已部署版本"), { target: { value: "example/one" } });
  expect((screen.getByLabelText("K 线周期") as HTMLSelectElement).value).toBe(
    String(Period.MINUTES_5),
  );
  expect((screen.getByLabelText("window") as HTMLInputElement).value).toBe("9007199254740993");
  expect((screen.getByLabelText("总绝对权重预算") as HTMLInputElement).value).toBe("0.8");
  expect((screen.getByLabelText("SHFE.rb2610") as HTMLInputElement).checked).toBe(false);
  fireEvent.change(screen.getByLabelText("实例 ID"), { target: { value: "from-backtest" } });
  fireEvent.click(screen.getByLabelText("SHFE.rb2610"));
  await click("核对启动配置");
  expect(writes()).toHaveLength(1);
  await click("确认启动");
  const [, operation] = writes()[1];
  expect(operation.case).toBe("startStrategy");
  if (operation.case !== "startStrategy") throw new Error("missing start");
  expect(operation.value).toMatchObject({
    period: Period.MINUTES_5,
    params: frozen.manifest!.research!.params,
    weightLimits: frozen.manifest!.research!.weightLimits,
    instruments: [rb],
  });
});

test("review sends explicit configuration and exact values only on confirmation, without authorizing", async () => {
  vi.mocked(strategyRequest).mockImplementation(async (_account, operation) =>
    operation.case === "startStrategy"
      ? create(HostStrategiesSchema, {
          packages: [code.manifest!],
          instance: { instance: operation.value, state: InstanceState.WARMING },
        })
      : installed(),
  );
  vi.mocked(lab).mockResolvedValue(
    create(StrategiesSchema, {
      strategies: [
        {
          id: "example",
          parameters: [
            { name: "window", default: { value: { case: "integer", value: 5n } } },
            { name: "lots", default: { value: { case: "integer", value: 1n } } },
          ],
        },
      ],
    }) as never,
  );
  render(
    <Packages
      account="account-a"
      repository="/strategies"
      listParameters
      record={record}
      connected
    />,
  );
  await waitFor(() => expect(screen.getByLabelText("实例 ID")).toBeDefined());
  fireEvent.change(screen.getByLabelText("实例 ID"), { target: { value: "new-run" } });
  fireEvent.change(screen.getByLabelText("已部署版本"), { target: { value: "example/one" } });
  fireEvent.click(screen.getByLabelText("SHFE.rb2610"));
  fireEvent.change(screen.getByLabelText("K 线周期"), {
    target: { value: String(Period.MINUTES_5) },
  });
  // The strategy's own parameters are listed; one left blank keeps its default.
  const window = (await screen.findByLabelText("window")) as HTMLInputElement;
  expect(window.placeholder).toBe("5");
  expect((screen.getByLabelText("lots") as HTMLInputElement).placeholder).toBe("1");
  fireEvent.change(window, { target: { value: "9007199254740993" } });
  fireEvent.click(screen.getByLabelText("设置策略权重预算"));
  fireEvent.change(screen.getByLabelText("总绝对权重预算"), { target: { value: "1.00000001" } });
  fireEvent.change(screen.getByLabelText("净权重绝对值预算"), { target: { value: "0" } });
  await click("核对启动配置");
  expect(writes()).toEqual([]);
  await click("确认启动");
  expect(writes()).toHaveLength(1);
  const [account, operation] = writes()[0];
  expect(account).toBe("account-a");
  expect(operation.case).toBe("startStrategy");
  if (operation.case !== "startStrategy") throw new Error("missing start");
  expect(operation.value).toMatchObject({
    id: "new-run",
    instruments: [rb],
    period: Period.MINUTES_5,
    weightLimits: { gross: { text: "1.00000001" }, net: { text: "0" } },
    params: { window: { value: { case: "integer", value: 9007199254740993n } } },
  });
  expect(Object.keys(operation.value.params!)).toEqual(["window"]);
  expect(screen.getByText("正在现场预热")).toBeDefined();
});

test("an ended instance resumes from its record under a new id after review", async () => {
  const ended = create(InstanceSchema, {
    id: "run-a",
    packageName: "example",
    packageVersion: "one",
    instruments: [rb],
    period: Period.MINUTES_1,
    params: {
      lots: { value: { case: "integer", value: 2n } },
      fast: { value: { case: "integer", value: 7n } },
    },
  });
  vi.mocked(strategyRequest).mockResolvedValue(
    create(HostStrategiesSchema, {
      packages: [code.manifest!],
      instance: { instance: ended, state: InstanceState.INTERRUPTED, completedSteps: 2n },
    }),
  );
  render(<Packages account="account-a" repository="" record={record} connected />);
  await waitFor(() => expect(screen.getByLabelText("实例 ID")).toBeDefined());
  fireEvent.change(screen.getByLabelText("实例 ID"), { target: { value: "run-b" } });
  expect((screen.getByLabelText("lots") as HTMLInputElement).placeholder).toBe("2");
  fireEvent.change(screen.getByLabelText("lots"), { target: { value: "4" } });
  await click("从实例 run-a 的记录恢复");
  expect(screen.getByText("实例 run-a 的记录")).toBeDefined();
  expect(writes()).toEqual([]);
  await click("确认启动");
  const [, operation] = writes()[0];
  if (operation.case !== "startStrategy") throw new Error("missing start");
  expect(operation.value).toMatchObject({
    ...configuration,
    id: "run-b",
    resumeFrom: "run-a",
    params: {
      lots: { value: { case: "integer", value: 4n } },
      fast: { value: { case: "integer", value: 7n } },
    },
  });
  expect(configuration.resumeFrom).toBe("");
});

test("pause and flatten act on the inspected instance", async () => {
  const ready = create(HostStrategiesSchema, {
    packages: [code.manifest!],
    instance: {
      instance: configuration,
      state: InstanceState.READY,
      events: [{ timeMs: 1n, text: "sent SHFE/rb2610 buy open 1 @ 3501" }],
    },
  });
  vi.mocked(strategyRequest).mockResolvedValue(ready);
  render(<Packages account="account-a" repository="" record={record} connected />);
  await waitFor(() => expect(screen.getByText(/sent SHFE\/rb2610/)).toBeDefined());
  await click("暂停新增");
  expect(writes()[0][1]).toEqual({ case: "pauseStrategy", value: { id: "run-a", paused: true } });
  await click("平仓");
  expect(writes()).toHaveLength(1);
  await click("确认平仓");
  expect(writes()[1][1]).toEqual({ case: "flattenStrategy", value: { id: "run-a" } });
});

test("stop remains available during an inspection and a late reply cannot resurrect the stopped instance", async () => {
  vi.mocked(strategyRequest).mockResolvedValue(runtime(InstanceState.WARMING));
  render(<Packages account="account-a" repository="" />);
  await waitFor(() => expect(screen.getByText("正在现场预热")).toBeDefined());
  let reply!: (value: Strategies) => void;
  vi.mocked(strategyRequest).mockReturnValueOnce(
    new Promise(resolve => {
      reply = resolve;
    }),
  );
  fireEvent.click(screen.getByRole("button", { name: "刷新版本" }));
  await click("停止策略并撤单");
  expect(writes()).toEqual([]);
  vi.mocked(strategyRequest).mockResolvedValueOnce(runtime(InstanceState.STOPPED));
  await click("确认停止并撤单");
  expect(writes()).toEqual([["account-a", { case: "stopStrategy", value: { id: "run-a" } }]]);
  await act(async () => reply(runtime(InstanceState.WARMING)));
  expect(screen.getByText("已停止")).toBeDefined();
  expect(screen.queryByText("正在现场预热")).toBeNull();
});

test("a changed account policy requires reviewing the configuration again before starting", async () => {
  const props = { account: "account-a", repository: "", connected: true };
  const view = render(<Packages {...props} record={record} />);
  await waitFor(() => expect(screen.getByLabelText("实例 ID")).toBeDefined());
  fireEvent.change(screen.getByLabelText("实例 ID"), { target: { value: "new-run" } });
  fireEvent.change(screen.getByLabelText("已部署版本"), { target: { value: "example/one" } });
  fireEvent.click(screen.getByLabelText("SHFE.rb2610"));
  await click("核对启动配置");
  view.rerender(
    <Packages
      {...props}
      record={{ ...record, session: { ...record.session!, policyRevision: "policy-b" } }}
    />,
  );
  expect(screen.getByRole("alert").textContent).toContain("风控设置已变化");
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
  vi.mocked(strategyRequest).mockResolvedValue(runtime(InstanceState.READY));
  const view = render(<Packages account="account-a" repository="" />);
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
  let reply!: (value: Strategies) => void;
  vi.mocked(strategyRequest).mockReturnValueOnce(
    new Promise(resolve => {
      reply = resolve;
    }),
  );
  const view = render(<Packages account="account-a" repository="" />);
  view.rerender(<Packages account="account-b" repository="" />);
  await act(async () => reply(runtime(InstanceState.READY)));
  expect(screen.queryByText("策略已就绪")).toBeNull();
  expect(strategyRequest).toHaveBeenLastCalledWith("account-b", {
    case: "getStrategies",
    value: {},
  });
});

test("dominant series status renders active contract targets and positions", async () => {
  vi.mocked(strategyRequest).mockResolvedValue(
    create(HostStrategiesSchema, {
      packages: [code.manifest!],
      instance: {
        instance: {
          id: "dom-run",
          packageName: "example",
          packageVersion: "one",
          series: [
            {
              exchange: "SHFE",
              product: "rb",
              contracts: [
                { instrument: { exchange: "SHFE", symbol: "rb2610" }, deliveryMonth: "2026-10" },
                { instrument: { exchange: "SHFE", symbol: "rb2701" }, deliveryMonth: "2027-01" },
              ],
            },
          ],
          period: Period.DAY,
        },
        state: InstanceState.READY,
        completedSteps: 4n,
        lastDecision: {
          sequence: 4n,
          ready: true,
          targets: [
            {
              instrument: { exchange: "SHFE", symbol: "rb2701" },
              size: { case: "lots", value: 2n },
            },
          ],
        },
      },
    }),
  );
  render(<Packages account="account-a" repository="" />);
  await waitFor(() => expect(screen.getByText("SHFE.rb · SHFE.rb2701")).toBeDefined());
  expect(screen.getByText("SHFE.rb")).toBeDefined();
});

test("preparing historical warmup disables the start form until Lab responds", async () => {
  let finishWarmup!: (value: unknown) => void;
  vi.mocked(lab).mockImplementation(
    () =>
      new Promise(resolve => {
        finishWarmup = resolve;
      }) as never,
  );
  render(<Packages account="account-a" repository="" record={record} connected />);
  await waitFor(() => expect(screen.getByLabelText("实例 ID")).toBeDefined());
  fireEvent.change(screen.getByLabelText("实例 ID"), { target: { value: "warm-run" } });
  fireEvent.change(screen.getByLabelText("已部署版本"), { target: { value: "example/one" } });
  fireEvent.click(screen.getByLabelText("SHFE.rb2610"));
  fireEvent.change(screen.getByLabelText("历史预热 K 线数"), { target: { value: "20" } });
  fireEvent.click(screen.getByRole("button", { name: "核对启动配置" }));
  const preparing = await screen.findByRole("button", { name: "正在准备预热数据…" });
  expect((preparing as HTMLButtonElement).disabled).toBe(true);
  await act(async () => finishWarmup({ datasetVersion: "v1", sha256: "a".repeat(64), bars: [] }));
  expect(await screen.findByRole("button", { name: "确认启动" })).toBeTruthy();
});
