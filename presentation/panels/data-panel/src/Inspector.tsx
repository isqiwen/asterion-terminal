import type { Snapshot } from "./client";
export function Inspector({
  snapshot,
  onClose,
}: {
  snapshot?: Snapshot;
  onClose: () => void;
}) {
  return (
    <aside className="inspector" aria-label="对象检查器">
      <div className="panel-heading">
        <h2>检查器</h2>
        <span className="panel-spacer" />
        <button aria-label="关闭检查器" onClick={onClose}>
          ×
        </button>
      </div>
      {snapshot ? (
        <div className="inspector-details">
          <h3>{snapshot.manifest.contracts.join(", ")}</h3>
          <span className="good">PUBLISHED</span>
          <dl>
            <dt>来源</dt>
            <dd>{snapshot.manifest.source}</dd>
            <dt>行数</dt>
            <dd>{snapshot.manifest.rows.toLocaleString()}</dd>
            <dt>起始时间</dt>
            <dd>{snapshot.manifest.start}</dd>
            <dt>结束时间</dt>
            <dd>{snapshot.manifest.end}</dd>
            <dt>快照版本</dt>
            <dd className="mono">{snapshot.id}</dd>
          </dl>
          <details>
            <summary>校验信息</summary>
            <dl>
              <dt>SHA-256</dt>
              <dd className="mono">{snapshot.manifest.checksum}</dd>
              <dt>数据引用</dt>
              <dd className="mono">{snapshot.manifest.uri}</dd>
            </dl>
          </details>
        </div>
      ) : (
        <div className="panel-empty">
          <span>未选择对象</span>
          <small>选择合约或快照查看详情</small>
        </div>
      )}
    </aside>
  );
}
