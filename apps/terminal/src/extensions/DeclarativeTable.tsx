export type TableDefinition = {
  id: string;
  title: string;
  columns: { key: string; label: string }[];
};
export function DeclarativeTable({ definition, rows }: {
  definition: TableDefinition;
  rows: Record<string, string | number | boolean | null>[];
}) {
  return <table aria-label={definition.title}>
    <thead><tr>{definition.columns.map((column) => <th key={column.key}>{column.label}</th>)}</tr></thead>
    <tbody>{rows.map((row, index) => <tr key={index}>{definition.columns.map((column) =>
      <td key={column.key}>{row[column.key] == null ? "—" : String(row[column.key])}</td>
    )}</tr>)}</tbody>
  </table>;
}
