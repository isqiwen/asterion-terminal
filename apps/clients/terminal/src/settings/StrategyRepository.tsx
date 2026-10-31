import { useState } from "react";
import type { CommandCall, CommandHandler } from "../bridge/commands";
import type { Snapshot } from "../bridge/client";
import { nativeDesktop, open } from "../bridge/desktop";
import { translate } from "../i18n";
import { ErrorNotice, asDisplayError, type DisplayError } from "../i18n/errors";
import { SettingsSection } from "./Section";
const t = (key: string) => translate("host", key);
// Where the user's strategies live: a Git repository of their own. The
// Terminal keeps the directory; lab reads the strategies from it.
export function StrategyRepository({
  snapshot,
  busy,
  trade,
}: {
  snapshot: Snapshot | null;
  busy: boolean;
  trade: CommandHandler;
}) {
  const saved = snapshot?.strategy_repository;
  const path = saved && "path" in saved ? saved.path : "";
  const [typed, setTyped] = useState("");
  const [error, setError] = useState<DisplayError>("");
  const run = async (...args: CommandCall) => {
    setError("");
    try {
      await trade(...args);
    } catch (reason) {
      setError(asDisplayError(reason));
    }
  };
  const choose = async () => {
    const chosen = await open({ directory: true });
    if (typeof chosen === "string") await run("research.repository.save", { path: chosen });
  };
  return (
    <SettingsSection
      title={t("策略仓库")}
      description={t(
        "你自己的 Git 仓库，策略写在其中 strategies/ 目录下的模块里。研究页从这里列出策略，运行时记下提交号和未提交的改动。",
      )}
      actions={
        nativeDesktop && (
          <button
            className={path ? undefined : "primary"}
            disabled={busy}
            onClick={() => void choose()}
          >
            {t(path ? "更换目录" : "选择目录")}
          </button>
        )
      }
    >
      {saved && "error" in saved && (
        <p role="alert">{t("研究设置文件无法读取，已保留原文件供检查；重新选择会覆盖它。")}</p>
      )}
      {path && (
        <div className="settings-item-row">
          <p className="settings-hint">
            <code>{path}</code>
          </p>
          <div className="settings-actions">
            <button disabled={busy} onClick={() => void run("research.repository.clear", {})}>
              {t("清除")}
            </button>
          </div>
        </div>
      )}
      {!nativeDesktop && (
        <div className="settings-item-row">
          <input
            aria-label={t("策略仓库目录")}
            placeholder="/Users/me/strategies"
            value={typed}
            onChange={event => setTyped(event.target.value)}
          />
          <button
            disabled={busy || !typed}
            onClick={() => void run("research.repository.save", { path: typed })}
          >
            {t("保存")}
          </button>
        </div>
      )}
      <ErrorNotice error={error} />
    </SettingsSection>
  );
}
