import { useCallback, useState } from "react";
import { ErrorNotice, asDisplayError, type DisplayError } from "../i18n/errors";

// Why an action the user just took failed, shown beside where they took it.
// The notification centre is for what happens without them.
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
  if (!error) return null;
  return (
    <p className="action-error" role="alert">
      <ErrorNotice error={error} namespace={namespace} />
    </p>
  );
}
