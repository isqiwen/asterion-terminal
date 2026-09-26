import { useEffect, useRef, useState } from "react";
import type { SetupProgress } from "./Setup";

const historyLength = 40;
function rateLabel(rate: number): string {
  if (rate >= 1024 * 1024) return `${(rate / (1024 * 1024)).toFixed(2)} MB/s`;
  if (rate >= 1024) return `${(rate / 1024).toFixed(1)} KB/s`;
  return `${Math.round(rate)} B/s`;
}

// Mounted for one installation attempt. Unmounting stops the clock and retry starts fresh.
export function SetupTelemetry({
  progress,
  language,
}: {
  progress: SetupProgress;
  language: "zh" | "en";
}) {
  const latest = useRef(progress);
  latest.current = progress;
  const [sample, setSample] = useState({
    elapsed: 0,
    rate: null as number | null,
    upload: null as number | null,
    history: [] as (number | null)[],
  });
  useEffect(() => {
    const started = performance.now();
    let previous = latest.current.network;
    const timer = setInterval(() => {
      const now = performance.now();
      const current = latest.current.network;
      let rate: number | null = null;
      let upload: number | null = null;
      if (
        current &&
        previous &&
        current.interfaces === previous.interfaces &&
        current.sampled_ms > previous.sampled_ms &&
        current.received >= previous.received &&
        current.sent >= previous.sent
      ) {
        const seconds = (current.sampled_ms - previous.sampled_ms) / 1000;
        rate = (current.received - previous.received) / seconds;
        upload = (current.sent - previous.sent) / seconds;
      }
      previous = current;
      setSample((old) => ({
        elapsed: Math.floor((now - started) / 1000),
        rate,
        upload,
        history: [...old.history, rate].slice(-historyLength),
      }));
    }, 1000);
    return () => clearInterval(timer);
  }, []);
  const zh = language === "zh";
  const rate = progress.network ? sample.rate : null;
  const upload = progress.network ? sample.upload : null;
  const peak = Math.max(1, ...sample.history.map((value) => value ?? 0));
  let drawing = false;
  const line = sample.history
    .map((value, index) => {
      if (value === null) {
        drawing = false;
        return "";
      }
      const x =
        ((index + historyLength - sample.history.length) * 240) /
        (historyLength - 1);
      const y = 30 - (value / peak) * 26;
      const segment = `${drawing ? "L" : "M"}${x.toFixed(2)},${y.toFixed(2)}`;
      drawing = true;
      return segment;
    })
    .join(" ");
  const duration =
    sample.elapsed < 60
      ? `${sample.elapsed}s`
      : `${Math.floor(sample.elapsed / 60)}m ${sample.elapsed % 60}s`;
  return (
    <div
      className="setup-telemetry"
      aria-label={zh ? "安装状态" : "Installation statistics"}
    >
      <span data-testid="setup-elapsed">
        {zh ? "耗时" : "Elapsed"}: {duration}
      </span>
      <span
        data-testid="setup-speed"
        data-rate={rate ?? "unknown"}
        title={
          zh
            ? "本机物理网卡流量，包含其他应用；不含回环与虚拟网卡"
            : "Physical interface traffic for this computer, including other applications"
        }
      >
        ↓ {rate === null ? "—" : rateLabel(rate)}
      </span>
      <span
        data-testid="setup-upload"
        title={
          zh ? "本机物理网卡上传速度" : "Host physical interface upload speed"
        }
      >
        ↑ {upload === null ? "—" : rateLabel(upload)}
      </span>
      <svg
        viewBox="0 0 240 34"
        role="img"
        aria-label={
          zh ? "最近 40 秒下载速度" : "Download speed over the last 40 seconds"
        }
      >
        <path className="setup-speed-baseline" d="M0,31 L240,31" />
        <path className="setup-speed-line" d={line} />
      </svg>
    </div>
  );
}
