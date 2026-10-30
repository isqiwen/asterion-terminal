import { useState } from "react";
import type { CommandCall, CommandHandler } from "../bridge/commands";
import type { Snapshot } from "../bridge/client";
import { translate } from "../i18n";
import { ErrorNotice, asDisplayError, type DisplayError } from "../i18n/errors";
import { SettingsSection } from "./Section";
const t = (key: string) => translate("host", key);
// The recording host is a data source like any other: lab reads the ticks and
// minutes it recorded. It logs in to the broker on its own host; the Terminal
// only keeps where its read-only interface listens.
export function RecorderSource({
  snapshot,
  busy,
  trade,
}: {
  snapshot: Snapshot | null;
  busy: boolean;
  trade: CommandHandler;
}) {
  const saved = snapshot?.recorder_endpoint;
  const endpoint = saved && "host" in saved ? saved : null;
  const [editing, setEditing] = useState(false);
  const [host, setHost] = useState("");
  const [port, setPort] = useState("7450");
  const [clearing, setClearing] = useState(false);
  const [error, setError] = useState<DisplayError>("");
  const run = async (...args: CommandCall) => {
    setError("");
    try {
      await trade(...args);
      return true;
    } catch (reason) {
      setError(asDisplayError(reason));
      return false;
    }
  };
  const edit = () => {
    setHost(endpoint?.host ?? "");
    setPort(String(endpoint?.port ?? 7450));
    setClearing(false);
    setEditing(true);
  };
  return (
    <SettingsSection
      title={t("行情录制主机")}
      description={t(
        "录制主机自己登录行情，按交易日保存全市场 Tick 与分钟线；这里只填它的只读接口地址，在数据页读取录制数据时使用。",
      )}
      actions={
        !editing && (
          <button className={endpoint ? undefined : "primary"} disabled={busy} onClick={edit}>
            {t(endpoint ? "修改地址" : "填写地址")}
          </button>
        )
      }
    >
      {saved && "error" in saved && (
        <p role="alert">{t("录制主机设置文件无法读取，已保留原文件供检查；重新填写会覆盖它。")}</p>
      )}
      {!editing &&
        (endpoint ? (
          <div className="settings-item-row">
            <p className="settings-hint">
              {t("地址")} <code>{`${endpoint.host}:${endpoint.port}`}</code>
            </p>
            <div className="settings-actions">
              <button
                className={clearing ? "danger" : undefined}
                disabled={busy}
                onClick={async () => {
                  if (!clearing) {
                    setClearing(true);
                    return;
                  }
                  if (await run("recorder.endpoint.clear", {})) setClearing(false);
                }}
              >
                {t(clearing ? "确认清除" : "清除")}
              </button>
            </div>
          </div>
        ) : (
          !(saved && "error" in saved) && <p className="settings-hint">{t("尚未填写录制主机。")}</p>
        ))}
      {editing && (
        <form
          onSubmit={async event => {
            event.preventDefault();
            if (await run("recorder.endpoint.save", { host: host.trim(), port: Number(port) }))
              setEditing(false);
          }}
        >
          <fieldset disabled={busy}>
            <div className="settings-fields">
              <label className="settings-field">
                {t("主机地址")}
                <input
                  aria-label={t("主机地址")}
                  required
                  maxLength={253}
                  placeholder="192.168.1.20"
                  value={host}
                  onChange={event => setHost(event.target.value)}
                />
              </label>
              <label className="settings-field">
                {t("端口")}
                <input
                  aria-label={t("端口")}
                  type="number"
                  required
                  min={1}
                  max={65535}
                  value={port}
                  onChange={event => setPort(event.target.value)}
                />
              </label>
            </div>
            <div className="settings-actions">
              <button type="button" onClick={() => setEditing(false)}>
                {t("取消")}
              </button>
              <button className="primary">{t("保存地址")}</button>
            </div>
          </fieldset>
        </form>
      )}
      {error && (
        <p className="alert" role="alert">
          <ErrorNotice error={error} />
        </p>
      )}
    </SettingsSection>
  );
}
