import { schema, type Context } from "@asterion/api-types/wire.generated";

export function validateWire(name: string, value: unknown): void {
  const defs = schema.$defs as Record<string, any>;
  const visit = (s: any, v: any): void => {
    if (s.$ref) return visit(defs[s.$ref.split("/").at(-1)], v);
    if (s.anyOf) {
      for (const branch of s.anyOf) {
        try {
          visit(branch, v);
          return;
        } catch {}
      }
      throw new Error("通信字段不符合契约");
    }
    const actual =
      v === null
        ? "null"
        : Array.isArray(v)
          ? "array"
          : typeof v === "number"
            ? Number.isInteger(v)
              ? "integer"
              : "number"
            : typeof v;
    const types = typeof s.type === "string" ? [s.type] : s.type;
    if ((types && !types.includes(actual)) || (s.enum && !s.enum.includes(v)))
      throw new Error("通信类型或版本无效");
    if (
      typeof v === "string" &&
      (Array.from(v).length < (s.minLength ?? 0) ||
        Array.from(v).length > (s.maxLength ?? Infinity))
    )
      throw new Error("通信字段长度无效");
    if (
      typeof v === "number" &&
      (!Number.isFinite(v) ||
        (Number.isInteger(v) && !Number.isSafeInteger(v)) ||
        v < (s.minimum ?? -Infinity) ||
        v > (s.maximum ?? Infinity))
    )
      throw new Error("通信数值无效");
    if (actual === "object") {
      const props = s.properties ?? {};
      if (
        (s.required ?? []).some((k: string) => !Object.hasOwn(v, k)) ||
        (s.additionalProperties === false &&
          Object.keys(v).some((k) => !Object.hasOwn(props, k)))
      )
        throw new Error("通信字段缺失或未声明");
      Object.entries(v).forEach(([k, item]) => visit(props[k] ?? {}, item));
    } else if (actual === "array")
      v.forEach((item: unknown) => visit(s.items ?? {}, item));
    else if (
      !["null", "string", "number", "integer", "boolean"].includes(actual)
    )
      throw new Error("通信值不可序列化");
  };
  if (!defs[name]) throw new Error("通信契约未登记");
  visit(defs[name], value);
}

export function createContext(timeout: number, parent?: Context): Context {
  const requestId = crypto.randomUUID().replaceAll("-", "");
  const context: Context = {
    version: 1,
    request_id: requestId,
    correlation_id: parent?.correlation_id ?? requestId,
    causation_id: parent?.request_id ?? null,
    deadline_ms: Math.min(
      Date.now() + timeout,
      parent?.deadline_ms ?? Infinity,
    ),
  };
  validateWire("Context", context);
  return context;
}

export function communicationHeaders(
  timeout: number,
  parent?: Context,
): Record<string, string> {
  return {
    "X-Asterion-Context": JSON.stringify(createContext(timeout, parent)),
  };
}
