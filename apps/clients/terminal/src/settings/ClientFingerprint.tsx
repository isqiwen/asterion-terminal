import { useState } from "react";
import { translate } from "../i18n";
const t = (key: string) => translate("host", key);

// The one certificate fingerprint this Terminal is known by on Market and trading hosts.
export function ClientFingerprint({ value }: { value?: string }) {
  const [copied, setCopied] = useState(false);
  if (!value) return null;
  return (
    <p className="settings-hint client-fingerprint">
      {t("本机证书指纹（Market 和交易主机登记同一个）")}
      <code>{value}</code>
      <button
        type="button"
        onClick={() => void navigator.clipboard.writeText(value).then(() => setCopied(true))}
      >
        {t(copied ? "已复制" : "复制")}
      </button>
    </p>
  );
}
