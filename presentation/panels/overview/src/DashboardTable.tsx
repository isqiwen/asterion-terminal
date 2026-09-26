import type { ReactNode } from "react";

/** Bounded, keyboard-scrollable table for dashboard contributions. */
export function DashboardTable({ label, columns, rows, empty }: {
  label: string;
  columns: readonly { title: string; numeric?: boolean }[];
  rows: readonly { id: string; cells: readonly ReactNode[] }[];
  empty: string;
}) {
  return <div className="dashboard-table" tabIndex={0} role="region" aria-label={`${label}滚动区`}>
    <table className="data-table" aria-label={label}>
      <thead><tr>{columns.map(c => <th key={c.title} className={c.numeric ? "numeric" : undefined}>{c.title}</th>)}</tr></thead>
      <tbody>{rows.length ? rows.map(row => <tr key={row.id}>{row.cells.map((cell, i) => <td key={columns[i].title} className={columns[i].numeric ? "numeric" : undefined}>{cell}</td>)}</tr>) : <tr><td colSpan={columns.length} className="positions-unavailable">{empty}</td></tr>}</tbody>
    </table>
  </div>;
}
