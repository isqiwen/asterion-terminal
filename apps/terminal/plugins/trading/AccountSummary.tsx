import { translate, type TerminalContext } from "../contract";
import { timestamp } from "../../src/bridge/client";
const t = (key: string) => translate("asterion.terminal.trading", key);
export function AccountSummary({ context }: { context: TerminalContext }) {
  const paper = context.snapshot?.paper;
  const open = () => context.navigate("workspace.trading");
  if (!paper)
    return (
      <div className="overview-account-empty">
        <span>{t("尚未创建模拟账户")}</span>
        <button onClick={open}>{t("开始模拟")}</button>
      </div>
    );
  const stale =
    paper.storage_state === "recovery_required" ||
    context.snapshot?.connection?.state !== "connected";
  return (
    <>
      <div className="overview-card-bar">
        <span>
          {t("历史模拟")} · {paper.contract.currency}
        </span>
        <button onClick={open}>{t("查看账户")}</button>
      </div>
      {stale && (
        <p role="alert" className="dashboard-caption bad">
          <strong>{t("模拟会话需要恢复")}</strong> ·{" "}
          {t("交易连接或存储异常，显示的是最后确认状态。")}
        </p>
      )}
      <dl className="account-summary">
        {[
          ["账户权益", paper.equity],
          ["可用资金", paper.available],
          ["浮动盈亏", paper.unrealized],
          ["占用保证金", paper.margin],
        ].map(([label, value]) => (
          <div key={label}>
            <dt>{t(label)}</dt>
            <dd>{value}</dd>
          </div>
        ))}
      </dl>
      {paper.positions.length ? (
        <div className="dashboard-table" tabIndex={0} role="region" aria-label={t("模拟持仓")}>
          <table className="data-table">
            <thead>
              <tr>
                {["合约", "方向", "持仓手数", "持仓成本"].map(key => (
                  <th key={key}>{t(key)}</th>
                ))}
              </tr>
            </thead>
            <tbody>
              {paper.positions.map((position, index) => (
                <tr key={index}>
                  <td>{paper.contract.symbol}</td>
                  <td>
                    {t(position.side === "buy" ? "多头" : "空头")} ·{" "}
                    {t(position.bucket === "today" ? "今仓" : "昨仓")}
                  </td>
                  <td className="numeric">{position.quantity}</td>
                  <td className="numeric">{position.basis}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      ) : (
        <p className="dashboard-caption">{t("暂无持仓")}</p>
      )}
      <div className="overview-account-footer">
        <span>
          {t("回放进度")} {paper.cursor} / {paper.total}
        </span>
        {paper.timestamp_ns && <time>{timestamp(paper.timestamp_ns)}</time>}
      </div>
      <details className="overview-account-details">
        <summary>{t("资金明细")}</summary>
        <dl className="account-summary">
          <div>
            <dt>{t("委托冻结")}</dt>
            <dd>{paper.frozen}</dd>
          </div>
          <div>
            <dt>{t("累计手续费")}</dt>
            <dd>{paper.fees}</dd>
          </div>
        </dl>
      </details>
    </>
  );
}
