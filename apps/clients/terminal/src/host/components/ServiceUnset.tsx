import { translate, useLocale } from "../../i18n";

// What a page that reads a service shows before one is set, and the way to set it.
export function ServiceUnset({
  openSettings,
  page = "market",
}: {
  openSettings: (page: "market" | "warehouse") => void;
  page?: "market" | "warehouse";
}) {
  useLocale();
  const t = (key: string) => translate("host", key);
  return (
    <div className="workspace-empty">
      <strong>{t(page === "market" ? "尚未设置行情服务" : "尚未设置数据服务")}</strong>
      <button className="primary" onClick={() => openSettings(page)}>
        {t(page === "market" ? "添加行情连接" : "添加数据连接")}
      </button>
    </div>
  );
}
