import "./setup.css";
import { SetupTelemetry } from "./SetupTelemetry";

export type SetupProgress = {
  running: boolean;
  ready: boolean;
  step: number;
  downloaded: number;
  network: {
    received: number;
    sent: number;
    interfaces: string;
    sampled_ms: number;
  } | null;
  dependencies: {
    total: number | null;
    installed: number;
    phase: string;
    current: string;
  };
  total: number | null;
  error: string;
  directory: string;
};
const steps = {
  zh: [
    ["准备安装工具", "检查数据库并下载安装工具"],
    ["安装 Python 运行时", "下载用于后台计算的独立 Python"],
    ["创建隔离工作区", "建立应用专用环境，避免依赖冲突"],
    ["安装运行依赖", "下载数据存储、交易与分析所需依赖"],
    ["验证运行环境", "检查 Python、依赖库与本机数据库"],
  ],
  en: [
    [
      "Prepare Installation",
      "Check system PostgreSQL and download the environment installer",
    ],
    [
      "Install Python Runtime",
      "The Python engine used for background computation",
    ],
    [
      "Create Isolated Workspace",
      "Keep application libraries separate and conflict-free",
    ],
    [
      "Install Runtime Libraries",
      "Download storage, trading and analytics dependencies",
    ],
    ["Verify Runtime", "Check Python, libraries and the local database"],
  ],
};
export function Setup({
  progress,
  language,
  onLanguage,
  onStart,
  onEnter,
  checking,
}: {
  progress: SetupProgress;
  language: "zh" | "en";
  onLanguage: (value: "zh" | "en") => void;
  onStart: () => void;
  onEnter: () => void;
  checking: boolean;
}) {
  const dataDirectory = progress.directory.slice(
    0,
    progress.directory.lastIndexOf("/"),
  );
  const shortDirectory = dataDirectory.replace(
    /^\/(home|Users)\/[^/]+(?=\/)/,
    "~",
  );
  const zh = language === "zh";
  const active = progress.running || !!progress.error;
  const dependency = progress.dependencies;
  const labels: Record<string, string> = zh
    ? {
        system: "正在通过系统包管理器准备数据库",
        resolving: "正在解析依赖",
        preparing: "正在准备依赖",
        downloading: "正在下载",
        installing: "正在安装依赖",
        installed: "依赖安装完成",
        application: "正在安装星枢",
        verifying: "正在验证运行环境",
      }
    : {
        system: "Preparing database with the system package manager",
        resolving: "Resolving dependencies",
        preparing: "Preparing dependencies",
        downloading: "Downloading",
        installing: "Installing dependencies",
        installed: "Dependencies installed",
        application: "Installing Asterion",
        verifying: "Verifying runtime",
      };
  const dependencyText = `${labels[dependency.phase] ?? ""}${dependency.current ? `: ${dependency.current}` : ""}`;

  return (
    <main
      className={`first-setup${progress.running ? " is-installing" : ""}`}
      aria-label={zh ? "首次设置" : "First-Time Setup"}
    >
      <div className="setup-language-control">
        <select
          className="setup-language"
          aria-label={zh ? "语言" : "Language"}
          value={language}
          onChange={(e) => onLanguage(e.target.value as "zh" | "en")}
        >
          <option value="zh">简体中文</option>
          <option value="en">English</option>
        </select>
        <span className="setup-language-arrow" aria-hidden="true" />
      </div>
      <section className="setup-content">
        <header>
          <h1>ASTERION TERMINAL</h1>
          <p>{zh ? "准备你的工作区" : "Getting your workspace ready"}</p>
          <small>
            {zh
              ? "首次设置会准备系统 PostgreSQL，并下载独立运行工具和依赖库。"
              : "Setup prepares system PostgreSQL and downloads isolated tools and libraries."}
            <br />
            {zh
              ? "完成后保存在本机，下次启动无需重新下载。"
              : "They stay on this computer for future launches."}
          </small>
        </header>
        <div className="setup-steps" aria-live="polite">
          {steps[language].map(([title, description], index) => {
            const done = progress.ready || index < progress.step;
            const current = active && index === progress.step;
            const failed = current && !!progress.error;
            const percent = done
              ? 100
              : current && progress.total
                ? Math.min(
                    100,
                    Math.floor((progress.downloaded / progress.total) * 100),
                  )
                : current && index === 3 && dependency.total
                  ? Math.min(
                      100,
                      Math.floor(
                        (dependency.installed / dependency.total) * 100,
                      ),
                    )
                  : !current
                    ? 0
                    : undefined;
            return (
              <div className="setup-step" key={index}>
                <div>
                  <b>{title}</b>
                  <small>{description}</small>
                </div>
                <div
                  className={`setup-track ${done ? "done" : failed ? "failed" : current ? (percent === undefined ? "indeterminate" : "running") : ""}`}
                  role="progressbar"
                  aria-label={title}
                  aria-valuemin={0}
                  aria-valuemax={100}
                  aria-valuenow={percent}
                >
                  <i
                    style={
                      percent === undefined
                        ? undefined
                        : { width: `${percent}%` }
                    }
                  />
                </div>
                <span
                  className={
                    done
                      ? "setup-status-done"
                      : failed
                        ? "setup-status-failed"
                        : current
                          ? "setup-status-running"
                          : ""
                  }
                >
                  {done
                    ? zh
                      ? "完成"
                      : "DONE"
                    : failed
                      ? zh
                        ? "失败"
                        : "Failed"
                      : current
                        ? percent !== undefined
                          ? `${percent}%`
                          : zh
                            ? "进行中"
                            : "Running"
                        : zh
                          ? "等待"
                          : "Waiting"}
                </span>
              </div>
            );
          })}
        </div>
        {progress.error && (
          <p className="setup-error" role="alert">
            {progress.error}
          </p>
        )}
        <button
          className="setup-begin"
          disabled={checking || progress.running}
          onClick={progress.ready ? onEnter : onStart}
        >
          {checking
            ? zh
              ? "检查环境..."
              : "CHECKING..."
            : progress.ready
              ? zh
                ? "进入工作台"
                : "OPEN TERMINAL"
              : progress.running
                ? zh
                  ? "正在设置..."
                  : "SETTING UP..."
                : progress.error
                  ? zh
                    ? "重试设置"
                    : "RETRY SETUP"
                  : zh
                    ? "开始设置"
                    : "BEGIN SETUP"}
        </button>
        <p
          className="setup-note"
          data-testid="setup-detail"
          title={dependencyText}
        >
          {dependency.phase &&
          (progress.running || progress.ready || progress.error) ? (
            <>
              <span className="setup-current">
                {progress.ready
                  ? zh
                    ? "设置完成"
                    : "Setup complete"
                  : dependencyText}
              </span>
              {dependency.phase !== "system" && (
                <span className="setup-dependency-count">
                  {zh ? "已安装依赖" : "Dependencies installed"}:{" "}
                  {dependency.installed}/{dependency.total ?? "—"}
                </span>
              )}
            </>
          ) : progress.running ? (
            `${steps[language][progress.step]?.[0] ?? (zh ? "正在设置" : "Setting up")}...`
          ) : progress.ready ? (
            zh ? (
              "设置完成，可以开始使用。"
            ) : (
              "Setup complete. Your workspace is ready."
            )
          ) : zh ? (
            "首次设置需要联网，耗时取决于网络速度。"
          ) : (
            "Internet connection required. Time depends on your connection."
          )}
        </p>
        {progress.running && (
          <SetupTelemetry progress={progress} language={language} />
        )}
        <footer>
          {progress.directory &&
            `${zh ? "安装位置" : "Installing to"}: ${shortDirectory}`}
        </footer>
      </section>
    </main>
  );
}
