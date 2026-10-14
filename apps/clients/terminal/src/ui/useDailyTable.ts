import { useEffect, useRef, useState } from "react";
import type { Snapshot, TableCoverage, TableCoverageRow } from "../bridge/client";
import { asDisplayError, type DisplayError } from "../i18n/errors";

// Where a dataset reads settlement prices, trading days, open interest and
// units: the market-wide daily table, its calendar and its contract list,
// under the snapshot the user saw.
export type DailyTable = {
  snapshot: string;
  daily_source: string;
  calendar_source: string;
  contracts_source: string;
};
export type DailyTableQuery = (
  method: "data.tables.coverage",
  params: Record<string, unknown>,
) => Promise<Snapshot>;

// The daily table that holds data, and the calendar and contract list of the
// same data source, with what the table holds of each exchange.
export function dailyTableOf(
  coverage: TableCoverage | undefined,
): { table: DailyTable; stored: TableCoverageRow[] } | null {
  const daily = coverage?.sources.find(
    source =>
      source.table === "futures.daily" &&
      coverage.rows.some(row => row.table === source.table && row.source === source.source),
  );
  const of = (table: string) =>
    coverage?.sources.find(
      source => source.table === table && source.plugin_id === daily?.plugin_id,
    );
  const calendar = of("futures.calendar");
  const contracts = of("futures.contracts");
  if (!coverage?.snapshot || !daily || !calendar || !contracts) return null;
  return {
    table: {
      snapshot: coverage.snapshot,
      daily_source: daily.source,
      calendar_source: calendar.source,
      contracts_source: contracts.source,
    },
    stored: coverage.rows.filter(
      row => row.table === "futures.daily" && row.source === daily.source,
    ),
  };
}

// Read again when the data service changes or a table update finishes; the
// snapshot read stays what it was, whatever is stored after it.
export function useDailyTable(snapshot: Snapshot | null, query: DailyTableQuery) {
  const ref = useRef(query);
  ref.current = query;
  const connection = snapshot?.data?.connection_id;
  const online = !!snapshot?.data?.online;
  const updated = (snapshot?.task_service?.tasks ?? []).filter(
    task => task.kind === "table_update" && task.state === "succeeded",
  ).length;
  const [state, setState] = useState<{
    connection?: string;
    held: ReturnType<typeof dailyTableOf>;
    error: DisplayError;
    loading: boolean;
  }>({ held: null, error: "", loading: true });
  useEffect(() => {
    let active = true;
    setState({ connection, held: null, error: "", loading: online });
    if (online)
      void ref
        .current("data.tables.coverage", {})
        .then(response => {
          if (active)
            setState({
              connection,
              held: dailyTableOf(response.table_coverage),
              error: "",
              loading: false,
            });
        })
        .catch(error => {
          if (active)
            setState({ connection, held: null, error: asDisplayError(error), loading: false });
        });
    return () => {
      active = false;
    };
  }, [connection, online, updated]);
  return state.connection === connection ? state : { held: null, error: "", loading: true };
}
