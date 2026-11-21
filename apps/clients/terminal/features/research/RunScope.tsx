import { useId, useLayoutEffect, useRef, useState } from "react";
import type { TerminalContext } from "../../src/host/feature-registry";
import { getLocale, listSeparator, type MessageValues, translate } from "../../src/i18n";
import { warehouseName } from "../../src/ui/contract-names";
import { DateInput } from "../../src/ui/DateInput";
import { SymbolField, type useStoredCatalog } from "./LabParts";
import { symbolsOf, type RunForm } from "./research-lab";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.research", key, values);

const limits = [
  ["初始资金", "deposit"],
  ["滑点（跳）", "slippage"],
  ["单笔手数", "maxOrderLots"],
  ["单合约持仓手数", "maxContractLots"],
  ["账户总持仓手数", "maxGrossLots"],
  ["在途委托数", "maxWorkingOrders"],
  ["总权重上限", "maxGrossWeight"],
  ["净权重上限", "maxNetWeight"],
  ["保证金率上限", "maxMarginRatio"],
  ["最小调仓偏离", "minRebalanceWeight"],
  ["每年期数", "annualPeriods"],
] as const;

type ScopeProps = {
  context: TerminalContext;
  form: RunForm;
  change: (patch: Partial<RunForm>) => void;
  catalog: ReturnType<typeof useStoredCatalog>;
  // What blank days resolve to.
  begin: string;
  end: string;
};

function summary({ form, begin, end }: Pick<ScopeProps, "form" | "begin" | "end">) {
  const names = symbolsOf(form.symbols).map(symbol => warehouseName(symbol) ?? symbol);
  if (!names.length) return "";
  const shown = names.slice(0, 3).join(listSeparator());
  const day = (value: string, stored: string) => value || (/^\d/.test(stored) ? stored : "");
  const [first, last] = [day(form.begin, begin), day(form.end, end)];
  const deposit = Number(form.deposit);
  return [
    names.length > 3
      ? t("{names} 等 {count} 个品种", { names: shown, count: names.length })
      : shown,
    first && last
      ? `${first} – ${last}`
      : first
        ? t("{day} 起", { day: first })
        : last
          ? t("至 {day}", { day: last })
          : t("全部区间"),
    form.minutes === "0" ? t("日线") : t("{count} 分钟", { count: form.minutes }),
    Number.isFinite(deposit)
      ? new Intl.NumberFormat(getLocale(), { notation: "compact" }).format(deposit)
      : form.deposit,
    t("滑点 {count} 跳", { count: form.slippage }),
    ...(form.holdoutDay ? [t("留出 {day}", { day: form.holdoutDay })] : []),
  ].join(" · ");
}

// What every backtest on this page is measured under: one line, edited in a dialog.
export function RunScope(props: ScopeProps) {
  const [editing, setEditing] = useState(false);
  const text = summary(props);
  return (
    <div className="lab-scope">
      <span>{t("口径")}</span>
      {text && <p title={text}>{text}</p>}
      <button type="button" className="lab-link" onClick={() => setEditing(true)}>
        {t(text ? "修改" : "选择品种")}
      </button>
      {editing && <ScopeDialog {...props} close={() => setEditing(false)} />}
    </div>
  );
}

function ScopeDialog({
  context,
  form,
  change,
  catalog,
  begin,
  end,
  close,
}: ScopeProps & { close: () => void }) {
  const dialog = useRef<HTMLDialogElement>(null);
  const heading = useId();
  useLayoutEffect(() => {
    const element = dialog.current;
    element?.showModal();
    return () => element?.close();
  }, []);
  const dateField = (label: string, key: "begin" | "end" | "holdoutDay", placeholder?: string) => (
    <label>
      <span>{t(label)}</span>
      <DateInput
        value={form[key]}
        placeholder={placeholder}
        onChange={value => change({ [key]: value })}
      />
    </label>
  );
  return (
    <dialog
      ref={dialog}
      className="lab-scope-dialog"
      aria-labelledby={heading}
      onCancel={event => {
        event.preventDefault();
        close();
      }}
    >
      {/* Focus starts here: the symbol field opens its list when focused. */}
      <h2 id={heading} tabIndex={-1}>
        {t("回测口径")}
      </h2>
      <div className="lab-scope-fields">
        <section>
          <h3>{t("品种与区间")}</h3>
          <div className="lab-form">
            <SymbolField
              context={context}
              value={form.symbols}
              minutes={form.minutes !== "0"}
              catalog={catalog}
              onChange={symbols => change({ symbols })}
            />
            {dateField("开始日", "begin", begin)}
            {dateField("结束日", "end", end)}
            <label>
              <span>{t("周期")}</span>
              <select
                value={form.minutes}
                onChange={event => change({ minutes: event.target.value })}
              >
                <option value="0">{t("日线")}</option>
                {[1, 5, 10, 15, 30, 60].map(value => (
                  <option key={value} value={String(value)}>
                    {t("{count} 分钟", { count: value })}
                  </option>
                ))}
              </select>
            </label>
            {dateField("留出日", "holdoutDay")}
          </div>
        </section>
        <section>
          <h3>{t("资金与风控")}</h3>
          <div className="lab-form">
            {limits.map(([label, key]) => (
              <label key={key}>
                <span>{t(label)}</span>
                <input
                  value={form[key]}
                  onChange={event => change({ [key]: event.target.value })}
                />
              </label>
            ))}
            <label className="lab-check wide">
              <input
                type="checkbox"
                checked={form.zeroRiskFree}
                onChange={event => change({ zeroRiskFree: event.target.checked })}
              />
              {t("无风险利率为零")}
            </label>
          </div>
        </section>
      </div>
      <footer>
        <button type="button" className="primary" onClick={close}>
          {t("完成")}
        </button>
      </footer>
    </dialog>
  );
}
