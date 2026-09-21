import { useEffect, useState, type ReactNode } from "react";
import { ContributionRegistry } from "./registry";

export type Panel<C> = {
  [K in keyof C]: {
    id: string;
    scope: K;
    keepMounted?: boolean;
    render: (context: C[K], active: boolean) => ReactNode;
  };
}[keyof C];

function renderPanel<C>(panel: Panel<C>, contexts: C, active: boolean) {
  if (!Object.prototype.hasOwnProperty.call(contexts, panel.scope))
    throw new Error(`Missing panel scope: ${String(panel.scope)}`);
  // The discriminated contribution binds its renderer to this exact scope.
  const render = panel.render as (
    context: C[keyof C],
    active: boolean,
  ) => ReactNode;
  return render(contexts[panel.scope], active);
}
export function PanelHost<C>({
  registry,
  activeId,
  context,
}: {
  registry: ContributionRegistry<Panel<C>>;
  activeId: string;
  context: C;
}) {
  registry.get(activeId);
  const [visited, setVisited] = useState<ReadonlySet<string>>(new Set());
  useEffect(() => {
    setVisited((previous) =>
      previous.has(activeId) ? previous : new Set([...previous, activeId]),
    );
  }, [activeId]);
  return registry.all().map((panel) => {
    const active = panel.id === activeId;
    if (!active && !(panel.keepMounted && visited.has(panel.id))) return null;
    return (
      <div
        key={panel.id}
        hidden={!active}
        style={{ display: active ? "contents" : "none" }}
      >
        {renderPanel(panel, context, active)}
      </div>
    );
  });
}
