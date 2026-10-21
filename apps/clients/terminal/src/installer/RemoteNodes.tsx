import { useEffect, useState } from "react";
import { translate } from "../i18n";
const t = (key: string) => translate("installer", key);
export interface RemoteRequest {
  maintenance: {
    action: "INSPECT" | "PREPARE" | "FINISH" | "UNINSTALL" | "CONFIGURE";
    nodes?: string[];
    removeData?: boolean;
    connections?: Record<string, string>[];
  };
}
interface RemoteNode {
  id: string;
  host: string;
  directory: string;
  installed: boolean;
  port: number;
  version?: string;
  ssh?: Record<string, string>;
}
export interface RemoteView {
  nodes: RemoteNode[];
  pending: null | {
    action: string;
    version: string;
    selection: string[];
    remove_data: boolean;
    nodes: { id: string; phase: string; error: string }[];
  };
}
export function RemoteNodes({
  request,
  progress,
  busy,
  setBusy,
}: {
  request(input: RemoteRequest): Promise<unknown>;
  progress?: RemoteView;
  busy: boolean;
  setBusy(value: boolean): void;
}) {
  const [view, setView] = useState<RemoteView>();
  useEffect(() => {
    if (progress) setView(progress);
  }, [progress]);
  const [selected, setSelected] = useState<string[]>([]);
  const [preserve, setPreserve] = useState(true);
  const [confirmed, setConfirmed] = useState(false);
  const [error, setError] = useState("");
  const [form, setForm] = useState({
    id: "",
    host: "",
    sshPort: "22",
    agentPort: "46000",
    knownHosts: "",
    username: "asterion",
    keySource: "managed",
    privateKey: "",
  });
  async function invoke(input: RemoteRequest) {
    setBusy(true);
    setError("");
    setConfirmed(false);
    try {
      await request(input);
      const next = (await request({ maintenance: { action: "INSPECT" } })) as RemoteView;
      setView(next);
      if (next.pending?.action === "uninstall") {
        setSelected(
          next.pending.selection.length
            ? next.pending.selection
            : next.pending.nodes.map(n => n.id),
        );
        setPreserve(!next.pending.remove_data);
      }
    } catch (cause) {
      setError(cause instanceof Error ? cause.message : String(cause));
    } finally {
      setBusy(false);
    }
  }
  const pending = view?.pending;
  return (
    <section className="installer-card installer-remote" aria-label={t("remoteTitle")}>
      <h2>{t("remoteTitle")}</h2>
      <p>{t("remoteDescription")}</p>
      <button disabled={busy} onClick={() => void invoke({ maintenance: { action: "INSPECT" } })}>
        {t("remoteInspect")}
      </button>
      {view && (
        <select
          aria-label={t("remoteConnection")}
          value={form.id}
          disabled={busy}
          onChange={event => {
            const node = view.nodes.find(item => item.id === event.target.value);
            if (node)
              setForm({
                id: node.id,
                host: node.host,
                agentPort: String(node.port),
                sshPort: node.ssh?.ssh_port || "22",
                username: "asterion",
                knownHosts: node.ssh?.known_hosts || "",
                keySource: node.ssh?.key_source || "provided",
                privateKey: "",
              });
            setConfirmed(false);
          }}
        >
          <option value="">{t("remoteConnection")}</option>
          {view.nodes.map(node => (
            <option key={node.id} value={node.id}>
              {node.id}
            </option>
          ))}
        </select>
      )}
      {view?.nodes.map(node => (
        <label key={node.id} className="installer-confirm">
          <input
            type="checkbox"
            disabled={busy || !!pending || !node.installed}
            checked={selected.includes(node.id)}
            onChange={event => {
              setSelected(
                event.target.checked
                  ? [...selected, node.id]
                  : selected.filter(id => id !== node.id),
              );
              setConfirmed(false);
            }}
          />
          <span>
            {node.id} · {node.host} · {t(node.installed ? "installed" : "notInstalled")} ·{" "}
            {node.version || "—"}
            <code>{node.directory}</code>
          </span>
        </label>
      ))}
      {pending && (
        <div role="status">
          <strong>
            {t("remotePending")} · {pending.version}
          </strong>
          {pending.nodes.map(node => (
            <p key={node.id}>
              {node.id} · {t("remotePhase" + node.phase)}
              <code>{node.error}</code>
            </p>
          ))}
          {pending.action === "prepare" && <p>{t("remoteContinueHelp")}</p>}
        </div>
      )}
      <label className="installer-confirm">
        <input
          type="checkbox"
          checked={preserve}
          disabled={busy || !!pending}
          onChange={event => {
            setPreserve(event.target.checked);
            setConfirmed(false);
          }}
        />
        {t("remotePreserveOption")}
      </label>
      {!preserve && <p role="alert">{t("remoteDeleteWarning")}</p>}
      <label className="installer-confirm">
        <input
          type="checkbox"
          checked={confirmed}
          disabled={busy}
          onChange={event => setConfirmed(event.target.checked)}
        />
        {t("remoteConfirm")}
      </label>
      <button
        disabled={
          busy || !confirmed || !selected.length || (!!pending && pending.action !== "uninstall")
        }
        onClick={() =>
          void invoke({
            maintenance: {
              action: "UNINSTALL",
              nodes: pending?.selection || selected,
              removeData: !preserve,
            },
          })
        }
      >
        {t("remoteUninstall")}
      </button>
      {pending?.action === "prepare" && (
        <button
          disabled={
            busy ||
            !confirmed ||
            pending.nodes.some(n => !["prepared", "starting", "complete"].includes(n.phase))
          }
          onClick={() => void invoke({ maintenance: { action: "FINISH" } })}
        >
          {t("remoteFinish")}
        </button>
      )}
      <details>
        <summary>{t("remoteConnectionSettings")}</summary>
        {(["sshPort", "knownHosts"] as const).map(name => (
          <label key={name}>
            {t("remoteField" + name)}
            <input
              value={form[name]}
              disabled={busy || !form.id}
              onChange={event => {
                setForm({ ...form, [name]: event.target.value });
                setConfirmed(false);
              }}
            />
          </label>
        ))}
        <label>
          {t("remotePrivateKey")}
          <textarea
            value={form.privateKey}
            disabled={busy || !form.id}
            spellCheck={false}
            onChange={event => {
              setForm({
                ...form,
                privateKey: event.target.value,
                keySource: event.target.value ? "provided" : "managed",
              });
              setConfirmed(false);
            }}
          />
        </label>
        <button
          disabled={busy || !form.id || !form.host || !form.knownHosts}
          onClick={() => void invoke({ maintenance: { action: "CONFIGURE", connections: [form] } })}
        >
          {t("remoteSaveConnection")}
        </button>
      </details>
      {error && (
        <pre role="alert" className="installer-error">
          {error}
        </pre>
      )}
    </section>
  );
}
