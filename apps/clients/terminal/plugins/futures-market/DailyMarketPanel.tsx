import { useEffect, useRef, useState } from "react";
import type { TableListedContracts, TableProduct } from "../../src/bridge/client";
import {
  translate,
  useDailyTable,
  useWorkspaceDraft,
  ErrorNotice,
  asDisplayError,
  type DisplayError,
  type TerminalContext,
} from "../contract";
import { ContractHistory } from "./ContractHistory";
import { identityName } from "./contract-name";
const t = (key: string) => translate("asterion.terminal.futures-market", key);

export function DailyMarketPanel({ context }: { context: TerminalContext }) {
  const { snapshot, query } = context;
  const table = useDailyTable(snapshot, query);
  const held = table.held?.table;
  const online = !!snapshot?.data?.online;
  const sourceKey = JSON.stringify([snapshot?.data?.connection_id, online, held]);
  const queryRef = useRef(query);
  queryRef.current = query;
  const [products, setProducts] = useState<{
    key: string;
    items: TableProduct[];
    error: DisplayError;
  }>();
  const [exchange, setExchange] = useWorkspaceDraft("daily-market-exchange", "");
  const [product, setProduct] = useWorkspaceDraft("daily-market-product", "");
  const [selected, setSelected] = useWorkspaceDraft("daily-market-contract", "");
  useEffect(() => {
    if (!held || !online) return;
    let active = true;
    void queryRef
      .current("data.tables.products", {
        snapshot: held.snapshot,
        contracts_source: held.contracts_source,
      })
      .then(response => {
        if (active) setProducts({ key: sourceKey, items: response.table_products!, error: "" });
      })
      .catch(error => {
        if (active) setProducts({ key: sourceKey, items: [], error: asDisplayError(error) });
      });
    return () => {
      active = false;
    };
  }, [sourceKey, held, online]);
  const currentProducts = products?.key === sourceKey ? products : undefined;
  const exchanges = [...new Set(currentProducts?.items.map(item => item.exchange))];
  const venue = exchanges.includes(exchange) ? exchange : (exchanges[0] ?? "");
  const available = currentProducts?.items.filter(item => item.exchange === venue) ?? [];
  const symbol = available.some(item => item.product === product)
    ? product
    : (available[0]?.product ?? "");
  const catalogKey = JSON.stringify([sourceKey, venue, symbol]);
  const [catalog, setCatalog] = useState<{
    key: string;
    items: TableListedContracts["items"];
    error: DisplayError;
  }>();
  useEffect(() => {
    if (!held || !online || !venue || !symbol) return;
    let active = true;
    void queryRef
      .current("data.tables.listed_contracts", {
        snapshot: held.snapshot,
        contracts_source: held.contracts_source,
        exchange: venue,
        product: symbol,
      })
      .then(response => {
        if (active)
          setCatalog({ key: catalogKey, items: response.table_listed_contracts!.items, error: "" });
      })
      .catch(error => {
        if (active) setCatalog({ key: catalogKey, items: [], error: asDisplayError(error) });
      });
    return () => {
      active = false;
    };
  }, [catalogKey, held, online, venue, symbol]);
  const currentCatalog = catalog?.key === catalogKey ? catalog : undefined;
  const items = currentCatalog?.items ?? [];
  const contract = items.find(item => item.contract_id === selected) ?? items[0];
  const error = table.error || currentProducts?.error || currentCatalog?.error;
  const loading =
    online && (table.loading || (held && !currentProducts) || (symbol && !currentCatalog));
  return (
    <section className="history-market-board" aria-label={t("日线合约")}>
      <div className="history-market-contracts">
        <header>
          <strong>{t("日线合约")}</strong>
        </header>
        <div className="daily-market-selection">
          <label>
            {t("交易所")}
            <select
              value={venue}
              onChange={event => setExchange(event.target.value)}
              disabled={!exchanges.length}
            >
              {!exchanges.length && <option value="">—</option>}
              {exchanges.map(value => (
                <option key={value}>{value}</option>
              ))}
            </select>
          </label>
          <label>
            {t("品种")}
            <select
              value={symbol}
              onChange={event => setProduct(event.target.value)}
              disabled={!available.length}
            >
              {!available.length && <option value="">—</option>}
              {available.map(item => (
                <option key={item.product}>{item.product}</option>
              ))}
            </select>
          </label>
          <label>
            {t("合约月份")}
            <select
              value={contract?.contract_id ?? ""}
              onChange={event => setSelected(event.target.value)}
              disabled={!items.length}
            >
              {!items.length && <option value="">—</option>}
              {items.map(item => (
                <option key={item.contract_id} value={item.contract_id}>
                  {item.contract_id.split("/")[2]} · {item.name || identityName(item.contract_id)}
                </option>
              ))}
            </select>
          </label>
        </div>
        {contract && (
          <p>
            {contract.list_date} — {contract.delist_date}
          </p>
        )}
        <p>{t("合约目录不代表行情数据完整。图表仅显示快照中已有的数据。")}</p>
        {error && (
          <p role="alert">
            <ErrorNotice error={error} />
          </p>
        )}
        {loading ? (
          <p role="status">{t("正在读取合约目录")}</p>
        ) : (
          !contract && (
            <p role="status">
              {t(
                !online
                  ? "历史数据服务未连接"
                  : held
                    ? "该品种没有可查看的合约"
                    : "请先在数据工作区更新日线、交易日历和合约表。",
              )}
            </p>
          )
        )}
        <button onClick={() => context.navigate("workspace.data", { page: "tables" })}>
          {t("更新全市场数据")}
        </button>
      </div>
      <div className="history-market-detail">
        {contract && held && (
          <ContractHistory
            key={contract.contract_id}
            venue={venue}
            symbol={contract.contract_id}
            historyContractId={contract.contract_id}
            dailyTable={held}
            context={context}
            preferLongest
            preferenceKey="history-daily"
          />
        )}
      </div>
    </section>
  );
}
