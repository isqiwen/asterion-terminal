import { useCallback, useState } from "react";
import { BackendError, ErrorNotice, asDisplayError, type DisplayError } from "../i18n/errors";
import { useErrorNotification } from "./notifications";

// Input and operation errors stay beside the action; service failures go to notifications.
export function useActionError() {
  const [error, setError] = useState<DisplayError>("");
  const attempt = useCallback(async (action: () => Promise<unknown>) => {
    setError("");
    try {
      await action();
      return true;
    } catch (reason) {
      setError(asDisplayError(reason));
      return false;
    }
  }, []);
  return { error, attempt, clear: () => setError("") };
}

export function ActionError({ error, namespace }: { error: DisplayError; namespace?: string }) {
  const inline = useRequestError(error, namespace);
  if (!inline) return null;
  return (
    <p className="action-error" role="alert">
      <ErrorNotice error={error} namespace={namespace} />
    </p>
  );
}

function useRequestError(error: DisplayError, namespace?: string) {
  const service =
    error instanceof BackendError &&
    (error.code === "unavailable" ||
      error.code === "internal_error" ||
      error.diagnostic === "unknown Protobuf field" ||
      /^(market|warehouse|trading) protocol differs from this Terminal$/.test(error.diagnostic));
  useErrorNotification(service ? error : "", namespace);
  return !!error && !service;
}

export function RequestError({ error, namespace }: { error: DisplayError; namespace?: string }) {
  const inline = useRequestError(error, namespace);
  return inline ? <ErrorNotice error={error} namespace={namespace} /> : null;
}
