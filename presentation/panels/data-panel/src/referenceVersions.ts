import type { RequestClient } from "@asterion/runtime-client/requests";

export type ReferenceVersion = {
  id: string;
  dataset_id: string;
  created_at: number;
  archived?: boolean;
  manifest: {
    demo?: boolean;
    first?: string;
    last?: string;
    scope: { exchange?: string; connection_id?: string };
  };
};
type Page = { items: ReferenceVersion[]; total: number };

export async function referenceVersions(
  api: RequestClient,
  type: string,
  source: string,
  connection: string | undefined,
  exchange: string,
  active: () => boolean,
) {
  const items: ReferenceVersion[] = [];
  for (let offset = 0; active(); offset += 100) {
    const page = await api.request<Page>(
      `/data/catalog?type_id=${type}&layer=STANDARD&source=${encodeURIComponent(connection ?? source)}&limit=100&offset=${offset}`,
    );
    for (const dataset of page.items) {
      if (
        dataset.manifest.scope.exchange !== exchange ||
        dataset.manifest.scope.connection_id !== connection ||
        dataset.manifest.demo
      )
        continue;
      for (let offset = 0; active(); offset += 100) {
        const history = await api.request<Page>(
          `/data/catalog/${dataset.dataset_id}/versions?limit=100&offset=${offset}`,
        );
        items.push(
          ...history.items.filter(
            (item) => !item.archived && !item.manifest.demo,
          ),
        );
        if (offset + 100 >= history.total) break;
      }
    }
    if (offset + 100 >= page.total) break;
  }
  return items.sort((a, b) => b.created_at - a.created_at);
}
