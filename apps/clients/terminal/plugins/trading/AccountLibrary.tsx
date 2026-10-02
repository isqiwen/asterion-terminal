import { open } from "@asterion/desktop-bridge/desktop";
import { nativeDesktop } from "../../src/bridge/desktop";
import { translate, type TerminalContext } from "../contract";
const t = (key: string) => translate("asterion.terminal.trading", key);
export function accountName(directory: string, fallback: string) {
  return directory.split(/[\\/]/).filter(Boolean).at(-1) || fallback;
}
export function AccountLibrary({
  context,
  kind,
  directory,
  setDirectory,
  onCreate,
  onOpen,
  onError,
}: {
  context: TerminalContext;
  kind: "paper" | "live";
  directory: string;
  setDirectory: (value: string) => void;
  onCreate: () => void;
  onOpen: (directory: string) => void;
  onError: (error: unknown) => void;
}) {
  const node = context.snapshot?.nodes.find(n => n.id === "local");
  const accounts = node?.health?.services.filter(s => s.kind === kind) ?? [];
  const unavailable = context.busy || !!context.snapshot?.stale || node?.state !== "online";
  return (
    <section className="account-library" aria-label={t("账户列表")}>
      <div className="workflow-heading">
        <div>
          <h3>{t("账户列表")}</h3>
          <p className="subtle">{t("打开已有账户，或创建新的独立账户。记录由本机服务保存。")}</p>
        </div>
        <button className="primary" disabled={unavailable} onClick={onCreate}>
          {t(kind === "paper" ? "新建回放账户" : "添加 CTP 账户")}
        </button>
      </div>
      {!accounts.length && <p className="workflow-empty">{t("尚无账户")}</p>}
      <div className="account-cards">
        {accounts.map(account => (
          <article key={account.id}>
            <strong>{accountName(account.directory, account.id)}</strong>
            <span className="subtle">
              {t(kind === "paper" ? "历史回放 · 无真实委托" : "CTP 账户 · 连接前确认环境")}
            </span>
            <button disabled={unavailable} onClick={() => onOpen(account.directory)}>
              {t("打开账户")}
            </button>
            <details>
              <summary>{t("记录位置")}</summary>
              <p>{account.directory}</p>
              <p>{account.id}</p>
            </details>
          </article>
        ))}
      </div>
      <details>
        <summary>{t("从指定目录恢复")}</summary>
        <p className="subtle">{t("用于打开不在列表中的账户，不修改原有记录。")}</p>
        <div className="futures-file">
          <label>
            {t(kind === "paper" ? "交易记录目录" : "实盘记录目录")}
            <input
              aria-label={t(kind === "paper" ? "交易记录目录" : "实盘记录目录")}
              value={directory}
              onChange={e => setDirectory(e.target.value)}
            />
          </label>
          {nativeDesktop && (
            <button
              disabled={context.busy}
              onClick={async () => {
                try {
                  const value = await open({ directory: true, multiple: false });
                  if (typeof value === "string") setDirectory(value);
                } catch (error) {
                  onError(error);
                }
              }}
            >
              {t("选择目录")}
            </button>
          )}
          <button disabled={context.busy || !directory} onClick={() => onOpen(directory)}>
            {t(kind === "paper" ? "恢复会话" : "恢复 CTP 账户")}
          </button>
        </div>
      </details>
    </section>
  );
}
