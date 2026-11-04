import { useState } from "react";
import { translate } from "../i18n";
const t = (key: string) => translate("host", key);

// This Terminal's certificate fingerprint, selectable, with a copy button.
export function ClientFingerprint({ value }: { value?: string }) {
  const [copied, setCopied] = useState(false);
  if (!value) return null;
  return (
    <span className="client-fingerprint">
      <code>{value}</code>
      <button
        type="button"
        onClick={() => void navigator.clipboard.writeText(value).then(() => setCopied(true))}
      >
        {t(copied ? "已复制" : "复制")}
      </button>
    </span>
  );
}
