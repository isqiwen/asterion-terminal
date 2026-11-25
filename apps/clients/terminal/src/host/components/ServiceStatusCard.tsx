import { translate, useLocale } from "../../i18n";
import { ErrorNotice, type DisplayError } from "../../i18n/errors";
import { StatusBadge } from "../../ui/StatusBadge";

export type RemoteService = "market" | "warehouse" | "trading";

const services = {
  market: {
    title: "行情服务",
    loading: "正在读取行情服务…",
    settings: "行情服务设置",
    namespace: "asterion.terminal.futures-market",
  },
  warehouse: {
    title: "数据服务",
    loading: "正在读取数据服务…",
    settings: "数据服务设置",
    namespace: "asterion.terminal.data-workbench",
  },
  trading: {
    title: "交易服务",
    loading: "正在读取交易服务…",
    settings: "交易服务设置",
    namespace: "asterion.terminal.trading",
  },
} as const;

// Centered connection card shared by Market, Warehouse, and Trading when no cached view exists.
export function ServiceStatusCard({
  service,
  title,
  address,
  failed,
  openSettings,
}: {
  service: RemoteService;
  title?: string;
  address: string;
  failed?: DisplayError;
  openSettings: (page: RemoteService) => void;
}) {
  useLocale();
  const t = (key: string) => translate("host", key);
  const spec = services[service];
  return (
    <div className="workspace-empty" role={failed ? "alert" : undefined}>
      <div className="service-status-card">
        <div className="service-status-head">
          <strong>{title ?? t(spec.title)}</strong>
          {failed && <StatusBadge tone="danger">{t("服务未连接")}</StatusBadge>}
        </div>
        <code className="service-status-address">{address}</code>
        {failed ? (
          <p>
            <ErrorNotice error={failed} namespace={spec.namespace} />
          </p>
        ) : (
          <p>{t(spec.loading)}</p>
        )}
        {failed && (
          <div className="workspace-empty-actions">
            <button type="button" className="primary" onClick={() => openSettings(service)}>
              {t(spec.settings)}
            </button>
          </div>
        )}
      </div>
    </div>
  );
}
