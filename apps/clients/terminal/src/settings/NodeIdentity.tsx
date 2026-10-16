import type { CommandHandler } from "../bridge/commands";
import { useState } from "react";
import type { MachineProfile } from "./RemoteMachine";
import { t } from "./service-state";

// Gives a remote machine a new mTLS identity over SSH. Everything the machine
// runs stops for it, so the owner reads what follows and confirms before
// anything is sent.
export function NodeIdentity({
  profile,
  busy,
  run,
  onCancel,
  onReplaced,
}: {
  profile: MachineProfile;
  busy: boolean;
  run: CommandHandler<boolean>;
  onCancel: () => void;
  onReplaced: () => void;
}) {
  const [keySource, setKeySource] = useState<"managed" | "provided">("managed");
  const [privateKey, setPrivateKey] = useState("");
  const [confirmed, setConfirmed] = useState(false);
  async function replace() {
    const secret = privateKey;
    setPrivateKey("");
    const { agent_port: _, ...machine } = profile;
    if (
      await run("node.identity.replace", {
        ...machine,
        key_source: keySource,
        private_key: secret,
      })
    )
      onReplaced();
  }
  return (
    <section className="deployment-wizard" aria-label={t("更换机器身份")}>
      <div className="deployment-heading">
        <h2>{t("更换 {machine} 的身份", { machine: profile.id })}</h2>
        <button disabled={busy} onClick={onCancel}>
          {t("取消")}
        </button>
      </div>
      <form
        onSubmit={event => {
          event.preventDefault();
          void replace();
        }}
      >
        <fieldset disabled={busy}>
          <p>
            {profile.id} · {profile.host}:{profile.ssh_port}
          </p>
          <p>
            {t(
              "证书有效期一年。更换时通过 SSH 为这台机器重新签发全部证书，服务、账户、数据和任务原样保留。",
            )}
          </p>
          <ul>
            <li>
              {t(
                "这台机器上的服务管理器和全部服务会停止再启动：交易授权和策略运行结束，行情需要重新登录，进行中的任务中断。",
              )}
            </li>
            <li>{t("本机到这台机器上各服务的连接需要重新连接。")}</li>
            <li>{t("交给他人使用的业务证书副本同时失效，需要重新分发。")}</li>
            <li>{t("中途失败时再次执行即可完成，使用的是同一套新证书。")}</li>
          </ul>
          <label>
            {t("SSH 密钥来源")}
            <select
              aria-label={t("SSH 密钥来源")}
              value={keySource}
              onChange={e => {
                setPrivateKey("");
                setKeySource(e.target.value as "managed" | "provided");
              }}
            >
              <option value="managed">{t("本机生成（推荐）")}</option>
              <option value="provided">{t("使用已有私钥")}</option>
            </select>
          </label>
          {keySource === "provided" && (
            <label>
              {t("SSH 私钥")}
              <textarea
                aria-label={t("SSH 私钥")}
                autoComplete="off"
                spellCheck={false}
                rows={4}
                value={privateKey}
                onChange={e => setPrivateKey(e.target.value)}
              />
            </label>
          )}
          <label className="deployment-check">
            <input
              type="checkbox"
              checked={confirmed}
              onChange={e => setConfirmed(e.target.checked)}
            />
            {t("我已了解这台机器上的服务会停止")}
          </label>
          <button
            className="primary"
            disabled={!confirmed || (keySource === "provided" && !privateKey.trim())}
          >
            {t("停止服务并更换身份")}
          </button>
        </fieldset>
      </form>
    </section>
  );
}
