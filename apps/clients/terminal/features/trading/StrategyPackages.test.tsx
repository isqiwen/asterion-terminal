import { act, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { beforeEach, expect, test, vi } from "vitest";
import { create } from "@bufbuild/protobuf";
import { Period } from "../../src/generated/asterion/common_pb";
import { InstanceSchema, PackageSchema } from "../../src/generated/asterion/strategy_pb";
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
import { useStrategyHost } from "./strategy-host";
import type { Account } from "../../src/generated/asterion/trading_pb";
import { NotificationCenter } from "../../src/host/notifications";
import { feature } from "./feature";

vi.mock("../../src/bridge/lab", () => ({ lab: vi.fn() }));
vi.mock("../../src/bridge/strategy", () => ({ strategyRequest: vi.fn() }));
registerTerminalFeatures([feature]);

type Props = { account: string; repository: string; record?: Account; connected?: boolean };
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
          positions={rest.record?.session?.positions ?? []}
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
          state={host.state}
          account={rest.record}
          available={host.fresh && !!rest.connected}
          busy={host.busy || host.stopping}
          start={configuration => host.mutate({ case: "startStrategy", value: configuration })}
        />
      )}
      <StrategyPackages host={host} repository={rest.repository} />
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

test("the prepared files are reviewed and published only to the selected account, without starting", async () => {
  labAnswers(async () => code);
  vi.mocked(strategyRequest).mockResolvedValue(
    create(HostStrategiesSchema, { packages: [code.manifest!] }),
  );
  render(<Packages account="account-a" repository="/strategies" />);
  await choose();
  await act(async () => fireEvent.click(screen.getByText("准备策略包")));
  expect(lab).toHaveBeenCalledWith({
    case: "buildPackage",
    value: { repository: "/strategies", strategy: "rule.Example" },
  });
  expect(screen.getByText("rule.py")).toBeDefined();
  expect(writes()).toEqual([]);
  await act(async () => fireEvent.click(screen.getByText("发布到此交易主机")));
  expect(writes()).toEqual([
    [
      "account-a",
      {
        case: "publishStrategy",
        value: code,
      },
    ],
  ]);
  expect(screen.queryByText("发布到此交易主机")).toBeNull();
});

test("a repository change discards a late preparation; publishing an uncertain outcome is not retried", async () => {
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
  fireEvent.click(screen.getByText("准备策略包"));
  view.rerender(
    <>
      <Packages account="account-a" repository="/new" />
      <NotificationCenter />
    </>,
  );
  await act(async () => finish(code));
  expect(screen.queryByText("发布到此交易主机")).toBeNull();
  labAnswers(async () => code);
  await act(async () => fireEvent.click(screen.getByText("准备策略包")));
  vi.mocked(strategyRequest).mockRejectedValue(new Error("connection lost"));
  await act(async () => fireEvent.click(screen.getByText("发布到此交易主机")));
  expect(writes()).toHaveLength(1);
  expect(screen.getByText(/connection lost/)).toBeDefined();
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
  render(<Packages account="account-a" repository="" record={record} connected />);
  await waitFor(() => expect(screen.getByLabelText("实例 ID")).toBeDefined());
  fireEvent.change(screen.getByLabelText("实例 ID"), { target: { value: "new-run" } });
  fireEvent.change(screen.getByLabelText("已发布版本"), { target: { value: "example/one" } });
  fireEvent.click(screen.getByLabelText("SHFE.rb2610"));
  fireEvent.change(screen.getByLabelText("K 线周期"), {
    target: { value: String(Period.MINUTES_5) },
  });
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
  expect(operation.case).toBe("startStrategy");
  if (operation.case !== "startStrategy") throw new Error("missing start");
  expect(operation.value).toMatchObject({
    id: "new-run",
    instruments: [rb],
    period: Period.MINUTES_5,
    weightLimits: { gross: { text: "1.00000001" }, net: { text: "0" } },
    params: { window: { value: { case: "integer", value: 9007199254740993n } } },
  });
  expect(screen.getByText("正在现场预热")).toBeDefined();
});

test("an ended instance resumes from its record under a new id after review", async () => {
  vi.mocked(strategyRequest).mockResolvedValue(runtime(InstanceState.INTERRUPTED));
  render(<Packages account="account-a" repository="" record={record} connected />);
  await waitFor(() => expect(screen.getByLabelText("实例 ID")).toBeDefined());
  fireEvent.change(screen.getByLabelText("实例 ID"), { target: { value: "run-b" } });
  await click("添加参数");
  fireEvent.change(screen.getByLabelText("参数名 1"), { target: { value: "lots" } });
  fireEvent.change(screen.getByLabelText("参数值 1"), { target: { value: "4" } });
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
    params: { lots: { value: { case: "integer", value: 4n } } },
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
  fireEvent.click(screen.getByRole("button", { name: "读取主机策略包" }));
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
  fireEvent.change(screen.getByLabelText("已发布版本"), { target: { value: "example/one" } });
  fireEvent.click(screen.getByLabelText("SHFE.rb2610"));
  await click("核对启动配置");
  view.rerender(
    <Packages
      {...props}
      record={{ ...record, session: { ...record.session!, policyRevision: "policy-b" } }}
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
