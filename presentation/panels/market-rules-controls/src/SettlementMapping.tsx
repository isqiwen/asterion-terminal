import { useEffect, useState } from "react";
import type { RequestClient } from "@asterion/runtime-client/requests";
import type { components } from "@asterion/api-types/schema";
import type { RulePeriod } from "./public";

type Evidence = components["schemas"]["SettlementEvidence-Output"];
type Confirmation = components["schemas"]["SettlementConfirmation"];
type Version = {
  id: string;
  manifest: { scope: { symbol?: string }; first?: string; last?: string };
};

export function SettlementMapping({
  api,
  contract,
  onApply,
}: {
  api: RequestClient;
  contract: string;
  onApply: (period: RulePeriod) => void;
}) {
  const [versions, setVersions] = useState<Version[]>([]);
  const [version, setVersion] = useState("");
  const [day, setDay] = useState("");
  const [evidence, setEvidence] = useState<Evidence | null>(null);
  const [fee, setFee] = useState("");
  const [margin, setMargin] = useState("");
  const [start, setStart] = useState("");
  const [end, setEnd] = useState("");
  const [note, setNote] = useState("");
  const [confirmed, setConfirmed] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  useEffect(() => {
    let live = true;
    api
      .request<{ items: Version[] }>(
        "/data/catalog?type_id=futures.settlement&layer=STANDARD&limit=100",
      )
      .then((v) => {
        if (live) setVersions(v.items);
      })
      .catch((e) => {
        if (live) setError(String(e));
      });
    return () => {
      live = false;
    };
  }, [api]);
  async function read() {
    setBusy(true);
    setError("");
    setEvidence(null);
    setConfirmed(false);
    try {
      setEvidence(
        await api.request<Evidence>(
          "/contract-rules/settlement/preview",
          {
            version_id: version,
            contract_id: contract,
            trading_day: day,
          },
        ),
      );
    } catch (e) {
      setError(String(e));
    } finally {
      setBusy(false);
    }
  }
  async function apply() {
    if (!evidence || !confirmed || !fee || !margin) return;
    setBusy(true);
    setError("");
    try {
      const result = await api.request<RulePeriod>(
        "/contract-rules/settlement/confirm",
        {
          evidence,
          start,
          end,
          interpretation: note,
          fee_field:
            fee === "yuan_per_lot" ? "trading_fee" : "trading_fee_rate",
          fee_unit: fee as Confirmation["fee_unit"],
          margin_unit: margin as Confirmation["margin_unit"],
          fee_scope: "long_open_and_non_today_close",
          availability_assumption: "after_source_day",
        },
      );
      onApply(result);
    } catch (e) {
      setError(String(e));
    } finally {
      setBusy(false);
    }
  }
  return (
    <fieldset disabled={busy}>
      <legend>采用每日结算参数</legend>
      <p>
        先在数据同步中获取“每日结算参数”。选择固定版本及其中一个交易日；缺失值不会补成零。
      </p>
      <label>
        结算参数版本
        <select
          value={version}
          onChange={(e) => {
            setVersion(e.target.value);
            setEvidence(null);
          }}
        >
          <option value="">选择已同步版本</option>
          {versions.map((v) => (
            <option key={v.id} value={v.id}>
              {v.manifest.scope.symbol} · {v.manifest.first}—{v.manifest.last} ·{" "}
              {v.id.slice(0, 8)}
            </option>
          ))}
        </select>
      </label>
      <label>
        结算版本 ID
        <input
          value={version}
          onChange={(e) => {
            setVersion(e.target.value);
            setEvidence(null);
          }}
          placeholder="也可填写历史版本 ID"
        />
      </label>
      <label>
        参数交易日
        <input
          type="date"
          value={day}
          onChange={(e) => {
            setDay(e.target.value);
            setEvidence(null);
          }}
        />
      </label>
      <button
        type="button"
        disabled={!version || !day || !contract}
        onClick={() => void read()}
      >
        读取结算参数
      </button>
      {evidence && (
        <div aria-label="结算参数确认">
          <p>
            {evidence.row.symbol} · 参数交易日 {evidence.row.trading_day} ·
            采集于 {evidence.observed_at}
          </p>
          <p>
            交易费率原值 {evidence.row.trading_fee_rate ?? "缺失"}
            ；按手手续费原值 {evidence.row.trading_fee ?? "缺失"}
            ；买投机保证金原值 {evidence.row.long_margin_rate ?? "缺失"}
            ；平今费率原值 {evidence.row.offset_today_fee ?? "缺失"}。
          </p>
          <p>
            历史公布时刻未知。下列日期是你明确指定的研究假设，不能作为历史可获知性证明；开始日期必须晚于参数交易日。本模型仅使用买投机保证金和普通开平仓费用，不支持平今、套保或卖空规则。
          </p>
          <label>
            手续费原值单位
            <select
              value={fee}
              onChange={(e) => {
                setFee(e.target.value);
                setConfirmed(false);
              }}
            >
              <option value="">请核实字段和单位</option>
              <option value="yuan_per_lot">按手手续费：元 / 手</option>
              <option value="ratio">交易费率：比例</option>
              <option value="percent">交易费率：百分数</option>
              <option value="permille">交易费率：千分数</option>
              <option value="permyriad">交易费率：万分数</option>
            </select>
          </label>
          <label>
            买投机保证金原值单位
            <select
              value={margin}
              onChange={(e) => {
                setMargin(e.target.value);
                setConfirmed(false);
              }}
            >
              <option value="">请核实单位</option>
              <option value="ratio">比例（0—1）</option>
              <option value="percent">百分数（0—100）</option>
            </select>
          </label>
          <label>
            假设生效开始
            <input
              type="date"
              value={start}
              onChange={(e) => {
                setStart(e.target.value);
                setConfirmed(false);
              }}
            />
          </label>
          <label>
            假设生效结束
            <input
              type="date"
              value={end}
              onChange={(e) => {
                setEnd(e.target.value);
                setConfirmed(false);
              }}
            />
          </label>
          <label>
            单位、开平仓与期间确认依据
            <input
              value={note}
              onChange={(e) => {
                setNote(e.target.value);
                setConfirmed(false);
              }}
            />
          </label>
          <label>
            <input
              type="checkbox"
              checked={confirmed}
              onChange={(e) => setConfirmed(e.target.checked)}
            />
            我确认上述单位，并假设此费用同时适用于多头开仓和非平今平仓，期间内参数不变；历史公布时刻未经证明。
          </label>
          <button
            type="button"
            disabled={
              !confirmed || !fee || !margin || !start || !end || !note.trim()
            }
            onClick={() => void apply()}
          >
            确认并采用此期间
          </button>
        </div>
      )}
      {error && <p role="alert">{error}</p>}
    </fieldset>
  );
}
