import { ContractPicker } from "./ContractPicker";
import { TimePicker, type TimeVersion } from "../trading_time/public";
import { SettlementMapping } from "./SettlementMapping";
import type { components } from "../../api/schema";
import { SourceMapping } from "./SourceMapping";
import { useEffect, useState } from "react";
import type { RequestClient } from "../../api/requests";

export type RulePeriod = {
  settlement_basis: components["schemas"]["SettlementBasis-Output"] | null;
  start: string;
  end: string;
  margin_rate: string;
  fee_mode: "per_lot" | "notional";
  open_fee: string;
  close_fee: string;
};
export type ContractBasis = {
  provider: string;
  contract_id: string;
  version_id: string;
  checksum: string;
  connection_id: string | null;
  symbol: string;
  exchange: string;
  name: string;
  listed: string;
  delisted: string | null;
  trade_unit: string | null;
  per_unit: string | null;
  multiplier: string | null;
  quote_unit: string | null;
  quote_unit_desc: string | null;
};
export type RuleSpec = {
  trading_time: TimeVersion | null;
  contract: components["schemas"]["Contract"] | null;
  title: string;
  source: string;
  multiplier: string;
  tick_size: string;
  periods: RulePeriod[];
  basis: ContractBasis | null;
};
export type RuleVersion = { id: string; spec: RuleSpec };
const period = (): RulePeriod => ({
  settlement_basis: null,
  start: "",
  end: "",
  margin_rate: "",
  fee_mode: "per_lot",
  open_fee: "",
  close_fee: "",
});
const empty = (): RuleSpec => ({
  trading_time: null,
  contract: null,
  title: "",
  source: "",
  multiplier: "",
  tick_size: "",
  periods: [period()],
  basis: null,
});

export function RuleEvidence({ value }: { value: RuleVersion }) {
  return (
    <div aria-label="固定合约规则">
      <p>
        {value.spec.contract?.id} · {value.spec.title} · 乘数{" "}
        {value.spec.multiplier} · 最小变动价位 {value.spec.tick_size}
      </p>
      <p>规则依据：{value.spec.source}</p>
      {value.spec.basis && (
        <details>
          <summary>{value.spec.basis.provider} 固定来源资料</summary>
          <p>
            {value.spec.basis.symbol} · {value.spec.basis.name} · 每手数量{" "}
            {value.spec.basis.per_unit ?? "未提供"}{" "}
            {value.spec.basis.trade_unit} · 提供方乘数{" "}
            {value.spec.basis.multiplier ?? "未提供"}
          </p>
          <p>
            报价单位：{value.spec.basis.quote_unit ?? "未提供"}；最小报价说明：
            {value.spec.basis.quote_unit_desc ?? "未提供"}
          </p>
          <p>资料采集不证明历史规则生效时间；最终计算采用下列已确认参数。</p>
          <p className="research-hash">
            资料版本：{value.spec.basis.version_id}
            <br />
            连接：{value.spec.basis.connection_id ?? "提供方配置（无命名连接）"}
            <br />
            资料校验和：{value.spec.basis.checksum}
          </p>
        </details>
      )}
      <p>交易时间：{value.spec.trading_time?.spec.title} · {value.spec.trading_time?.spec.exchange}.{value.spec.trading_time?.spec.product}</p>
      <p className="research-hash">规则版本：{value.id}</p>
      <table className="data-table">
        <thead>
          <tr>
            <th>生效区间</th>
            <th>保证金比例</th>
            <th>收费方式</th>
            <th>开仓</th>
            <th>平仓</th>
          </tr>
        </thead>
        <tbody>
          {value.spec.periods.map((p, i) => (
            <tr key={i}>
              <td>
                {p.start}—{p.end}
              </td>
              <td>{p.margin_rate}</td>
              <td>{p.fee_mode === "per_lot" ? "元 / 手" : "成交金额比例"}</td>
              <td>{p.open_fee}</td>
              <td>
                {p.close_fee}
                {p.settlement_basis && (
                  <details>
                    <summary>结算依据与研究假设</summary>
                    <p>
                      来源交易日：{p.settlement_basis.evidence.row.trading_day}
                      ；采集时间：{p.settlement_basis.evidence.observed_at}。
                    </p>
                    <p>
                      费用单位：
                      {
                        {
                          yuan_per_lot: "元 / 手",
                          ratio: "比例",
                          percent: "百分数",
                          permille: "千分数",
                          permyriad: "万分数",
                        }[p.settlement_basis.fee_unit]
                      }
                      ；保证金单位：
                      {p.settlement_basis.margin_unit === "ratio"
                        ? "比例"
                        : "百分数"}
                      。多头开仓与非平今平仓采用相同费用。
                    </p>
                    <p>
                      {p.settlement_basis.interpretation}
                      。历史公布时刻未经证明。
                    </p>
                    <p className="research-hash">
                      来源版本：{p.settlement_basis.evidence.version_id}
                      <br />
                      校验和：{p.settlement_basis.evidence.checksum}
                    </p>
                  </details>
                )}
              </td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}

export function RulePicker({
  api,
  disabled,
  value,
  onChange,
  contract,
}: {
  api: RequestClient;
  disabled: boolean;
  value: RuleVersion | null;
  contract: string;
  onChange: (value: RuleVersion) => void;
}) {
  const [versions, setVersions] = useState<RuleVersion[]>([]);
  const [spec, setSpec] = useState(empty);
  const [editing, setEditing] = useState(false);
  const [mapping, setMapping] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  useEffect(() => {
    if (disabled) return;
    let live = true;
    api
      .request<RuleVersion[]>("/contract-rules")
      .then((v) => {
        if (live) setVersions(v);
      })
      .catch((e) => {
        if (live) setError(String(e));
      });
    return () => {
      live = false;
    };
  }, [api, disabled]);
  async function save() {
    setBusy(true);
    setError("");
    try {
      if (!spec.contract) throw new Error("请选择实际合约身份");
      if (!spec.trading_time) throw new Error("请选择交易时间版本");
      const saved = await api.request<RuleVersion>("/contract-rules", spec);
      setVersions((v) => [saved, ...v.filter((item) => item.id !== saved.id)]);
      onChange(saved);
      setEditing(false);
    } catch (e) {
      setError(String(e));
    } finally {
      setBusy(false);
    }
  }
  return (
    <section aria-label="合约规则">
      {contract && <p>当前行情代码：{contract}</p>}
      <label>
        固定规则版本
        <select
          disabled={disabled || busy}
          required
          value={value?.id || ""}
          onChange={(e) => {
            const found = versions.find((v) => v.id === e.target.value);
            if (found) onChange(found);
          }}
        >
          <option value="">选择已保存的合约规则</option>
          {value && !versions.some((v) => v.id === value.id) && (
            <option value={value.id}>原规则待核验：{value.spec.title}</option>
          )}
          {versions.map((v) => (
            <option key={v.id} value={v.id}>
              {v.spec.contract?.id} · {v.spec.title} · {v.id.slice(0, 8)}
            </option>
          ))}
        </select>
      </label>
      <button
        type="button"
        disabled={disabled || busy}
        onClick={() => {
          setSpec(empty());
          setEditing(true);
        }}
      >
        新建规则
      </button>
      <button
        type="button"
        disabled={disabled || busy}
        onClick={() => {
          setMapping(!mapping);
        }}
      >
        从合约资料创建
      </button>
      {mapping && (
        <SourceMapping
          key={api.key}
          api={api}
          disabled={disabled || busy}
          contract={value?.spec.contract?.id ?? ""}
          onApply={(mapped) => {
            setSpec({
              ...empty(),
              contract: mapped.contract,
              title: mapped.basis.name + "研究规则",
              source: `${mapped.basis.provider} · ${mapped.basis.symbol}；补充费用、保证金及生效期间依据`,
              multiplier: mapped.suggested_multiplier ?? "",
              basis: mapped.basis,
            });
            setEditing(true);
            setMapping(false);
            setError("");
          }}
        />
      )}
      {value && (
        <button
          type="button"
          disabled={disabled || busy}
          onClick={() => {
            setSpec(structuredClone(value.spec));
            setEditing(true);
          }}
        >
          复制并修改规则
        </button>
      )}
      {value && <RuleEvidence value={value} />}
      {editing && (
        <fieldset disabled={disabled || busy}>
          <legend>保存新的不可变规则版本</legend>
          {spec.basis ? <p>规则合约：{spec.contract?.id}（来源资料已固定）</p> : <ContractPicker api={api} value={spec.contract} onChange={actual => setSpec(s => ({ ...s, contract: actual, trading_time: null, periods: [period()] }))} />}
          <TimePicker api={api} value={spec.trading_time} onChange={v => setSpec(s => ({...s, trading_time:v}))} disabled={disabled || busy} />
          <p>
            按实际依据填写，不预置交易所参数。修改将产生新版本，已有运行保持原规则。
          </p>
          {spec.basis && (
            <p>
              已绑定来源资料 {spec.basis.symbol} · {spec.basis.version_id}
              。请补充来源说明、最小变动价位、费用、保证金及实际生效期间后保存。
            </p>
          )}
          <div className="research-fields">
            {(
              [
                ["title", "规则名称"],
                ["source", "规则来源与依据"],
                ["multiplier", "合约乘数"],
                ["tick_size", "最小变动价位"],
              ] as const
            ).map(([key, label]) => (
              <label key={key}>
                {label}
                <input
                  value={spec[key]}
                  onChange={(e) =>
                    setSpec((s) => ({ ...s, [key]: e.target.value }))
                  }
                />
              </label>
            ))}
          </div>
          {spec.periods.map((p, i) => (
            <div className="research-fields" key={i}>
              {(
                [
                  ["start", "规则开始日期"],
                  ["end", "规则结束日期"],
                  ["margin_rate", "保证金比例（0—1）"],
                  ["open_fee", "开仓费用"],
                  ["close_fee", "平仓费用"],
                ] as const
              ).map(([key, label]) => (
                <label key={key}>
                  {label}
                  <input
                    type={key === "start" || key === "end" ? "date" : "number"}
                    min="0"
                    step="any"
                    readOnly={!!p.settlement_basis}
                    value={p[key]}
                    onChange={(e) =>
                      setSpec((s) => ({
                        ...s,
                        periods: s.periods.map((v, n) =>
                          n === i ? { ...v, [key]: e.target.value } : v,
                        ),
                      }))
                    }
                  />
                </label>
              ))}
              <label>
                收费方式
                <select
                  disabled={!!p.settlement_basis}
                  value={p.fee_mode}
                  onChange={(e) =>
                    setSpec((s) => ({
                      ...s,
                      periods: s.periods.map((v, n) =>
                        n === i
                          ? {
                              ...v,
                              fee_mode: e.target
                                .value as RulePeriod["fee_mode"],
                            }
                          : v,
                      ),
                    }))
                  }
                >
                  <option value="per_lot">按手（元）</option>
                  <option value="notional">按成交金额（比例）</option>
                </select>
              </label>
              {p.settlement_basis && (
                <p>
                  已采用 {p.settlement_basis.evidence.row.trading_day}{" "}
                  的结算参数；修改单位或期间请重新确认。
                </p>
              )}
              <details>
                <summary>从结算参数填写此期间</summary>
                <SettlementMapping
                  key={api.key + spec.contract?.id}
                  api={api}
                  contract={spec.contract?.id ?? ""}
                  onApply={(mapped) =>
                    setSpec((s) => ({
                      ...s,
                      periods: s.periods.map((v, n) => (n === i ? mapped : v)),
                    }))
                  }
                />
              </details>
              {p.settlement_basis && (
                <button
                  type="button"
                  onClick={() =>
                    setSpec((s) => ({
                      ...s,
                      periods: s.periods.map((v, n) =>
                        n === i ? { ...v, settlement_basis: null } : v,
                      ),
                    }))
                  }
                >
                  解除结算依据并手动填写
                </button>
              )}
              <button
                type="button"
                disabled={spec.periods.length === 1}
                onClick={() =>
                  setSpec((s) => ({
                    ...s,
                    periods: s.periods.filter((_, n) => n !== i),
                  }))
                }
              >
                移除此期间
              </button>
            </div>
          ))}
          <button
            type="button"
            onClick={() =>
              setSpec((s) => ({ ...s, periods: [...s.periods, period()] }))
            }
          >
            增加生效期间
          </button>
          <button type="button" onClick={() => void save()}>
            保存规则版本
          </button>
          <button type="button" onClick={() => setEditing(false)}>
            取消编辑
          </button>
        </fieldset>
      )}
      {error && <p role="alert">{error}</p>}
    </section>
  );
}
