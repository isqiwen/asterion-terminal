import { RequestError } from "../../src/host/action-error";
import { useEffect, useState } from "react";
import { lab } from "../../src/bridge/lab";
import type { TerminalContext } from "../../src/host/feature-registry";
import { useWorkspaceDraft } from "../../src/host/workspace/drafts";
import { type MessageValues, translate } from "../../src/i18n";
import { type DisplayError, asDisplayError } from "../../src/i18n/errors";
import { identityName } from "../../src/ui/contract-names";
import { ContractHistory } from "../futures-market/ContractHistory";
const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.data-workbench", key, values);

// Listed at most at once; the search narrows the rest.
const shownAtOnce = 200;

type Stored = { contract: string; name: string; dailyDays: number; minuteDays: number };

// The contracts a research dataset version holds, and the chosen one's history.
// An empty version is the current one.
export function StoredContracts({
  context,
  version,
}: {
  context: TerminalContext;
  version: string;
}) {
  const [selected, setSelected] = useWorkspaceDraft("stored-contracts-selected", "");
  const [search, setSearch] = useWorkspaceDraft("stored-contracts-search", "");
  const [stored, setStored] = useState<{ items: Stored[]; error: DisplayError } | null>(null);
  useEffect(() => {
    let active = true;
    void lab({ case: "listStoredContracts", value: { datasetVersion: version } })
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
        if (active) setStored({ items, error: "" });
      })
      .catch(reason => active && setStored({ items: [], error: asDisplayError(reason) }));
    return () => {
      active = false;
    };
  }, [version]);
  const items = stored?.items ?? [];
  const nameFor = (item: Stored) => identityName(item.contract, item.name) || item.contract;
  const matched = items.filter(item =>
    `${item.contract} ${nameFor(item)}`.toUpperCase().includes(search.trim().toUpperCase()),
  );
  const visible = matched.slice(0, shownAtOnce);
  const active = matched.some(item => item.contract === selected) ? selected : visible[0]?.contract;
  return (
    <>
      {!stored ? (
        <p className="subtle">{t("读取中")}</p>
      ) : stored.error ? (
        <RequestError error={stored.error} namespace="asterion.terminal.data-workbench" />
      ) : !items.length ? (
        <p className="subtle">{t("还没有合约")}</p>
      ) : (
        <div className="stored-contracts">
          <div className="stored-contracts-list">
            <input
              type="search"
              aria-label={t("搜索已采集合约")}
              placeholder={t("搜索已采集合约")}
              value={search}
              onChange={event => setSearch(event.target.value)}
            />
            <div role="group" aria-label={t("已采集合约")}>
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
                              Math.min(
                                visible.length - 1,
                                index + (event.key === "ArrowUp" ? -1 : 1),
                              ),
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
                {t("另有 {count} 个合约，搜索以缩小范围", {
                  count: matched.length - visible.length,
                })}
              </p>
            )}
          </div>
          <div className="stored-contracts-detail">
            {active ? (
              <ContractHistory
                key={active}
                venue={active.split("/")[0]}
                symbol={active}
                historyContractId={active}
                datasetVersion={version}
                context={context}
              />
            ) : (
              <p className="market-chart-empty">{t("没有匹配的合约")}</p>
            )}
          </div>
        </div>
      )}
    </>
  );
}
