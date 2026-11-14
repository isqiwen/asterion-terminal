import { ActionError, useActionError } from "../../src/host/action-error";
import { ServiceUnset } from "../../src/host/components/ServiceUnset";
import type { TerminalContext } from "../../src/host/feature-registry";
import { useMarketOpenError } from "../../src/host/market";
import { type MessageValues, translate } from "../../src/i18n";
const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.futures-market", key, values);

// What a live market view shows before it has a Market connection, and the one step to get one.
export function MarketUnavailable({ context }: { context: TerminalContext }) {
  const saved = context.snapshot?.market_endpoint;
  const endpoint = saved && "host" in saved ? saved : null;
  const { error, attempt } = useActionError();
  const openError = useMarketOpenError();
  if (!endpoint) return <ServiceUnset openSettings={context.openSettings} />;
  return (
    <div className="workspace-empty">
      <strong>{t("尚未连接行情服务")}</strong>
      <p>
        {endpoint.host}:{endpoint.port}
      </p>
      <button
        className="primary"
        disabled={context.busy}
        onClick={() => void attempt(() => context.trade("market.open"))}
      >
        {t("连接行情服务")}
      </button>
      <ActionError error={error || openError} namespace="asterion.terminal.futures-market" />
    </div>
  );
}
