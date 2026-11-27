import { useCallback, useState } from "react";
import { translate } from "../i18n";
import { BackendError, ErrorNotice, asDisplayError, type DisplayError } from "../i18n/errors";
import { Icon } from "../ui/Icon";
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
  const fail = useCallback((reason: unknown) => {
    setError(asDisplayError(reason));
  }, []);
  const clear = useCallback(() => setError(""), []);
  return { error, attempt, fail, clear };
}

function isServiceFailure(error: DisplayError) {
  return (
    error instanceof BackendError &&
    (error.code === "unavailable" ||
      error.code === "internal_error" ||
      error.diagnostic === "unknown Protobuf field" ||
      /^(market|warehouse|trading) protocol differs from this Terminal$/.test(error.diagnostic))
  );
}

export function ActionError({
  error,
  namespace,
  onDismiss,
  action,
}: {
  error: DisplayError;
  namespace?: string;
  onDismiss?: () => void;
  action?: { label: string; onClick: () => void };
}) {
  const service = isServiceFailure(error);
  useErrorNotification(service ? error : "", namespace);
  if (!error || service) return null;
  if (!onDismiss && !action) {
    return (
      <p className="action-error" role="alert">
        <ErrorNotice error={error} namespace={namespace} />
      </p>
    );
  }
  return (
    <div className="action-error is-dismissible" role="alert">
      <div className="action-error-body">
        <ErrorNotice error={error} namespace={namespace} />
        {action && (
          <button type="button" className="action-error-link" onClick={action.onClick}>
            {action.label}
          </button>
        )}
      </div>
      {onDismiss && (
        <button
          type="button"
          className="action-error-dismiss"
          aria-label={translate("host", "关闭")}
          onClick={onDismiss}
        >
          <Icon name="close" size={12} />
        </button>
      )}
    </div>
  );
}
