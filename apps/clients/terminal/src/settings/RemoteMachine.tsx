import { useState } from "react";
import { open } from "@asterion/desktop-bridge/desktop";
import { nativeDesktop } from "../bridge/desktop";
import { exportLinuxInitializer, type Snapshot, type TerminalCommand } from "../bridge/client";
import { asDisplayError, ErrorNotice, type DisplayError } from "../i18n/errors";
import { FirewallPreview } from "./FirewallPreview";
import { t } from "./service-state";
const storageKey = "asterion.ssh-node-profiles.v1";
export type MachineProfile = {
  id: string;
  host: string;
  ssh_port: string;
  username: string;
  known_hosts: string;
  agent_port: string;
};
const empty: MachineProfile = {
  id: "",
  host: "",
  ssh_port: "22",
  username: "asterion",
  known_hosts: "",
  agent_port: "7442",
};
export function loadMachineProfiles(): MachineProfile[] {
  const value: unknown = JSON.parse(localStorage.getItem(storageKey) ?? "[]");
  if (
    !Array.isArray(value) ||
    !value.every(
      p =>
        p &&
        Object.keys(p).length === Object.keys(empty).length &&
        Object.keys(empty).every(k => typeof p[k] === "string"),
    ) ||
    new Set(value.map(p => p.id)).size !== value.length
  )
    throw new Error(t("节点配置无法读取，原始内容未改写。请修复配置后重新打开设置。"));
  return value;
}
export function RemoteMachine({
  snapshot,
  busy,
  run,
  onCancel,
  onInstalled,
  onSaved,
  initial,
}: {
  snapshot: Snapshot | null;
  busy: boolean;
  initial?: MachineProfile;
  run: (method: TerminalCommand, params: Record<string, string>) => Promise<boolean>;
  onCancel: () => void;
  onInstalled: (profile: MachineProfile) => void;
  onSaved: (profile: MachineProfile) => void;
}) {
  const [step, setStep] = useState(0);
  const [profile, setProfile] = useState<MachineProfile>(initial ?? { ...empty });
  const [keySource, setKeySource] = useState<"managed" | "provided">("managed");
  const [privateKey, setPrivateKey] = useState("");
  const [error, setError] = useState<DisplayError>("");
  const [exported, setExported] = useState(false);
  const [initialized, setInitialized] = useState(false);
  const [verified, setVerified] = useState(false);
  const [copied, setCopied] = useState(false);
  const key = snapshot?.ssh_key?.id === profile.id ? snapshot.ssh_key : null;
  const ready = keySource === "managed" ? !!key : !!privateKey.trim();
  const plan = snapshot?.firewall_plan;
  const titles = ["机器信息", "初始化机器", "核验并连接"];
  function editMachine(next: MachineProfile) {
    setProfile(next);
    setInitialized(false);
    setVerified(false);
  }
  async function next() {
    setError("");
    if (step === 0) {
      try {
        if (!initial && loadMachineProfiles().some(p => p.id === profile.id))
          throw new Error(t("机器名称已存在，请从机器列表继续配置。"));
        if (keySource === "managed" && !(await run("node.key.prepare", { id: profile.id }))) return;
        setStep(1);
      } catch (reason) {
        setError(asDisplayError(reason));
      }
    } else if (step === 1) setStep(2);
    else {
      try {
        const profiles = loadMachineProfiles();
        localStorage.setItem(
          storageKey,
          JSON.stringify([...profiles.filter(p => p.id !== profile.id), profile]),
        );
        onSaved(profile);
        const secret = privateKey;
        setPrivateKey("");
        if (await run("node.bootstrap", { ...profile, key_source: keySource, private_key: secret }))
          onInstalled(profile);
      } catch (reason) {
        setError(asDisplayError(reason));
      }
    }
  }
  async function chooseKnownHosts() {
    try {
      const value = await open({ multiple: false });
      if (typeof value === "string") {
        setProfile({ ...profile, known_hosts: value });
        setVerified(false);
      }
    } catch (reason) {
      setError(asDisplayError(reason));
    }
  }
  return (
    <section className="deployment-wizard" aria-label={t("添加远程机器")}>
      <div className="deployment-heading">
        <h2>{t(initial ? "配置远程机器" : "添加远程机器")}</h2>
        <button disabled={busy} onClick={onCancel}>
          {t("取消")}
        </button>
      </div>
      <ol className="deployment-steps" aria-label={t("添加机器进度")}>
        {titles.map((title, index) => (
          <li key={title} aria-current={step === index ? "step" : undefined}>
            <span>{index + 1}</span>
            {t(title)}
          </li>
        ))}
      </ol>
      {error && (
        <p role="alert">
          <ErrorNotice error={error} namespace="host" />
        </p>
      )}
      <form
        onSubmit={event => {
          event.preventDefault();
          void next();
        }}
      >
        <fieldset disabled={busy}>
          <legend>{t(titles[step])}</legend>
          {step === 0 && (
            <>
              <p className="subtle">{t("仅支持 Linux x86_64。添加机器不会改变当前运行位置。")}</p>
              <div className="futures-fields">
                <label>
                  {t("机器名称")}
                  <input
                    aria-label={t("机器名称")}
                    required
                    pattern="[A-Za-z0-9_-]+"
                    maxLength={40}
                    placeholder="backtest-factor-1"
                    value={profile.id}
                    disabled={!!initial}
                    onChange={e => editMachine({ ...profile, id: e.target.value })}
                  />
                </label>
                <label>
                  {t("SSH 地址")}
                  <input
                    aria-label={t("SSH 地址")}
                    required
                    placeholder="192.0.2.10"
                    value={profile.host}
                    onChange={e => editMachine({ ...profile, host: e.target.value })}
                  />
                </label>
              </div>
              <details>
                <summary>{t("高级连接选项")}</summary>
                <div className="futures-fields">
                  {(
                    [
                      ["ssh_port", "SSH 端口"],
                      ["username", "SSH 用户"],
                      ["agent_port", "服务管理器端口"],
                    ] as const
                  ).map(([field, label]) => (
                    <label key={field}>
                      {t(label)}
                      <input
                        aria-label={t(label)}
                        required
                        {...(field !== "username" ? { type: "number", min: 1, max: 65535 } : {})}
                        value={profile[field]}
                        onChange={e => editMachine({ ...profile, [field]: e.target.value })}
                      />
                    </label>
                  ))}
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
                </div>
              </details>
            </>
          )}
          {step === 1 && (
            <>
              <p>{t("将公钥和初始化脚本交给目标机器的管理员。私钥始终留在本机。")}</p>
              {keySource === "managed" && (
                <>
                  <label>
                    {t("初始化公钥")}
                    <textarea
                      aria-label={t("初始化公钥")}
                      readOnly
                      rows={3}
                      value={key?.public_key ?? ""}
                    />
                  </label>
                  <div className="source-actions">
                    <button
                      type="button"
                      disabled={!key}
                      onClick={() => {
                        void navigator.clipboard
                          .writeText(key!.public_key)
                          .then(() => setCopied(true))
                          .catch(() => setError(t("无法访问剪贴板，请手动复制上方公钥")));
                      }}
                    >
                      {t("复制公钥")}
                    </button>
                  </div>
                  {copied && <p role="status">{t("公钥已复制")}</p>}
                </>
              )}
              {keySource === "provided" && (
                <p>{t("使用所选私钥对应的公钥初始化目标机器，下一步只在本机输入私钥。")}</p>
              )}
              <button
                type="button"
                onClick={() => {
                  void exportLinuxInitializer()
                    .then(ok => {
                      if (ok) setExported(true);
                    })
                    .catch(reason => setError(asDisplayError(reason)));
                }}
              >
                {t("导出 Linux 初始化脚本")}
              </button>
              {exported && (
                <p role="status">{t("初始化脚本已导出，请复制到目标 Linux 并由管理员执行")}</p>
              )}
              <p>{t("管理员在目标机器执行")}</p>
              <pre>sudo python3 -I initialize-linux.py</pre>
              <p className="subtle">
                {t("脚本创建专用账户并输出主机指纹。它不会安装业务服务或自动开放端口。")}
              </p>
              <label className="deployment-check">
                <input
                  type="checkbox"
                  checked={initialized}
                  onChange={e => setInitialized(e.target.checked)}
                />
                {t("管理员已完成初始化")}
              </label>
            </>
          )}
          {step === 2 && (
            <>
              <p>
                {profile.id} · {profile.host}:{profile.ssh_port}
              </p>
              <p>{t("使用管理员提供的主机指纹核验 known_hosts。未知或变化的主机身份会被拒绝。")}</p>
              <label>
                {t("已核验 known_hosts 文件")}
                <input
                  aria-label={t("已核验 known_hosts 文件")}
                  required
                  value={profile.known_hosts}
                  onChange={e => {
                    setProfile({ ...profile, known_hosts: e.target.value });
                    setVerified(false);
                  }}
                />
              </label>
              {nativeDesktop && (
                <button type="button" onClick={() => void chooseKnownHosts()}>
                  {t("选择文件")}
                </button>
              )}
              <label className="deployment-check">
                <input
                  type="checkbox"
                  checked={verified}
                  onChange={e => setVerified(e.target.checked)}
                />
                {t("已通过可信渠道核对主机指纹")}
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
              <details>
                <summary>{t("安装前检查防火墙")}</summary>
                <p>{t("只检查当前机器的管理端口；规则变更需要单独确认。")}</p>
                <div className="source-actions">
                  {(
                    [
                      ["allow", "检查并预览放行规则"],
                      ["remove", "检查已管理规则的撤销"],
                    ] as const
                  ).map(([action, label]) => (
                    <button
                      type="button"
                      key={action}
                      disabled={!ready || !profile.known_hosts || !verified}
                      onClick={() =>
                        void run("node.firewall.inspect", {
                          ...profile,
                          key_source: keySource,
                          private_key: privateKey,
                          firewall_port: profile.agent_port,
                          firewall_action: action,
                        })
                      }
                    >
                      {t(label)}
                    </button>
                  ))}
                </div>
              </details>
              {plan?.transport === "ssh" &&
                plan.id === profile.id &&
                plan.host === profile.host &&
                plan.port === Number(profile.agent_port) && (
                  <FirewallPreview
                    plan={plan}
                    busy={busy}
                    keyReady={ready && verified}
                    privateKey={privateKey}
                    clearKey={() => setPrivateKey("")}
                    run={run}
                  />
                )}
              <p className="subtle">{t("安装后仅连接机器。下一步再选择部署哪些服务。")}</p>
            </>
          )}
          <div className="source-actions wizard-actions">
            {step > 0 && (
              <button type="button" onClick={() => setStep(step - 1)}>
                {t("上一步")}
              </button>
            )}
            <button
              className="primary"
              type="submit"
              disabled={
                step === 1
                  ? !initialized
                  : step === 2
                    ? !ready || !verified
                    : !profile.id || !profile.host
              }
            >
              {busy ? t("正在校验并安装…") : t(step === 2 ? "安装并连接" : "下一步")}
            </button>
          </div>
        </fieldset>
      </form>
    </section>
  );
}
