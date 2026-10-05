import { useEffect, useRef, useState } from "react";
import type { HistoryDatasetRecord, Snapshot } from "../bridge/client";
import { asDisplayError, type DisplayError } from "../i18n/errors";
export type HistoryQuery = (
  method: "data.datasets" | "data.dataset.saved",
  params: Record<string, unknown>,
) => Promise<Snapshot>;
export function useHistoryDatasets(snapshot: Snapshot | null, query: HistoryQuery) {
  const ref = useRef(query);
  ref.current = query;
  const connection = snapshot?.data?.connection_id;
  const online = !!snapshot?.data?.online;
  const completed = (snapshot?.task_service?.tasks ?? [])
    .filter(task => task.history_dataset_id)
    .map(task => task.history_dataset_id)
    .join(",");
  const [state, setState] = useState<{
    connection?: string;
    items: HistoryDatasetRecord[];
    error: DisplayError;
    loading: boolean;
  }>({ items: [], error: "", loading: true });
  useEffect(() => {
    let active = true;
    setState({ connection, items: [], error: "", loading: online });
    if (online)
      void ref
        .current("data.datasets", { venue: "", product: "", contract_id: "", source: "" })
        .then(response => {
          if (active)
            setState({
              connection,
              items: response.history_datasets ?? [],
              error: "",
              loading: false,
            });
        })
        .catch(error => {
          if (active)
            setState({ connection, items: [], error: asDisplayError(error), loading: false });
        });
    return () => {
      active = false;
    };
  }, [connection, online, completed]);
  return state.connection === connection ? state : { items: [], error: "", loading: true };
}
