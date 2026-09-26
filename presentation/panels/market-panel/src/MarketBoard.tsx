import type { Bar, Snapshot } from "@asterion/ui-data-panel/client";
export function MarketBoard({
  snapshots,
  selected,
  onSnapshot,
  bars,
  contract,
  onContract,
  search,
  onSearch,
}: {
  snapshots: Snapshot[];
  selected?: Snapshot;
  onSnapshot: (s: Snapshot) => void;
  bars: Bar[];
  contract: string;
  onContract: (c: string) => void;
  search: string;
  onSearch: (s: string) => void;
}) {
  const latest = new Map<string, Bar>();
  for (const bar of bars) {
    if (
      !latest.has(bar.contract) ||
      Date.parse(bar.event_time) >
        Date.parse(latest.get(bar.contract)!.event_time)
    )
      latest.set(bar.contract, bar);
  }
  const rows = [...latest.values()].filter((b) =>
    b.contract.toLowerCase().includes(search.toLowerCase()),
  );
  return (
    <aside className="market-board">
      <div className="panel-heading">
        <h2>合约行情</h2>
        <span className="panel-spacer" />
        <small>历史快照 · 非实时</small>
      </div>
      <div className="market-filter">
        <input
          aria-label="搜索合约"
          placeholder="搜索合约…"
          value={search}
          onChange={(e) => onSearch(e.target.value)}
        />
        <select
          aria-label="行情快照"
          value={selected?.id ?? ""}
          onChange={(e) => {
            const s = snapshots.find((s) => s.id === e.target.value);
            if (s) onSnapshot(s);
          }}
        >
          <option value="" disabled>
            选择已发布快照
          </option>
          {snapshots.map((s) => (
            <option key={s.id} value={s.id}>
              {s.manifest.contracts.join(", ")} · {s.id.slice(0, 8)}
            </option>
          ))}
        </select>
      </div>
      <div className="panel-scroll">
        <table className="data-table market-quotes">
          <thead>
            <tr>
              <th>合约</th>
              <th className="numeric">收盘</th>
              <th className="numeric">开盘</th>
              <th className="numeric">最高</th>
              <th className="numeric">最低</th>
              <th className="numeric">成交量</th>
            </tr>
          </thead>
          <tbody>
            {rows.map((b) => (
              <tr
                key={b.contract}
                className={contract === b.contract ? "selected" : ""}
              >
                <td>
                  <button
                    className="text-button"
                    onClick={() => onContract(b.contract)}
                  >
                    {b.contract}
                  </button>
                </td>
                {(["close", "open", "high", "low", "volume"] as const).map(
                  (f) => (
                    <td className="numeric" key={f}>
                      {Number(b[f]).toLocaleString("zh-CN", {
                        maximumFractionDigits: 4,
                      })}
                    </td>
                  ),
                )}
              </tr>
            ))}
          </tbody>
        </table>
        {!rows.length && (
          <div className="table-empty">
            {selected ? "暂无匹配行情" : "选择快照以查看合约行情"}
          </div>
        )}
      </div>
      <div className="panel-footnote">
        {rows.length} 个合约 · 当前快照最多加载 1,000 行
      </div>
    </aside>
  );
}
