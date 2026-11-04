import { translate, useLocale } from "../../i18n";

// What a page that reads Market shows before one is set, and the way to set it.
export function MarketUnset({ openSettings }: { openSettings: (page: "sources") => void }) {
  useLocale();
  const t = (key: string) => translate("host", key);
  return (
    <div className="workspace-empty">
      <strong>{t("尚未设置行情服务")}</strong>
      <p>{t("在设置里填写 Market 的地址和证书指纹后即可使用。")}</p>
      <button className="primary" onClick={() => openSettings("sources")}>
        {t("配置行情服务")}
      </button>
    </div>
  );
}
