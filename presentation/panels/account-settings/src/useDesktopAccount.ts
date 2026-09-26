import { useEffect, useState } from "react";
import { nativeDesktop } from "@asterion/desktop-bridge/desktop";
import {
  AccountError,
  accountRequest,
  clearAccountSession,
  clearSharedSession,
  readSharedSession,
  useSharedToken,
  type AccountUser,
} from "./client";
export function useDesktopAccount(token: string) {
  const [account, setAccount] = useState<AccountUser>();
  const [restoring, setRestoring] = useState(nativeDesktop);
  useEffect(() => {
    if (!nativeDesktop || !token) return;
    let active = true;
    let busy = false;
    let revision = -1;
    let checkedAt = 0;
    async function sync() {
      if (busy) return;
      busy = true;
      try {
        const shared = await readSharedSession();
        if (!active) return;
        if (!shared.token) {
          clearAccountSession();
          setAccount(undefined);
          revision = shared.revision;
        } else if (
          revision !== shared.revision ||
          Date.now() - checkedAt > 60000
        ) {
          useSharedToken(shared.token);
          try {
            const user = await accountRequest<AccountUser>("/me", token);
            const current = await readSharedSession();
            if (active && current.revision === shared.revision) {
              setAccount(user);
              revision = shared.revision;
              checkedAt = Date.now();
            }
          } catch (e) {
            if (e instanceof AccountError && e.code === "SESSION_EXPIRED") {
              await clearSharedSession(shared.revision).catch(() => {});
              if (active) {
                clearAccountSession();
                setAccount(undefined);
              }
            }
          }
        }
      } finally {
        if (active) setRestoring(false);
        busy = false;
      }
    }
    void sync().catch(() => {});
    const interval = setInterval(() => void sync().catch(() => {}), 1000);
    return () => {
      active = false;
      clearInterval(interval);
    };
  }, [token]);
  return { account, setAccount, restoring };
}
