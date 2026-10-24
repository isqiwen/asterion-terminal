import { useCallback, useEffect, useState } from "react";
import { registerLanguageResources, translate, useLocale } from "../i18n";
import zh from "../i18n/locales/installer.zh-CN.json";
import en from "../i18n/locales/installer.en-US.json";
import "./installer.css";
import { RemoteNodes, type RemoteRequest, type RemoteView } from "./RemoteNodes";
registerLanguageResources("installer", { "zh-CN": zh, "en-US": en });
const t = (key: string) => translate("installer", key);
type MaintenanceAction = "install" | "upgrade" | "repair" | "uninstall";
export type Action = MaintenanceAction | "stop";
export interface Installation {
  directory: string;
  node: string;
  enrollment: string;
  installed: boolean;
  version: string;
  candidate_version: string;
  source_available: boolean;
  deployment_action: "" | "install" | "upgrade" | "repair";
  busy: string[];
  conflict: string;
  pending: "" | MaintenanceAction;
  preserve_data: boolean;
}
export interface InstallerBridge {
  remote(input: RemoteRequest): Promise<unknown>;
  onRemoteProgress(listener: (view: RemoteView) => void): () => void;
  inspect(): Promise<Installation>;
  run(action: Action, removeUserData: boolean, remoteAll?: boolean): Promise<Installation>;
  open(location: "terminal" | "logs" | "data"): Promise<void>;
  onProgress(listener: (step: string) => void): () => void;
}
declare global {
  interface Window {
    asterionInstaller: InstallerBridge;
  }
}
const steps = ["checking", "copying", "publishing", "preparing", "registering", "complete"];
export function Installer({ bridge = window.asterionInstaller }: { bridge?: InstallerBridge }) {
  const { locale, setLocale } = useLocale();
  const [remoteProgress, setRemoteProgress] = useState<RemoteView>();
  const [view, setView] = useState<Installation>();
  const [action, setAction] = useState<MaintenanceAction>("install");
  const [working, setWorking] = useState(false);
  const [remoteChecked, setRemoteChecked] = useState(false);
  const [confirmed, setConfirmed] = useState(false);
  const [stage, setStage] = useState("");
  const [error, setError] = useState("");
  const [completed, setCompleted] = useState(false);
  const [preserveData, setPreserveData] = useState(true);
  const [remoteAll, setRemoteAll] = useState(false);
  const [removedData, setRemovedData] = useState(false);
  const failure = (error: unknown) =>
    setError(error instanceof Error ? error.message : String(error));
  const accept = useCallback((next: Installation) => {
    setView(next);
    setConfirmed(false);
    setPreserveData(next.preserve_data);
    setAction(next.pending || next.deployment_action || (next.installed ? "repair" : "install"));
  }, []);
  const refresh = useCallback(async () => {
    setRemoteChecked(false);
    setWorking(true);
    setError("");
    setCompleted(false);
    setStage("");
    try {
      const inspected = await bridge.inspect();
      accept(inspected);
      if (inspected.installed && !inspected.pending) {
        setRemoteProgress(
          (await bridge.remote({ maintenance: { action: "INSPECT" } })) as RemoteView,
        );
        accept(await bridge.inspect());
        setRemoteChecked(true);
      }
    } catch (error) {
      setError(error instanceof Error ? error.message : String(error));
    } finally {
      setWorking(false);
    }
  }, [bridge, accept]);
  useEffect(() => {
    void refresh();
    const off = bridge.onProgress(setStage);
    const offRemote = bridge.onRemoteProgress(setRemoteProgress);
    return () => {
      off();
      offRemote();
    };
  }, [bridge, refresh]);
  async function run(selected: Action) {
    setRemoteChecked(false);
    const removeUserData = selected === "uninstall" && !preserveData;
    setWorking(true);
    setError("");
    setCompleted(false);
    setStage("checking");
    try {
      accept(
        await (selected === "uninstall" && remoteAll
          ? bridge.run(selected, removeUserData, true)
          : bridge.run(selected, removeUserData)),
      );
      setCompleted(selected !== "stop");
      setRemovedData(removeUserData);
      setStage("");
    } catch (error) {
      failure(error);
    } finally {
      setWorking(false);
    }
  }
  const changing = action !== "uninstall";
  const unavailable = (selected: MaintenanceAction) =>
    !view ||
    !!view.conflict ||
    (!view.pending &&
      (selected === "upgrade" || (selected === "uninstall" && remoteAll)) &&
      !remoteChecked) ||
    view.busy.some(
      name =>
        name === "terminal" || (selected === "uninstall" && !remoteAll) || selected === "install",
    ) ||
    (selected === "uninstall"
      ? !view.installed || (!!view.pending && view.pending !== "uninstall")
      : selected !== (view.pending || view.deployment_action));
  const blocked = unavailable(action);
  const mode = view?.pending
    ? "resumeMode"
    : view?.deployment_action === "upgrade"
      ? "upgradeMode"
      : view?.installed
        ? "maintenanceMode"
        : "installMode";
  const operations: MaintenanceAction[] = view?.pending
    ? [view.pending]
    : view?.installed
      ? [view.deployment_action === "upgrade" ? "upgrade" : "repair", "uninstall"]
      : ["install"];
  const olderPackage =
    view?.installed &&
    view.source_available &&
    !view.pending &&
    !view.conflict &&
    !view.deployment_action;
  const timeline =
    action === "uninstall"
      ? [
          "checking",
          "unregistering",
          "removing",
          ...(!preserveData ? ["removingData"] : []),
          "complete",
        ]
      : action === "install" || action === "upgrade"
        ? [...steps.slice(0, -1), "starting", "complete"]
        : steps;
  return (
    <div className="installer-shell">
      <aside className="installer-sidebar">
        <div className="installer-brand">
          <span className="installer-mark">A</span>
          <div>
            ASTERION<small>{t("maintenance")}</small>
          </div>
        </div>
        <p className="installer-kicker">TERMINAL / FUTURES</p>
        <h1>{t(mode)}</h1>
        <p>{t(mode + "Description")}</p>
        <nav aria-label={t("operation")}>
          {operations.map((item, index) => (
            <button
              key={item}
              aria-pressed={action === item}
              disabled={working || unavailable(item)}
              onClick={() => {
                setAction(item);
                setConfirmed(false);
                setPreserveData(view?.pending ? view.preserve_data : true);
                setCompleted(false);
                setStage("");
              }}
            >
              <span>0{index + 1}</span>
              {t(item)}
              <span className="installer-arrow">→</span>
            </button>
          ))}
        </nav>
        <div className="installer-scope">
          <strong>{t("userOnly")}</strong>
          <p>{t("scope")}</p>
        </div>
      </aside>
      <main className="installer-main">
        <header>
          <span>{t("workspace")}</span>
          <select
            aria-label={t("language")}
            value={locale}
            disabled={working}
            onChange={event => setLocale(event.target.value as "zh-CN" | "en-US")}
          >
            <option value="zh-CN">简体中文</option>
            <option value="en-US">English</option>
          </select>
        </header>
        <section className="installer-heading">
          <div>
            <p className="installer-kicker">{t("operation")}</p>
            <h2>{t(action)}</h2>
          </div>
          <span className="installer-badge">
            {view
              ? t(view.pending ? "resumeMode" : view.installed ? "installed" : "notInstalled")
              : t("checking")}
          </span>
        </section>
        <p>{t(action + "Description")}</p>
        <section className="installer-card" aria-label={t("installationInfo")}>
          <div className="installer-version">
            <div>
              <small>{t(view?.pending ? "registeredVersion" : "currentVersion")}</small>
              <strong>{view?.version || "—"}</strong>
            </div>
            <span>→</span>
            <div>
              <small>{t("packageVersion")}</small>
              <strong>{view?.candidate_version || "—"}</strong>
            </div>
          </div>
          <dl>
            <dt>{t("applicationPath")}</dt>
            <dd>{view?.directory || "—"}</dd>
            <dt>{t("dataPath")}</dt>
            <dd>{view?.node || "—"}</dd>
          </dl>
        </section>
        {view?.busy.length ? (
          <section className="installer-notice">
            <strong>
              {t("running")}: {view.busy.join(" · ")}
            </strong>
            <p>{t("stopInstructions")}</p>
            <div className="installer-inline">
              <button
                disabled={working || view.busy.includes("terminal")}
                onClick={() => void run("stop")}
              >
                {t("stopBackground")}
              </button>
            </div>
          </section>
        ) : null}
        {view?.pending && (
          <p role="status">
            {t("pending")}: {t(view.pending)}
          </p>
        )}
        {view?.conflict && (
          <p role="alert">
            {t("conflict")}
            <code>{view.conflict}</code>
          </p>
        )}
        {view && changing && !view.source_available && <p role="alert">{t("noPackage")}</p>}
        {olderPackage && (
          <p role="status" className="installer-notice">
            {t("olderPackage")}
          </p>
        )}
        {action === "uninstall" && (
          <label className="installer-confirm">
            <input
              type="checkbox"
              checked={remoteAll}
              disabled={working || !!view?.pending}
              onChange={event => {
                setRemoteAll(event.target.checked);
                setConfirmed(false);
              }}
            />
            {t("remoteAll")}
          </label>
        )}
        <div className="installer-preserve">
          {action === "uninstall" ? (
            <label className="installer-confirm">
              <input
                type="checkbox"
                checked={preserveData}
                disabled={working || blocked || !!view?.pending}
                onChange={event => {
                  setPreserveData(event.target.checked);
                  setConfirmed(false);
                  setCompleted(false);
                }}
              />
              {t("preserveOption")}
            </label>
          ) : (
            <strong>{t("preserveTitle")}</strong>
          )}
          {action === "uninstall" && !preserveData ? (
            <div className="installer-error" role="alert">
              <strong>{t("deleteTitle")}</strong>
              <p>{t("deleteBody")}</p>
              <code>{view?.node}</code>
              <code>{view?.enrollment}</code>
              <p>{t("deleteExclusions")}</p>
            </div>
          ) : (
            <p>{t("preserveBody")}</p>
          )}
        </div>
        {(working || stage) && (
          <ol className="installer-progress" aria-label={t("progress")} aria-live="polite">
            {timeline.map(item => (
              <li
                key={item}
                data-active={item === stage}
                data-done={timeline.indexOf(item) < timeline.indexOf(stage)}
              >
                {t(item)}
              </li>
            ))}
          </ol>
        )}
        {error && (
          <div className="installer-error" role="alert">
            <strong>{t("failed")}</strong>
            <p>
              {t(action === "uninstall" && !preserveData ? "failureDeleteHelp" : "failureHelp")}
            </p>
            <pre>{error}</pre>
            <button
              disabled={working || !view}
              onClick={() => void bridge.open("logs").catch(failure)}
            >
              {t("openLogs")}
            </button>
          </div>
        )}
        {!working &&
          !view?.pending &&
          (action === "upgrade" || (action === "uninstall" && remoteAll)) &&
          !remoteChecked && <p role="status">{t("remoteInspectionRequired")}</p>}
        {completed && !error && (
          <p role="status" className="installer-success">
            {t(removedData ? "successRemovedData" : "success")}
          </p>
        )}
        {view?.installed && !view.pending && (
          <RemoteNodes
            request={input => bridge.remote(input)}
            progress={remoteProgress}
            busy={working}
            setBusy={setWorking}
          />
        )}
        <footer>
          <label className="installer-confirm">
            <input
              type="checkbox"
              checked={confirmed}
              disabled={working || blocked}
              onChange={event => setConfirmed(event.target.checked)}
            />
            {t(action === "uninstall" && !preserveData ? "confirmDelete" : "confirm")}
          </label>
          <div className="installer-actions">
            <button disabled={working} onClick={() => void refresh()}>
              {t("refresh")}
            </button>
            {view?.installed && !view.pending && !view.conflict && (
              <button
                disabled={working}
                onClick={() => void bridge.open("terminal").catch(failure)}
              >
                {t("openTerminal")}
              </button>
            )}
            <button
              className="primary"
              disabled={working || blocked || !confirmed}
              onClick={() => void run(action)}
            >
              {working ? t("working") : t(view?.pending ? "resume" : "execute") + " · " + t(action)}
            </button>
          </div>
        </footer>
      </main>
    </div>
  );
}
