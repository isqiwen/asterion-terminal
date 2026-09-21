import type { Snapshot } from "./client";
export function SnapshotTable({
  snapshots,
  selected,
  onSelect,
}: {
  snapshots: Snapshot[];
  selected?: string;
  onSelect: (snapshot: Snapshot) => void;
}) {
  return (
    <table className="data-table">
      <thead>
        <tr>
          <th>合约</th>
          <th>来源</th>
          <th className="numeric">行数</th>
          <th>版本</th>
        </tr>
      </thead>
      <tbody>
        {snapshots.map((s) => (
          <tr key={s.id} className={selected === s.id ? "selected" : ""}>
            <td>
              <button className="text-button" onClick={() => onSelect(s)}>
                {s.manifest.contracts.join(", ")}
              </button>
            </td>
            <td title={s.manifest.source}>{s.manifest.source}</td>
            <td className="numeric">{s.manifest.rows.toLocaleString()}</td>
            <td className="mono">{s.id.slice(0, 8)}</td>
          </tr>
        ))}
      </tbody>
    </table>
  );
}
