import { ServiceUnset } from "../../src/host/components/ServiceUnset";
import type { TerminalContext } from "../../src/host/feature-registry";
import { useMarketOpenError } from "../../src/host/market";
import { type MessageValues, translate } from "../../src/i18n";
import { notify } from "../../src/host/notifications";
const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.futures-market", key, values);

// What a live market view shows before it has a Market connection, and the one step to get one.
export function MarketUnavailable({ context }: { context: TerminalContext }) {
  const saved = context.snapshot?.market_endpoint;
  const endpoint = saved && "host" in saved ? saved : null;
  const openError = useMarketOpenError();
  if (!endpoint) return <ServiceUnset openSettings={context.openSettings} />;
  if (openError) return null;
  return (
    <div className="workspace-empty">
      <strong>{t("尚未连接行情服务")}</strong>
      <button
        className="primary"
        disabled={context.busy}
        onClick={() =>
          void context
            .trade("market.open")
            .catch(error => notify(error, { namespace: "asterion.terminal.futures-market" }))
        }
      >
        {t("连接行情服务")}
      </button>
    </div>
  );
}
