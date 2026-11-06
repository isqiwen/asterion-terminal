import { useEffect, useState } from "react";
import {
  MarketUnset,
  identityName,
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
const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.futures-market", key, values);

// Listed at most at once; the search narrows the rest.
const shownAtOnce = 200;

type Stored = { contract: string; name: string; dailyDays: number; minuteDays: number };

// The contracts the research dataset holds, and the chosen one's history.
export function MarketPanel({ context }: { context: TerminalContext }) {
  const [selected, setSelected] = useWorkspaceDraft("history-market-selected", "");
  const [search, setSearch] = useWorkspaceDraft("history-market-search", "");
  const endpoint = context.snapshot?.quotes_endpoint;
  const address =
    endpoint && "host" in endpoint
      ? `${endpoint.host}:${endpoint.port}/${endpoint.fingerprint}`
      : "";
  const [stored, setStored] = useState<{
    address: string;
    items: Stored[];
    error: DisplayError;
  } | null>(null);
  useEffect(() => {
    let active = true;
    void lab({ case: "listStoredContracts", value: {} })
      .then(reply => {
        // Contracts with minutes first, then by identity.
        const items = reply.contracts
          .map(({ contract, name, daily, minutes }) => ({
            contract,
            name,
            dailyDays: daily?.days ?? 0,
            minuteDays: minutes?.days ?? 0,
          }))
          .sort(
            (a, b) =>
              Number(!a.minuteDays) - Number(!b.minuteDays) || a.contract.localeCompare(b.contract),
          );
        if (active) setStored({ address, items, error: "" });
      })
      .catch(reason => active && setStored({ address, items: [], error: asDisplayError(reason) }));
    return () => {
      active = false;
    };
  }, [address]);
  const current = stored?.address === address ? stored : null;
  const items = current?.items ?? [];
  const nameFor = (item: Stored) => identityName(item.contract, item.name) || item.contract;
  const matched = items.filter(item =>
    `${item.contract} ${nameFor(item)}`.toUpperCase().includes(search.trim().toUpperCase()),
  );
  const visible = matched.slice(0, shownAtOnce);
  const active = matched.some(item => item.contract === selected) ? selected : visible[0]?.contract;
  if (!address) return <MarketUnset openSettings={context.openSettings} />;
  if (!current)
    return (
      <div className="workspace-empty">
        <p>{t("读取中")}</p>
      </div>
    );
  if (current.error)
    return (
      <div className="workspace-empty" role="alert">
        <strong>{t("读取历史合约失败")}</strong>
        <ErrorNotice error={current.error} />
      </div>
    );
  if (!items.length)
    return (
      <div className="workspace-empty">
        <strong>{t("尚无已采集的历史合约")}</strong>
        <p>{t("在数据页采集日线或分钟线后，这里可以查看历史 K 线。")}</p>
        <button className="primary" onClick={() => context.navigate("workspace.coverage")}>
          {t("采集历史数据")}
        </button>
      </div>
    );
  return (
    <section className="history-market-board" aria-label={t("历史行情")}>
      <div className="history-market-contracts">
        <header>
          <strong>{t("历史合约")}</strong>
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
              {nameFor(item) !== item.contract && <small>{item.contract}</small>}
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
        {matched.length > visible.length && (
          <p className="subtle">
            {t("另有 {count} 个合约，请用搜索缩小范围", {
              count: matched.length - visible.length,
            })}
          </p>
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
          <p className="market-chart-empty">{t("没有匹配的历史合约")}</p>
        )}
      </div>
    </section>
  );
}
