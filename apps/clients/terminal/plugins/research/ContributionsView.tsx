import { useState } from "react";
import type { EvaluationContributions } from "../../src/bridge/client";
import { LedgerChecks } from "./LedgerView";
import { t } from "./shared";

export function ContributionsView({
  evidence,
  currency,
}: {
  evidence: EvaluationContributions;
  currency: string;
}) {
  const [contractIndex, setContractIndex] = useState(0),
    [page, setPage] = useState(0),
    [accountPage, setAccountPage] = useState(0),
    [selected, setSelected] = useState<number | null>(null),
    [fillPage, setFillPage] = useState(0);
  const contract = evidence.contracts[contractIndex];
  const rows = contract.days.slice(page * 50, page * 50 + 50);
  const detail = selected === null ? null : contract.days.find(d => d.source_index === selected);
  const accountRows = evidence.days.slice(accountPage * 50, accountPage * 50 + 50);
  return (
    <section aria-label={t("固定原合约贡献")}>
      <h5>{t("固定原合约贡献")}</h5>
      <p role="status">
        {t(
          evidence.reconciled
            ? "账本金额核对一致，不代表策略有效或已获交易许可"
            : "存在未解释差额，不能采信完整归因",
        )}
      </p>
      <p>
        {t(
          "完整账户原日线，按实际合约与交易日核对；毛变动包含未平仓结算影响，不是已平仓交易利润。",
        )}
      </p>
      <p>{t("账户本金不分配给合约，不展示未经定义的合约收益率；本表不是策略或方向归因。")}</p>
      <dl>
        {(
          [
            ["原期初资金", evidence.deposit],
            ["累计结算毛变动", evidence.gross],
            ["累计手续费", evidence.fees],
            ["解释净变动", evidence.explained_net],
            ["原账户净变动", evidence.recorded_net],
            ["累计未解释差额", evidence.residual],
          ] as const
        ).map(([label, value]) => (
          <div key={label}>
            <dt>{t(label)}</dt>
            <dd>
              {value} {currency}
            </dd>
          </div>
        ))}
      </dl>
      <table aria-label={t("全部原合约贡献合计")}>
        <thead>
          <tr>
            {["真实合约", "合约乘数", "累计结算毛变动", "累计手续费", "解释净变动"].map(s => (
              <th key={s}>{t(s)}</th>
            ))}
          </tr>
        </thead>
        <tbody>
          {evidence.contracts.map(c => (
            <tr key={c.venue + "/" + c.symbol}>
              <td>
                {c.venue}/{c.symbol}
              </td>
              <td>{c.multiplier}</td>
              <td>{c.gross}</td>
              <td>{c.fees}</td>
              <td>{c.net}</td>
            </tr>
          ))}
        </tbody>
      </table>
      <label>
        {t("查看原贡献合约")}
        <select
          value={contractIndex}
          onChange={e => {
            setContractIndex(Number(e.target.value));
            setPage(0);
            setSelected(null);
            setFillPage(0);
          }}
        >
          {evidence.contracts.map((c, i) => (
            <option key={c.venue + "/" + c.symbol} value={i}>
              {c.venue}/{c.symbol}
            </option>
          ))}
        </select>
      </label>
      <LedgerChecks
        rows={[contract.final_position]}
        label={t("所选合约期末净持仓核对")}
        currency={currency}
      />
      <table aria-label={t("原合约逐日贡献")}>
        <thead>
          <tr>
            {[
              "原行索引",
              "交易日",
              "原结算价",
              "期初净手数",
              "净手数变动",
              "期末净手数",
              "日结毛变动",
              "当日手续费",
              "原件明细",
            ].map(s => (
              <th key={s}>{t(s)}</th>
            ))}
          </tr>
        </thead>
        <tbody>
          {rows.map(r => (
            <tr key={r.source_index}>
              <td>{r.source_index}</td>
              <td>{r.trading_day}</td>
              <td>{r.settlement}</td>
              <td>{r.opening_quantity}</td>
              <td>{r.quantity_change}</td>
              <td>{r.closing_quantity}</td>
              <td>{r.gross}</td>
              <td>{r.fees}</td>
              <td>
                <button
                  onClick={() => {
                    setSelected(r.source_index);
                    setFillPage(0);
                  }}
                >
                  {t("查看原成交与核对项")}
                </button>
              </td>
            </tr>
          ))}
        </tbody>
      </table>
      <p>
        {page * 50 + 1}–{page * 50 + rows.length} / {contract.days.length}
      </p>
      <button disabled={page === 0} onClick={() => setPage(page - 1)}>
        {t("上一页")}
      </button>
      <button disabled={(page + 1) * 50 >= contract.days.length} onClick={() => setPage(page + 1)}>
        {t("下一页")}
      </button>
      {detail && (
        <section aria-label={t("所选合约原日结明细")}>
          <h6>
            {contract.venue}/{contract.symbol} · {detail.trading_day} · {detail.source_index}
          </h6>
          <p>
            {t("原观察时点UTC")}：
            {detail.timestamp_ns === "0"
              ? t("未记录观察时点")
              : new Date(Number(BigInt(detail.timestamp_ns) / 1000000n)).toISOString()}{" "}
            <code>{detail.timestamp_ns}</code>
          </p>
          <LedgerChecks rows={detail.checks} label={t("所选日结核对项")} currency={currency} />
          <table aria-label={t("所选合约原成交")}>
            <thead>
              <tr>
                {["成交身份", "委托身份", "方向", "原数量", "原成交价", "原手续费"].map(s => (
                  <th key={s}>{t(s)}</th>
                ))}
              </tr>
            </thead>
            <tbody>
              {detail.fills.slice(fillPage * 50, fillPage * 50 + 50).map(f => (
                <tr key={f.id}>
                  <td>
                    <code>{f.id}</code>
                  </td>
                  <td>
                    <code>{f.order_id}</code>
                  </td>
                  <td>{t(f.buy ? "买" : "卖")}</td>
                  <td>{f.quantity}</td>
                  <td>{f.price}</td>
                  <td>{f.fee}</td>
                </tr>
              ))}
            </tbody>
          </table>
          <button disabled={fillPage === 0} onClick={() => setFillPage(fillPage - 1)}>
            {t("上一页原成交")}
          </button>
          <button
            disabled={(fillPage + 1) * 50 >= detail.fills.length}
            onClick={() => setFillPage(fillPage + 1)}
          >
            {t("下一页原成交")}
          </button>
        </section>
      )}
      <table aria-label={t("原账户逐日贡献核对")}>
        <thead>
          <tr>
            {[
              "原行索引",
              "交易日",
              "日结毛变动",
              "当日手续费",
              "预期余额",
              "原余额",
              "原权益",
              "累计未解释差额",
            ].map(s => (
              <th key={s}>{t(s)}</th>
            ))}
          </tr>
        </thead>
        <tbody>
          {accountRows.map(r => (
            <tr key={r.source_index}>
              <td>{r.source_index}</td>
              <td>{r.trading_day}</td>
              <td>{r.gross}</td>
              <td>{r.fees}</td>
              <td>{r.expected_balance}</td>
              <td>{r.recorded_balance}</td>
              <td>{r.recorded_equity}</td>
              <td>{r.cumulative_residual}</td>
            </tr>
          ))}
        </tbody>
      </table>
      <p>
        {accountPage * 50 + 1}–{accountPage * 50 + accountRows.length} / {evidence.days.length}
      </p>
      <button disabled={accountPage === 0} onClick={() => setAccountPage(accountPage - 1)}>
        {t("上一页账户核对")}
      </button>
      <button
        disabled={(accountPage + 1) * 50 >= evidence.days.length}
        onClick={() => setAccountPage(accountPage + 1)}
      >
        {t("下一页账户核对")}
      </button>
      <LedgerChecks
        rows={evidence.final_checks}
        label={t("期末原账户核对项")}
        currency={currency}
      />
    </section>
  );
}
