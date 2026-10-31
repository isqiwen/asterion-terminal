import { useEffect, useState } from "react";
import {
  translate,
  lab,
  useWorkspaceDraft,
  ErrorNotice,
  asDisplayError,
  type DisplayError,
  type MessageValues,
  type TerminalContext,
} from "../contract";
import { ContractHistory } from "./ContractHistory";
import { identityName } from "./contract-name";
const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.futures-market", key, values);

// Listed at most at once; the search narrows the rest.
const shownAtOnce = 200;

type Stored = { contract: string; name: string; dailyDays: number; minuteDays: number };

// The contracts the lab has stored, Tushare's days and minutes with recorded
// minutes, and the chosen one's history.
export function MarketPanel({ context }: { context: TerminalContext }) {
  const [selected, setSelected] = useWorkspaceDraft("history-market-selected", "");
  const [search, setSearch] = useWorkspaceDraft("history-market-search", "");
  const [stored, setStored] = useState<{ items: Stored[]; error: DisplayError } | null>(null);
  useEffect(() => {
    let active = true;
    void Promise.all(
      ["tushare", "recorder"].map(provider =>
        lab({ case: "storedContracts", value: { provider } }, "storedContracts"),
      ),
    )
      .then(replies => {
        const merged = new Map<string, Stored>();
        for (const reply of replies)
          for (const item of reply.contracts) {
            const known = merged.get(item.contract) ?? {
              contract: item.contract,
              name: "",
              dailyDays: 0,
              minuteDays: 0,
            };
            merged.set(item.contract, {
              contract: item.contract,
              name: known.name || item.name,
              dailyDays: known.dailyDays + item.dailyDays,
              minuteDays: known.minuteDays + item.minuteDays,
            });
          }
        // Contracts with minutes first, then by identity.
        const items = [...merged.values()].sort(
          (a, b) =>
            Number(!a.minuteDays) - Number(!b.minuteDays) || a.contract.localeCompare(b.contract),
        );
        if (active) setStored({ items, error: "" });
      })
      .catch(reason => active && setStored({ items: [], error: asDisplayError(reason) }));
    return () => {
      active = false;
    };
  }, []);
  const items = stored?.items ?? [];
  const nameFor = (item: Stored) => item.name || identityName(item.contract) || item.contract;
  const matched = items.filter(item =>
    `${item.contract} ${nameFor(item)}`.toUpperCase().includes(search.trim().toUpperCase()),
  );
  const visible = matched.slice(0, shownAtOnce);
  const active = matched.some(item => item.contract === selected) ? selected : visible[0]?.contract;
  return (
    <section className="history-market-board" aria-label={t("历史行情")}>
      <div className="history-market-contracts">
        <header>
          <strong>{t("历史合约")}</strong>
          <button onClick={() => context.navigate("workspace.coverage")}>
            {t("采集历史数据")}
          </button>
        </header>
        <input
          type="search"
          aria-label={t("搜索历史合约")}
          placeholder={t("搜索历史合约")}
          value={search}
          onChange={event => setSearch(event.target.value)}
        />
        <div role="group" aria-label={t("历史合约")}>
          {visible.map((item, index) => (
            <button
              key={item.contract}
              title={item.contract}
              aria-pressed={active === item.contract}
              onClick={() => setSelected(item.contract)}
              onKeyDown={event => {
                if (!["ArrowUp", "ArrowDown", "Home", "End"].includes(event.key)) return;
                event.preventDefault();
                const target =
                  event.key === "Home"
                    ? 0
                    : event.key === "End"
                      ? visible.length - 1
                      : Math.max(
                          0,
                          Math.min(visible.length - 1, index + (event.key === "ArrowUp" ? -1 : 1)),
                        );
                setSelected(visible[target].contract);
                const button = event.currentTarget.parentElement?.children[target];
                if (button instanceof HTMLButtonElement) {
                  button.focus({ preventScroll: true });
                  button.scrollIntoView({ block: "nearest", inline: "nearest" });
                }
              }}
            >
              <span>{nameFor(item)}</span>
              <small>{item.contract}</small>
              <small>
                {[
                  item.dailyDays ? t("日线 {days} 天", { days: item.dailyDays }) : "",
                  item.minuteDays ? t("分钟线 {days} 天", { days: item.minuteDays }) : "",
                ]
                  .filter(Boolean)
                  .join(" · ")}
              </small>
            </button>
          ))}
        </div>
        {stored?.error && (
          <p role="alert">
            <ErrorNotice error={stored.error} />
          </p>
        )}
        {matched.length > visible.length && (
          <p className="subtle">
            {t("另有 {count} 个合约，请用搜索缩小范围", {
              count: matched.length - visible.length,
            })}
          </p>
        )}
        {stored && !visible.length && (
          <p role="status">{t(items.length ? "没有匹配的历史合约" : "尚无已采集的历史合约")}</p>
        )}
      </div>
      <div className="history-market-detail">
        {active ? (
          <ContractHistory
            key={active}
            venue={active.split("/")[0]}
            symbol={active}
            historyContractId={active}
            context={context}
          />
        ) : (
          <div className="market-chart-empty">
            <p>{t(stored ? "请在数据页采集历史数据。" : "读取中")}</p>
          </div>
        )}
      </div>
    </section>
  );
}
