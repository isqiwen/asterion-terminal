import { desktop, nativeDesktop } from "../../bridge/desktop";
import { translate, useLocale } from "../../i18n";
import { notify } from "../notifications";

export function LocalLogsButton() {
  useLocale();
  if (!nativeDesktop) return null;
  const open = async () => {
    try {
      await desktop().openLogs();
    } catch (failure) {
      notify(failure);
    }
  };
  return (
    <span>
      <button type="button" onClick={() => void open()}>
        {translate("host", "打开本机日志目录")}
      </button>
    </span>
  );
}
