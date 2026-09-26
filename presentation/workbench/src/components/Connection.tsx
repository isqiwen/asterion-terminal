import { useState } from "react";
import { nativeDesktop } from "@asterion/desktop-bridge/desktop";
export function Connection({
  onConnect,
}: {
  onConnect: (token: string) => void;
}) {
  const [draft, setDraft] = useState("");
  if (nativeDesktop) return null;
  return (
    <form
      className="developer-connection"
      onSubmit={(e) => {
        e.preventDefault();
        onConnect(draft);
        setDraft("");
      }}
    >
      <span>开发连接</span>
      <input
        aria-label="会话令牌"
        type="password"
        value={draft}
        onChange={(e) => setDraft(e.target.value)}
        required
      />
      <button>连接</button>
    </form>
  );
}
