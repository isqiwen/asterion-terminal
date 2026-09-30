import { translate, useWorkspaceDraft, type TerminalContext } from "../contract";
import { ContractHistory } from "./ContractHistory";
const t = (key: string) => translate("asterion.terminal.futures-market", key);

export function MarketPanel({ context }: { context: TerminalContext }) {
  const [selected, setSelected] = useWorkspaceDraft("history-market-selected", "");
  const [search, setSearch] = useWorkspaceDraft("history-market-search", "");
  const offline = !context.snapshot?.research?.online;
  const completed = (context.snapshot?.research?.tasks ?? []).filter(
    task =>
      (task.kind === "minute_download" || task.kind === "daily_download") &&
      task.state === "succeeded",
  );
  const instruments = [...new Set(completed.map(task => task.instrument))].sort();
  const catalogContract = (instrument: string) =>
    context.snapshot?.market?.catalog.contracts.find(item => item.contract_id === instrument);
  const nameFor = (instrument: string) =>
    context.snapshot?.history_contracts?.items
      .find(item => item.code === instrument)
      ?.name.trim() ||
    catalogContract(instrument)?.name.trim() ||
    undefined;
  const visible = instruments.filter(instrument =>
    `${instrument} ${catalogContract(instrument)?.symbol ?? ""} ${nameFor(instrument) ?? ""}`
      .toUpperCase()
      .includes(search.trim().toUpperCase()),
  );
  const active = visible.includes(selected) ? selected : visible[0];
  const venue = active?.split("/")[0];
  const symbol = active;
  return (
    <section className="history-market-board" aria-label={t("历史行情")}>
      <div className="history-market-contracts">
        <header>
          <strong>{t("历史合约")}</strong>
          <button onClick={() => context.navigate("workspace.data", { page: "history" })}>
            {t("下载历史数据")}
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
          {visible.map((instrument, index) => (
            <button
              key={instrument}
              title={instrument.replace("/", " · ")}
              aria-pressed={active === instrument}
              onClick={() => setSelected(instrument)}
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
                setSelected(visible[target]);
                const button = event.currentTarget.parentElement?.children[target];
                if (button instanceof HTMLButtonElement) {
                  button.focus({ preventScroll: true });
                  button.scrollIntoView({ block: "nearest", inline: "nearest" });
                }
              }}
            >
              <span>{nameFor(instrument) ?? instrument.replace("/", " · ")}</span>
              {nameFor(instrument) && <small>{instrument.replace("/", " · ")}</small>}
              <small>
                {[
                  ...[
                    ...new Set(
                      completed
                        .filter(
                          task => task.instrument === instrument && task.kind === "minute_download",
                        )
                        .map(task => task.minute_interval_minutes),
                    ),
                  ]
                    .filter((period): period is number => period !== undefined)
                    .sort((a, b) => a - b)
                    .map(period => `${period} min`),
                  ...(completed.some(
                    task => task.instrument === instrument && task.kind === "daily_download",
                  )
                    ? [t("日K")]
                    : []),
                ].join(" / ")}
              </small>
            </button>
          ))}
        </div>
        {!visible.length && (
          <p role="status">
            {t(
              instruments.length
                ? "没有匹配的历史合约"
                : offline
                  ? "历史数据服务未连接"
                  : "尚无已下载的历史合约",
            )}
          </p>
        )}
      </div>
      <div className="history-market-detail">
        {active && venue && symbol ? (
          <ContractHistory
            key={active}
            venue={venue}
            symbol={symbol}
            historyContractId={active}
            context={context}
          />
        ) : (
          <div className="market-chart-empty">
            {instruments.length ? (
              <>
                <p>{t("没有匹配的历史合约")}</p>
                <button onClick={() => setSearch("")}>{t("清除搜索")}</button>
              </>
            ) : offline ? (
              <>
                <p>{t("历史数据服务未连接")}</p>
                <button onClick={() => context.navigate("workspace.data", { page: "history" })}>
                  {t("管理历史数据")}
                </button>
              </>
            ) : (
              <p>{t("请在数据工作区通过数据源下载历史数据。")}</p>
            )}
          </div>
        )}
      </div>
    </section>
  );
}
