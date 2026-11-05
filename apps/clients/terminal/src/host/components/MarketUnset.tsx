import { translate, useLocale } from "../../i18n";

// What a page that reads Market shows before one is set, and the way to set it.
export function MarketUnset({ openSettings }: { openSettings: (page: "market") => void }) {
  useLocale();
  const t = (key: string) => translate("host", key);
  return (
    <div className="workspace-empty">
      <strong>{t("尚未设置行情服务")}</strong>
      <p>{t("在设置里填写行情服务的地址和证书指纹后即可使用。")}</p>
      <button className="primary" onClick={() => openSettings("market")}>
        {t("管理行情连接")}
      </button>
    </div>
  );
}
