import {
  createContext,
  useContext,
  useEffect,
  useRef,
  useState,
  type FormEvent,
  type ReactNode,
} from "react";
import { nativeDesktop } from "@asterion/desktop-bridge/desktop";
import "./account.css";
import { accountRequest, type AccountUser } from "./client";
import { useLanguage } from "@asterion/ui-kit/language";
import { LanguagePicker } from "@asterion/ui-kit/LanguagePicker";
export type SecurityState = {
  locked: boolean;
  timeout_seconds: number;
  remaining_seconds: number;
  revision: number;
  retry_after: number;
};
const SecurityContext = createContext<{
  state?: SecurityState;
  update: (s: SecurityState) => void;
  token: string;
}>({ token: "", update: () => {} });
export function TerminalSecurity({
  children,
  token,
  account,
}: {
  children: ReactNode;
  token: string;
  account?: AccountUser;
}) {
  const [state, setState] = useState<SecurityState>();
  const [error, setError] = useState("");
  const [statusError, setStatusError] = useState("");
  const [language, setLanguage] = useLanguage();
  const [pin, setPin] = useState("");
  const [confirm, setConfirm] = useState("");
  const [password, setPassword] = useState("");
  const [reset, setReset] = useState(false);
  const [busy, setBusy] = useState(false);
  const current = useRef<SecurityState | undefined>(undefined);
  const deadline = useRef(0);
  const lastSent = useRef(0);
  const generation = useRef(0);
  const manualLock = useRef(false);
  const zh = language === "zh";
  function update(s: SecurityState) {
    if (typeof s.locked !== "boolean" || !Number.isFinite(s.remaining_seconds))
      throw new Error("无法确认终端锁定状态");
    if (current.current && s.revision < current.current.revision) return;
    const wasLocked = current.current?.locked;
    current.current = s;
    deadline.current = Date.now() + s.remaining_seconds * 1000;
    setState(s);
    if (wasLocked && !s.locked)
      window.dispatchEvent(new Event("asterion:unlocked"));
  }
  useEffect(() => {
    if (!nativeDesktop || !account) return;
    let active = true,
      checking = false;
    const epoch = ++generation.current;
    current.current = undefined;
    manualLock.current = false;
    setState(undefined);
    async function check(action = "") {
      if (manualLock.current) action = "/lock";
      if (checking && !action) return;
      if (!action) checking = true;
      try {
        const s = await accountRequest<SecurityState>(
          `/security${action}`,
          token,
          action ? {} : undefined,
        );
        if (active && generation.current === epoch) {
          if (manualLock.current && !s.locked) return;
          if (s.locked) manualLock.current = false;
          update(s);
          setStatusError("");
        }
      } catch (e) {
        if (active) {
          setStatusError(String(e));
          setState((s) => (s ? { ...s, locked: true } : s));
        }
      } finally {
        if (!action) checking = false;
      }
    }
    function expired() {
      if (
        current.current &&
        Date.now() >= deadline.current &&
        !current.current.locked
      ) {
        current.current = { ...current.current, locked: true };
        setState(current.current);
        return true;
      }
      return false;
    }
    function activity(event: Event) {
      if (expired()) {
        void check();
        return;
      }
      if (
        !event.isTrusted ||
        !document.hasFocus() ||
        document.visibilityState !== "visible" ||
        !current.current ||
        current.current.locked ||
        Date.now() - lastSent.current < 10000
      )
        return;
      lastSent.current = Date.now();
      void check("/activity");
    }
    function lock() {
      manualLock.current = true;
      setState((s) => (s ? { ...s, locked: true } : s));
      void check("/lock");
    }
    function focus() {
      expired();
      void check();
    }
    function shortcut(e: KeyboardEvent) {
      if (
        (e.metaKey || e.ctrlKey) &&
        e.shiftKey &&
        e.key.toLowerCase() === "l"
      ) {
        e.preventDefault();
        lock();
      }
    }
    void check();
    const timer = setInterval(() => {
      expired();
      void check();
    }, 1000);
    const events = ["pointerdown", "pointermove", "keydown", "wheel"];
    events.forEach((name) =>
      window.addEventListener(name, activity, { passive: true }),
    );
    window.addEventListener("focus", focus);
    document.addEventListener("visibilitychange", focus);
    window.addEventListener("asterion:lock", lock);
    window.addEventListener("keydown", shortcut);
    return () => {
      active = false;
      clearInterval(timer);
      events.forEach((name) => window.removeEventListener(name, activity));
      window.removeEventListener("focus", focus);
      document.removeEventListener("visibilitychange", focus);
      window.removeEventListener("asterion:lock", lock);
      window.removeEventListener("keydown", shortcut);
    };
  }, [token, account?.email]);
  const locked = nativeDesktop && !!account && (!state || state.locked);
  useEffect(() => {
    document.documentElement.dataset.locked = String(locked);
    if (locked) {
      setPin("");
      setConfirm("");
      setPassword("");
    }
    return () => {
      delete document.documentElement.dataset.locked;
    };
  }, [locked]);
  async function submit(event: FormEvent) {
    event.preventDefault();
    if (reset && pin !== confirm) {
      setError(zh ? "两次输入的 PIN 不一致" : "PINs do not match");
      return;
    }
    if (!state) return;
    setBusy(true);
    setError("");
    const epoch = generation.current;
    try {
      const next = await accountRequest<SecurityState>(
        `/security/${reset ? "change" : "unlock"}`,
        token,
        {
          pin,
          password: reset ? password : undefined,
          expected: state.revision,
        },
      );
      if (epoch === generation.current) {
        update(next);
        setReset(false);
        setPin("");
        setConfirm("");
        setPassword("");
      }
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
      setPin("");
    } finally {
      setBusy(false);
    }
  }
  return (
    <SecurityContext.Provider value={{ state, update, token }}>
      <div className="security-content" hidden={locked} inert={locked}>
        {children}
      </div>
      {locked && (
        <main
          className="account-screen terminal-lock"
          aria-label={zh ? "终端锁屏" : "Terminal lock"}
        >
          {!nativeDesktop && (
            <div className="account-brand">
              <span>✧</span> ASTERION TERMINAL
            </div>
          )}
          <LanguagePicker value={language} onChange={setLanguage} />
          <div className="account-center">
            <section className="account-box account-lock">
              <header>
                <h1>
                  {reset
                    ? zh
                      ? "重设 PIN"
                      : "RESET PIN"
                    : zh
                      ? "终端已锁定"
                      : "TERMINAL LOCKED"}
                </h1>
                <span className="panel-spacer" />
                <small className="good">SECURE</small>
              </header>
              <p className="account-description">
                {!state
                  ? zh
                    ? "正在确认锁定状态…"
                    : "Checking terminal security…"
                  : zh
                    ? "输入你的 6 位 PIN 解锁终端"
                    : "Enter your 6-digit PIN to unlock"}
              </p>
              {state && state.retry_after > 0 && (
                <p role="status" className="account-description">
                  {zh
                    ? `请在 ${state.retry_after} 秒后重试`
                    : `Try again in ${state.retry_after}s`}
                </p>
              )}
              <form onSubmit={submit}>
                <fieldset
                  className="account-fields"
                  disabled={busy || !state || state.retry_after > 0}
                >
                  {reset && (
                    <label>
                      {zh ? "账号密码" : "ACCOUNT PASSWORD"}
                      <input
                        type="password"
                        autoComplete="current-password"
                        required
                        value={password}
                        onChange={(e) => setPassword(e.target.value)}
                      />
                    </label>
                  )}
                  <label>
                    PIN
                    <input
                      autoFocus
                      className="pin-input"
                      type="password"
                      inputMode="numeric"
                      autoComplete="off"
                      pattern="[0-9]{6}"
                      maxLength={6}
                      required
                      placeholder="— — — — — —"
                      value={pin}
                      onChange={(e) =>
                        setPin(e.target.value.replace(/\D/g, "").slice(0, 6))
                      }
                    />
                  </label>
                  {reset && (
                    <label>
                      {zh ? "确认 PIN" : "CONFIRM PIN"}
                      <input
                        type="password"
                        inputMode="numeric"
                        autoComplete="off"
                        pattern="[0-9]{6}"
                        maxLength={6}
                        required
                        value={confirm}
                        onChange={(e) =>
                          setConfirm(
                            e.target.value.replace(/\D/g, "").slice(0, 6),
                          )
                        }
                      />
                    </label>
                  )}
                  {(error || statusError) && (
                    <p role="alert" className="account-error">
                      {error || statusError}
                    </p>
                  )}
                  <button className="account-primary" type="submit">
                    {busy
                      ? "…"
                      : reset
                        ? zh
                          ? "保存 PIN"
                          : "SAVE PIN"
                        : zh
                          ? "解锁"
                          : "UNLOCK"}
                  </button>
                </fieldset>
              </form>
              {statusError && (
                <button
                  className="account-secondary"
                  onClick={() =>
                    window.dispatchEvent(new Event("asterion:reconnect"))
                  }
                >
                  {zh ? "重连本机服务" : "RECONNECT LOCAL SERVICE"}
                </button>
              )}
              {state && (
                <button
                  className="account-secondary"
                  disabled={busy}
                  onClick={() => {
                    setReset(!reset);
                    setPin("");
                    setConfirm("");
                    setPassword("");
                    setError("");
                  }}
                >
                  {reset
                    ? zh
                      ? "返回 PIN 解锁"
                      : "BACK TO PIN"
                    : zh
                      ? "忘记 PIN？使用账号密码重设"
                      : "FORGOT PIN? RESET WITH PASSWORD"}
                </button>
              )}
            </section>
          </div>
        </main>
      )}
    </SecurityContext.Provider>
  );
}
export function SecurityPreferences() {
  const { state, update, token } = useContext(SecurityContext);
  const [error, setError] = useState("");
  return (
    <>
      <h2 className="settings-section-title">终端锁屏</h2>
      <div className="setting-row">
        <label htmlFor="lock-timeout">无操作后自动锁定</label>
        <select
          id="lock-timeout"
          value={state?.timeout_seconds ?? 300}
          disabled={!state}
          onChange={async (e) => {
            try {
              update(
                await accountRequest<SecurityState>(
                  "/security/timeout",
                  token,
                  { seconds: Number(e.target.value) },
                ),
              );
              setError("");
            } catch (e) {
              setError(String(e));
            }
          }}
        >
          {[1, 5, 10, 15, 30].map((n) => (
            <option key={n} value={n * 60}>
              {n} 分钟
            </option>
          ))}
        </select>
      </div>
      <p>
        所有终端窗口共用锁屏状态。离开应用、无操作或睡眠期间计时，后台任务继续运行。
      </p>
      <button onClick={() => window.dispatchEvent(new Event("asterion:lock"))}>
        立即锁定
      </button>
      <p>快捷键：⌘⇧L。需要修改 PIN 时，锁屏后使用账号密码重设。</p>
      {error && <p role="alert">{error}</p>}
    </>
  );
}
