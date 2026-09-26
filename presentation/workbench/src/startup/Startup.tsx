import { useEffect, useState } from "react";

const steps = [
  ["检查本机配置", "准备当前用户的独立工作区"],
  ["启动本机服务", "启动数据存储与后台任务服务"],
  ["验证服务连接", "确认工作台可以访问本机服务"],
];
export function Startup({
  step,
  error,
  retry,
}: {
  step: number;
  error: string;
  retry: () => void;
}) {
  const [elapsed, setElapsed] = useState(0);
  useEffect(() => {
    const started = Date.now();
    const timer = setInterval(
      () => setElapsed(Math.floor((Date.now() - started) / 1000)),
      1000,
    );
    return () => clearInterval(timer);
  }, []);
  return (
    <main className="startup-screen" aria-label="启动工作台">
      <span className="startup-language">简体中文</span>
      <section className="startup-content">
        <header>
          <h1>ASTERION TERMINAL</h1>
          <p>正在准备你的工作台</p>
          <small>
            首次启动需要初始化本机工作区。
            <br />
            准备完成后进入账号登录。
          </small>
        </header>
        <div className="startup-steps" aria-live="polite">
          {steps.map(([title, description], index) => (
            <div className="startup-step" key={title}>
              <div>
                <b>{title}</b>
                <small>{description}</small>
              </div>
              <div
                className={`startup-track ${index < step ? "done" : index === step ? (error ? "failed" : "running") : ""}`}
                role="progressbar"
                aria-label={title}
                aria-valuemin={0}
                aria-valuemax={100}
                aria-valuenow={
                  index < step ? 100 : index > step ? 0 : undefined
                }
              >
                <i />
              </div>
              <span className={index < step ? "complete" : ""}>
                {index < step
                  ? "完成"
                  : index === step
                    ? error
                      ? "失败"
                      : "进行中"
                    : "等待"}
              </span>
            </div>
          ))}
        </div>
        {error ? (
          <>
            <p className="startup-error" role="alert">
              {error}
            </p>
            <button className="startup-retry" onClick={retry}>
              重试启动
            </button>
          </>
        ) : (
          <div className="startup-action">正在准备…</div>
        )}
        <footer>
          <span>已用时 {elapsed}s</span>
          <span>
            {error ? "启动未完成" : steps[Math.min(step, steps.length - 1)][0]}
          </span>
        </footer>
      </section>
    </main>
  );
}
