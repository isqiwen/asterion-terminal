import { desktop, nativeDesktop } from "../../bridge/desktop";
import { translate, useLocale } from "../../i18n";
import { ActionError, useActionError } from "../action-error";

export function LocalLogsButton() {
  useLocale();
  const { error, attempt } = useActionError();
  if (!nativeDesktop) return null;
  return (
    <span>
      <button type="button" onClick={() => void attempt(() => desktop().openLogs())}>
        {translate("host", "打开本机日志目录")}
      </button>
      <ActionError error={error} />
    </span>
  );
}
