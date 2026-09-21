import { widgets } from "./widgets";
import { nativeDesktop } from "../../deployment/desktop";
import { Chart } from "./Chart";

import type { TerminalPlugin } from "../../extensions/plugins";
import type { PanelContext } from "../workflow/context";
import type { SettingsContext } from "../workflow/settingsContext";
function Market(context: PanelContext["market"], active: boolean) {
  const {
    layout,
    setDock,
    saveViewport,
    preferences,
    bars,
    contractBars,
    contract,
    setContract,
    selected,
    last,
    openData,
    merge,
    popout,
  } = context;
  return (
    <>
      {layout.detached && (
        <div className="table-empty-state">
          <span>图表已在独立窗口打开</span>
          <button onClick={merge}>归并图表</button>
        </div>
      )}
      {!layout.detached && (
        <>
          <div className="panel-heading">
            <h2>历史 K 线</h2>
            <span className="panel-spacer" />
            {bars.length > 0 && (
              <select
                aria-label="合约"
                value={contract}
                onChange={(e) => setContract(e.target.value)}
              >
                {[...new Set(bars.map((b) => b.contract))].map((c) => (
                  <option key={c}>{c}</option>
                ))}
              </select>
            )}
            {layout.kind !== "chart" && (
              <select
                aria-label="行情面板停靠位置"
                value={layout.dock}
                onChange={(e) => setDock(e.target.value as typeof layout.dock)}
              >
                <option value="left">行情居左</option>
                <option value="right">行情居右</option>
                <option value="top">行情居上</option>
                <option value="bottom">行情居下</option>
              </select>
            )}
            <small>
              {selected?.manifest.demo === true
                ? "合成示例 · 非真实行情"
                : last
                  ? selected?.manifest.frequency === "1d"
                    ? "历史日线 · 按交易日标记"
                    : "历史行情"
                  : "未加载"}
            </small>
            {nativeDesktop &&
              (layout.kind === "chart" ? (
                <button onClick={merge}>归并图表</button>
              ) : (
                <button onClick={() => void popout("chart")}>拆出图表</button>
              ))}
          </div>
          <div className="quote-strip">
            <b className="mono">{contract || "—"}</b>
            {(["open", "high", "low", "close", "volume"] as const).map(
              (field, i) => (
                <span key={field}>
                  <small>{["开", "高", "低", "收", "量"][i]}</small>
                  <strong>
                    {last
                      ? Number(last[field]).toLocaleString("zh-CN", {
                          maximumFractionDigits: 4,
                        })
                      : "—"}
                  </strong>
                </span>
              ),
            )}
          </div>
          <div className="chart-panel">
            {contractBars.length ? (
              <Chart
                bars={contractBars}
                colors={preferences.colors}
                viewport={
                  layout.viewport?.snapshot === selected?.id &&
                  layout.viewport?.contract === contract
                    ? layout.viewport
                    : null
                }
                onViewport={saveViewport}
              />
            ) : (
              <div className="chart-placeholder">
                <div>
                  <span>⌁</span>
                  <h2>尚未选择行情</h2>
                  <p>同步或导入历史数据后选择合约查看走势</p>
                  <button
                    onClick={() =>
                      layout.kind === "chart" ? void merge() : openData()
                    }
                  >
                    {layout.kind === "chart" ? "返回原工作台" : "获取数据"}
                  </button>
                </div>
              </div>
            )}
          </div>
          <div className="panel-footnote">
            <span>
              {selected
                ? `${selected.manifest.start} — ${selected.manifest.end}`
                : "数据时间 —"}
            </span>
            <span className="panel-spacer" />
            <span>
              {bars.length
                ? `${bars.length} 行 · 最多展示 1,000 行`
                : "历史数据"}
            </span>
          </div>
        </>
      )}
    </>
  );
}

export const plugin: TerminalPlugin<PanelContext, SettingsContext> = {
  apiVersion: 1,
  extensions: widgets,
  id: "asterion.market",
  requires: ["asterion.data", "asterion.overview", "asterion.connections"],
  workspaces: [
    {
      id: "workspace.market",
      objectTools: true,
      title: "市场",
      icon: "⌁",
      sections: [
        { title: "行情全景", panel: "market.chart" },
        { title: "历史图表", panel: "market.chart" },
      ],
    },
  ],
  panels: [{ id: "market.chart", scope: "market", render: Market }],
};
