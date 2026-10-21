import { useState } from "react";
import { desktop, nativeDesktop } from "../../bridge/desktop";
import { translate, useLocale } from "../../i18n";
import { asDisplayError, ErrorNotice, type DisplayError } from "../../i18n/errors";

export function LocalLogsButton() {
  useLocale();
  const [error, setError] = useState<DisplayError>("");
  if (!nativeDesktop) return null;
  const open = async () => {
    setError("");
    try {
      await desktop().openLogs();
    } catch (failure) {
      setError(asDisplayError(failure));
    }
  };
  return (
    <span>
      <button type="button" onClick={() => void open()}>
        {translate("host", "打开本机日志目录")}
      </button>
      {error && (
        <span role="alert">
          <ErrorNotice error={error} />
        </span>
      )}
    </span>
  );
}
