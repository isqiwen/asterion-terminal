import { decimal, groupedText } from "../../src/bridge/decimal";
import type { Decimal } from "../../src/generated/asterion/common_pb";
import type { Session } from "../../src/generated/asterion/trading_pb";
import { contractLots } from "./exposure";
import { t, key } from "./live-shared";

// Display only: Trading enforces every limit itself, also counting unconfirmed
// orders by their own lots, which the session does not carry.
const number = (value: Decimal | undefined) => Number(value?.text || "0");
const money = (value: number) =>
  groupedText(decimal((Math.abs(value) < 0.005 ? 0 : value).toFixed(2)));
const percent = (value: number) => `${(value * 100).toFixed(1)}%`;

export type RiskMeter = {
  label: string;
  used: number;
  limit: number;
  text: string;
  title?: string;
  // The code Trading reports while this limit stops new openings.
  breach?: string;
};

// How much of each configured limit the account uses now, as the risk rules count it.
export function riskMeters(live: Session): RiskMeter[] {
  const limits = live.policy?.limits;
  const held = contractLots(live);
  const gross = [...held.values()].reduce((sum, value) => sum + value, 0);
  const [largest, contract] = [...held].reduce<[number, string]>(
    (best, [name, value]) => (value > best[0] ? [value, name] : best),
    [0, ""],
  );
  const working = live.workingOrders.length + live.unconfirmed.length;
  const meters: RiskMeter[] = [
    {
      label: "账户总持仓",
      used: gross,
      limit: number(limits?.maxGrossLots),
      text: `${gross} / ${number(limits?.maxGrossLots)}`,
    },
    {
      label: "单合约持仓",
      used: largest,
      limit: number(limits?.maxContractLots),
      text: `${largest} / ${number(limits?.maxContractLots)}`,
      title: contract || undefined,
    },
    {
      label: "在途委托",
      used: working,
      limit: limits?.maxWorkingOrders ?? 0,
      text: `${working} / ${limits?.maxWorkingOrders ?? 0}`,
    },
  ];
  const funds = live.funds;
  const equity = number(funds?.dynamicEquity);
  if (funds && live.policy?.maxDailyLoss) {
    const loss = Math.max(
      0,
      number(funds.preBalance) + number(funds.deposit) - number(funds.withdraw) - equity,
    );
    const limit = number(live.policy.maxDailyLoss);
    meters.push({
      label: "日内亏损",
      used: loss,
      limit,
      text: `${money(loss)} / ${money(limit)}`,
      breach: "daily_loss",
    });
  }
  if (funds && live.policy?.maxDrawdown) {
    const drawdown = Math.max(0, number(funds.peakEquity) - equity);
    const limit = number(live.policy.maxDrawdown);
    meters.push({
      label: "回撤",
      used: drawdown,
      limit,
      text: `${money(drawdown)} / ${money(limit)}`,
      breach: "drawdown",
    });
  }
  if (funds && live.policy?.maxMarginRatio && equity > 0) {
    const ratio = number(funds.margin) / equity;
    const limit = number(live.policy.maxMarginRatio);
    meters.push({
      label: "保证金率",
      used: ratio,
      limit,
      text: `${percent(ratio)} / ${percent(limit)}`,
      breach: "margin_ratio",
    });
  }
  const cancelLimit = live.policy?.maxDailyCancelsPerContract ?? 0;
  if (cancelLimit) {
    const [most, cancelled] = live.cancels.reduce<[number, string]>(
      (best, item) => (item.cancels > best[0] ? [item.cancels, key(item.instrument)] : best),
      [0, ""],
    );
    meters.push({
      label: "单合约日撤单",
      used: most,
      limit: cancelLimit,
      text: `${most} / ${cancelLimit}`,
      title: cancelled || undefined,
      breach: `daily_cancels:${cancelled}`,
    });
  }
  return meters;
}

const detailFields = [
  ["balance", "动态权益"],
  ["margin", "占用保证金"],
  ["positionProfit", "持仓盈亏"],
  ["closeProfit", "平仓盈亏"],
  ["commission", "手续费"],
  ["preBalance", "上次结算准备金"],
  ["deposit", "入金"],
  ["withdraw", "出金"],
] as const;

// The four figures an operator reads first, the broker's other figures, and
// the use of each risk limit.
export function AccountSummary({ live }: { live: Session }) {
  const funds = live.funds;
  const equity = number(funds?.dynamicEquity);
  const baseEquity = number(funds?.preBalance) + number(funds?.deposit) - number(funds?.withdraw);
  const daily = equity - baseEquity;
  const dailyTone = daily > 0 ? "ctp-up" : daily < 0 ? "ctp-down" : undefined;
  const dailyPct =
    baseEquity > 0 ? `${daily >= 0 ? "+" : ""}${((daily / baseEquity) * 100).toFixed(2)}%` : "";
  return (
    <section className="account-summary" aria-label={t("账户概览")}>
      <div className="account-summary-primary">
        {funds && (
          <dl className="account-headline">
            <div>
              <dt>{t("盯市净值")}</dt>
              <dd>{groupedText(funds.dynamicEquity)}</dd>
            </div>
            <div>
              <dt>{t("当日盈亏")}</dt>
              <dd className="account-pnl-value">
                <span className={dailyTone}>
                  {daily >= 0.005 ? `+${money(daily)}` : money(daily)}
                </span>
                {dailyPct && <small className={dailyTone}>{dailyPct}</small>}
              </dd>
            </div>
            <div>
              <dt>{t("可用资金")}</dt>
              <dd>{groupedText(funds.available)}</dd>
            </div>
            <div>
              <dt>{t("保证金占用率")}</dt>
              <dd>{equity > 0 ? percent(number(funds.margin) / equity) : "—"}</dd>
            </div>
          </dl>
        )}
        <ul className="risk-usage" aria-label={t("风控占用")}>
          {riskMeters(live).map(meter => {
            const share = meter.limit > 0 ? meter.used / meter.limit : 0;
            const breached = !!meter.breach && live.riskBreaches.includes(meter.breach);
            const tone = breached || share >= 1 ? "danger" : share >= 0.8 ? "attention" : "ok";
            return (
              <li key={meter.label} data-tone={tone} title={meter.title}>
                <span>{t(meter.label)}</span>
                <span className="risk-value">{meter.text}</span>
                <span
                  className="risk-bar"
                  role="meter"
                  aria-label={t(meter.label)}
                  aria-valuemin={0}
                  aria-valuemax={meter.limit}
                  aria-valuenow={meter.used}
                >
                  <span style={{ width: `${Math.min(share, 1) * 100}%` }} />
                </span>
              </li>
            );
          })}
        </ul>
        {funds && (
          <details className="account-details">
            <summary>{t("资金明细")}</summary>
            <dl>
              {detailFields.map(([field, label]) => (
                <div key={field}>
                  <dt>{t(label)}</dt>
                  <dd>{groupedText(funds[field])}</dd>
                </div>
              ))}
            </dl>
          </details>
        )}
      </div>
    </section>
  );
}
