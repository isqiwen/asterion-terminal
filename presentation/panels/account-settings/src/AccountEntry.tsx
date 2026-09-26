import { useEffect, useRef, useState, type FormEvent } from "react";
import { nativeDesktop } from "@asterion/desktop-bridge/desktop";
import "./account.css";
import { AccountError, accountRequest, type AccountUser } from "./client";
import { useLanguage } from "@asterion/ui-kit/language";
import { LanguagePicker } from "@asterion/ui-kit/LanguagePicker";

type Screen = "login" | "register" | "verify" | "reset";
const messages = {
  zh: {
    login: "登录",
    register: "创建账号",
    verify: "验证邮箱",
    reset: "找回密码",
    intro: "访问你的终端账号",
    first: "名",
    last: "姓",
    email: "邮箱",
    password: "密码",
    confirm: "确认密码",
    code: "区号",
    phone: "手机号码（选填）",
    show: "显示",
    hide: "隐藏",
    forgot: "忘记密码？",
    noAccount: "还没有账号？",
    signUp: "注册",
    hasAccount: "已有账号？",
    verifyInfo: "输入邮箱收到的 6 位验证码，确认你的邮箱地址。",
    digits: "6 位验证码",
    resend: "重新发送验证码",
    back: "返回登录",
    resetInfo: "输入账号邮箱，申请密码重置邮件。",
    send: "发送重置邮件",
    local: "直接进入本机工作台",
    unavailable: "账号服务尚未连接。当前无法登录、注册或发送邮件。",
    mismatch: "两次输入的密码不一致。",
    short: "密码至少需要 12 个字符。",
    invalidCode: "请输入 6 位数字验证码。",
    strength: ["至少 12 个字符", "较短", "长度合适", "长密码"],
    verifyLink: "已有验证码？验证邮箱",
    note: "账号服务未连接 · 本机功能可直接使用",
    backRegister: "返回",
    passwordHint: "至少 12 个字符",
  },
  en: {
    login: "SIGN IN",
    register: "CREATE ACCOUNT",
    verify: "VERIFY YOUR EMAIL",
    reset: "RESET PASSWORD",
    intro: "Access your terminal account",
    first: "FIRST NAME",
    last: "LAST NAME",
    email: "EMAIL",
    password: "PASSWORD",
    confirm: "CONFIRM PASSWORD",
    code: "CODE",
    phone: "PHONE (OPTIONAL)",
    show: "SHOW",
    hide: "HIDE",
    forgot: "FORGOT PASSWORD?",
    noAccount: "No account?",
    signUp: "SIGN UP",
    hasAccount: "Already have an account?",
    verifyInfo:
      "Enter the 6-digit code from your email to confirm your address.",
    digits: "6-DIGIT CODE",
    resend: "SEND A NEW CODE",
    back: "BACK TO LOGIN",
    resetInfo: "Enter your account email to request a password reset.",
    send: "SEND RESET EMAIL",
    local: "CONTINUE TO LOCAL WORKSPACE",
    unavailable:
      "Account service is not connected. Sign-in, registration and email delivery are unavailable.",
    mismatch: "Passwords do not match.",
    short: "Use at least 12 characters.",
    invalidCode: "Enter a 6-digit verification code.",
    strength: [
      "Use at least 12 characters",
      "Too short",
      "Good length",
      "Long password",
    ],
    verifyLink: "HAVE A CODE? VERIFY EMAIL",
    note: "Account service offline · Local workspace available",
    backRegister: "Back",
    passwordHint: "At least 12 characters",
  },
};

export function AccountEntry({
  onEnter,
  token,
}: {
  onEnter: (user: AccountUser) => void;
  token: string;
}) {
  const [language, setLanguage] = useLanguage();
  const [localMode, setLocalMode] = useState(false);
  useEffect(() => {
    if (!token) return;
    accountRequest<{ verification: string }>("/capabilities", token)
      .then((c) => setLocalMode(c.verification === "local"))
      .catch(() => {});
  }, [token]);
  const t = messages[language];
  const [screen, setScreen] = useState<Screen>("login");
  const [email, setEmail] = useState("");
  const [password, setPassword] = useState("");
  const [confirm, setConfirm] = useState("");
  const [pin, setPin] = useState("");
  const [pinConfirm, setPinConfirm] = useState("");
  const [code, setCode] = useState("");
  const [show, setShow] = useState(false);
  const [error, setError] = useState("");
  const [notice, setNotice] = useState("");
  const [busy, setBusy] = useState(false);
  const [resetSent, setResetSent] = useState(false);
  const [cooldown, setCooldown] = useState(0);
  const firstInput = useRef<HTMLInputElement>(null);
  useEffect(() => {
    firstInput.current?.focus();
  }, [screen]);
  useEffect(() => {
    if (!cooldown) return;
    const timer = setTimeout(() => setCooldown((n) => n - 1), 1000);
    return () => clearTimeout(timer);
  }, [cooldown]);
  const say = (zh: string, en: string) => (language === "zh" ? zh : en);
  function navigate(next: Screen) {
    setPassword("");
    setConfirm("");
    setPin("");
    setPinConfirm("");
    setCode("");
    setShow(false);
    setError("");
    setNotice("");
    setResetSent(false);
    setScreen(next);
  }
  async function submit(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    const newPassword =
      screen === "register" || (screen === "reset" && resetSent);
    if (newPassword && password.length < 12) {
      setError(t.short);
      return;
    }
    if (newPassword && password !== confirm) {
      setError(t.mismatch);
      return;
    }
    if (
      screen === "register" &&
      (!/^[0-9]{6}$/.test(pin) || pin !== pinConfirm)
    ) {
      setError(
        say(
          "请设置 6 位数字 PIN，并确保两次输入一致。",
          "Set a matching 6-digit PIN.",
        ),
      );
      return;
    }
    const form = new FormData(event.currentTarget);
    setBusy(true);
    setError("");
    setNotice("");
    try {
      if (screen === "login") {
        const result = await accountRequest<{ user: AccountUser }>(
          "/login",
          token,
          { email, password },
        );
        setPassword("");
        onEnter(result.user);
      } else if (screen === "register") {
        await accountRequest("/register", token, {
          email,
          password,
          pin,
          first_name: form.get("given-name"),
          last_name: form.get("family-name"),
          country_code: form.get("country-code") ?? "",
          phone: form.get("phone") ?? "",
        });
        navigate("verify");
        setCooldown(localMode ? 0 : 60);
        setNotice(
          say(
            localMode
              ? "本地账号已创建，输入任意 6 位数字继续。"
              : "验证码已发送，请检查邮箱。",
            localMode
              ? "Local account created. Enter any 6 digits to continue."
              : "Verification code sent. Check your email.",
          ),
        );
      } else if (screen === "verify") {
        await accountRequest("/verify", token, { email, code });
        navigate("login");
        setNotice(
          say(
            localMode ? "本地账号已确认，请登录。" : "邮箱验证成功，请登录。",
            localMode
              ? "Local account confirmed. Please sign in."
              : "Email verified. Please sign in.",
          ),
        );
      } else if (!resetSent) {
        await accountRequest("/forgot", token, { email });
        setResetSent(true);
        setCooldown(localMode ? 0 : 60);
        setNotice(
          say(
            localMode
              ? "本地模式：输入任意 6 位数字重置密码。"
              : "如果账号存在，重置验证码已发送。",
            localMode
              ? "Local mode: enter any 6 digits to reset your password."
              : "If the account exists, a reset code has been sent.",
          ),
        );
      } else {
        await accountRequest("/reset", token, { email, code, password });
        navigate("login");
        setNotice(
          say("密码已更新，请重新登录。", "Password updated. Please sign in."),
        );
      }
    } catch (e) {
      if (e instanceof AccountError && e.code === "EMAIL_UNVERIFIED")
        navigate("verify");
      setError(e instanceof Error ? e.message : String(e));
    } finally {
      setBusy(false);
    }
  }
  async function resend() {
    setBusy(true);
    setError("");
    try {
      await accountRequest(screen === "reset" ? "/forgot" : "/resend", token, {
        email,
      });
      setCooldown(localMode ? 0 : 60);
      setNotice(
        say(
          localMode
            ? "本地验证已准备好，输入任意 6 位数字。"
            : "如果邮箱符合条件，验证码已发送。",
          localMode
            ? "Local verification is ready. Enter any 6 digits."
            : "If eligible, a verification code has been sent.",
        ),
      );
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    } finally {
      setBusy(false);
    }
  }
  const newPassword =
    screen === "register" || (screen === "reset" && resetSent);
  const strength =
    password.length === 0
      ? 0
      : password.length < 12
        ? 1
        : password.length < 20
          ? 2
          : 3;
  return (
    <main
      className="account-screen"
      lang={language === "zh" ? "zh-CN" : "en"}
      aria-label={say("账号入口", "Account entry")}
    >
      {!nativeDesktop && (
        <div className="account-brand">
          <span>✧</span> ASTERION TERMINAL
        </div>
      )}
      <LanguagePicker value={language} onChange={setLanguage} />
      <div className="account-center">
        <section
          className={`account-box account-${screen}`}
          aria-labelledby="account-title"
        >
          <header>
            {screen !== "login" && (
              <button
                disabled={busy}
                type="button"
                aria-label={t.backRegister}
                onClick={() => navigate("login")}
              >
                ‹
              </button>
            )}
            <h1 id="account-title">{t[screen]}</h1>
          </header>
          <form onSubmit={submit} key={screen}>
            <fieldset disabled={busy} className="account-fields">
              {screen === "login" && (
                <p className="account-description">{t.intro}</p>
              )}
              {screen === "register" && (
                <div className="account-name-row">
                  <label>
                    {t.first}
                    <input
                      ref={firstInput}
                      name="given-name"
                      autoComplete="given-name"
                      maxLength={80}
                      required
                    />
                  </label>
                  <label>
                    {t.last}
                    <input
                      name="family-name"
                      autoComplete="family-name"
                      maxLength={80}
                      required
                    />
                  </label>
                </div>
              )}
              <label>
                {t.email}
                <input
                  ref={screen !== "register" ? firstInput : undefined}
                  type="email"
                  autoComplete="email"
                  maxLength={254}
                  required
                  value={email}
                  onChange={(e) => setEmail(e.target.value)}
                  placeholder="user@domain.com"
                />
              </label>
              {screen === "register" && (
                <div className="account-phone-row">
                  <label>
                    {t.code}
                    <input
                      type="tel"
                      name="country-code"
                      autoComplete="tel-country-code"
                      maxLength={6}
                      placeholder="+86"
                    />
                  </label>
                  <label>
                    {t.phone}
                    <input
                      type="tel"
                      name="phone"
                      autoComplete="tel-national"
                      maxLength={30}
                    />
                  </label>
                </div>
              )}
              {(screen === "login" || newPassword) && (
                <label>
                  {t.password}
                  <div className="account-password">
                    <input
                      aria-label={t.password}
                      type={show ? "text" : "password"}
                      autoComplete={
                        screen === "login" ? "current-password" : "new-password"
                      }
                      maxLength={128}
                      required
                      value={password}
                      onChange={(e) => setPassword(e.target.value)}
                      placeholder={newPassword ? t.passwordHint : "••••••••"}
                    />
                    <button
                      type="button"
                      aria-pressed={show}
                      onClick={() => setShow((s) => !s)}
                    >
                      {show ? t.hide : t.show}
                    </button>
                  </div>
                </label>
              )}
              {newPassword && (
                <>
                  <div
                    className={`account-strength length-${strength}`}
                    aria-label={t.strength[strength]}
                  >
                    <i />
                    <i />
                    <i />
                    <i />
                  </div>
                  <p className="account-description">{t.strength[strength]}</p>
                  <label>
                    {t.confirm}
                    <input
                      type={show ? "text" : "password"}
                      autoComplete="new-password"
                      maxLength={128}
                      required
                      value={confirm}
                      onChange={(e) => setConfirm(e.target.value)}
                    />
                  </label>
                </>
              )}
              {screen === "register" && (
                <>
                  <label>
                    {say("终端 PIN（6 位数字）", "TERMINAL PIN (6 DIGITS)")}
                    <input
                      type="password"
                      inputMode="numeric"
                      autoComplete="off"
                      pattern="[0-9]{6}"
                      maxLength={6}
                      required
                      value={pin}
                      onChange={(e) =>
                        setPin(e.target.value.replace(/\D/g, "").slice(0, 6))
                      }
                    />
                  </label>
                  <label>
                    {say("确认 PIN", "CONFIRM PIN")}
                    <input
                      type="password"
                      inputMode="numeric"
                      autoComplete="off"
                      pattern="[0-9]{6}"
                      maxLength={6}
                      required
                      value={pinConfirm}
                      onChange={(e) =>
                        setPinConfirm(
                          e.target.value.replace(/\D/g, "").slice(0, 6),
                        )
                      }
                    />
                  </label>
                  <p className="account-description">
                    {say(
                      "离开终端后，用此 PIN 解锁。PIN 与邮箱验证码不同。",
                      "Use this PIN to unlock the terminal. It is separate from email verification.",
                    )}
                  </p>
                </>
              )}
              {(screen === "verify" || (screen === "reset" && resetSent)) && (
                <>
                  <p className="account-description">
                    {localMode
                      ? say(
                          "本地模式：输入任意 6 位数字即可完成验证，无需收取邮件。",
                          "Local mode: enter any 6 digits. No email is required.",
                        )
                      : t.verifyInfo}
                  </p>
                  <label>
                    {t.digits}
                    <input
                      className="account-code"
                      type="text"
                      inputMode="numeric"
                      autoComplete="one-time-code"
                      maxLength={6}
                      pattern="[0-9]{6}"
                      required
                      value={code}
                      onChange={(e) =>
                        setCode(e.target.value.replace(/\D/g, "").slice(0, 6))
                      }
                    />
                  </label>
                </>
              )}
              {screen === "reset" && !resetSent && (
                <p className="account-description">
                  {localMode
                    ? say(
                        "输入本地账号邮箱以重置密码。",
                        "Enter your local account email to reset your password.",
                      )
                    : t.resetInfo}
                </p>
              )}
              {notice && (
                <p role="status" className="account-description">
                  {notice}
                </p>
              )}
              {error && (
                <p role="alert" className="account-error">
                  {error}
                </p>
              )}
              <div
                className={
                  screen === "login"
                    ? "account-submit-row"
                    : "account-submit-full"
                }
              >
                {screen === "login" && (
                  <button
                    type="button"
                    className="account-link"
                    onClick={() => navigate("reset")}
                  >
                    {t.forgot}
                  </button>
                )}
                <button
                  type="submit"
                  className="account-primary"
                  disabled={!token}
                >
                  {busy
                    ? "…"
                    : screen === "reset"
                      ? resetSent
                        ? t.reset
                        : t.send
                      : t[screen]}
                </button>
              </div>
            </fieldset>
          </form>
          {screen === "login" && (
            <>
              <div className="account-switch">
                <span>{t.noAccount}</span>
                <button disabled={busy} onClick={() => navigate("register")}>
                  {t.signUp}
                </button>
              </div>
              <button
                disabled={busy}
                className="account-verify-link"
                onClick={() => navigate("verify")}
              >
                {t.verifyLink}
              </button>
            </>
          )}
          {screen === "register" && (
            <div className="account-switch">
              <span>{t.hasAccount}</span>
              <button disabled={busy} onClick={() => navigate("login")}>
                {t.login}
              </button>
            </div>
          )}
          {(screen === "verify" || (screen === "reset" && resetSent)) && (
            <button
              className="account-secondary"
              disabled={busy || cooldown > 0 || !email}
              onClick={resend}
            >
              {cooldown > 0 ? `${t.resend} (${cooldown}s)` : t.resend}
            </button>
          )}
          {(screen === "verify" || screen === "reset") && (
            <button
              disabled={busy}
              className="account-secondary"
              onClick={() => navigate("login")}
            >
              {t.back}
            </button>
          )}
        </section>
        {localMode && (
          <div className="account-local">
            <p>
              {say(
                "本地账号 · 无需邮件服务",
                "LOCAL ACCOUNT · NO EMAIL REQUIRED",
              )}
            </p>
          </div>
        )}
      </div>
    </main>
  );
}
