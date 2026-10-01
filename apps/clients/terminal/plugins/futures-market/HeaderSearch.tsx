import { translate, useWorkspaceDraft, type TerminalContext } from "../contract";
const t = (key: string) => translate("asterion.terminal.futures-market", key);
export function HeaderSearch(context: TerminalContext & { target?: string }) {
  const [search, setSearch] = useWorkspaceDraft("quote-search", "");
  const label = t(context.target ? "搜索自选合约" : "搜索市场合约");
  return (
    <input
      type="search"
      className="terminal-contract-search"
      aria-label={label}
      placeholder={label}
      value={search}
      onChange={event => {
        setSearch(event.target.value);
        context.navigate(context.target ?? "workspace.market", { marketMode: "live" });
      }}
    />
  );
}
