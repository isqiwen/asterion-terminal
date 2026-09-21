import { useLayoutEffect, useMemo, useSyncExternalStore } from "react";
import {
  createRequestScope,
  currentSessionRevision,
  subscribeSession,
} from "./client";
import type { RequestGrant } from "./requests";
export function useRequestClient(
  token: string,
  connected: boolean,
  grants: readonly RequestGrant[],
  scopeId: string,
) {
  const revision = useSyncExternalStore(
    subscribeSession,
    currentSessionRevision,
  );
  const scope = useMemo(
    () => createRequestScope(token, connected, grants, scopeId),
    [token, connected, grants, scopeId, revision],
  );
  useLayoutEffect(() => {
    scope.activate();
    return () => scope.revoke();
  }, [scope]);
  return scope.client;
}
