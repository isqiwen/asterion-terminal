import { translate, useLocale } from "../../i18n";
import { BrandMark } from "../../ui/BrandMark";

const labels = {
  market: "连接行情服务",
  warehouse: "连接数据服务",
  trading: "添加交易连接",
} as const;

// What a page that reads a service shows before one is set, and the way to set it.
export function ServiceUnset({
  openSettings,
  page = "market",
}: {
  openSettings: (page: "market" | "warehouse" | "trading") => void;
  page?: "market" | "warehouse" | "trading";
}) {
  useLocale();
  const t = (key: string) => translate("host", key);
  return (
    <div className="workspace-empty">
      <BrandMark size={36} />
      <button type="button" className="primary" onClick={() => openSettings(page)}>
        {t(labels[page])}
      </button>
    </div>
  );
}
