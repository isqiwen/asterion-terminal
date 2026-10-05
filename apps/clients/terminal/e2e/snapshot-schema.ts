import { expect } from "./test";
import { createGenerator } from "ts-json-schema-generator";
import Ajv, { type ValidateFunction } from "ajv";
import { resolve } from "node:path";
import type { Snapshot } from "../src/bridge/client";

const validators = new Map<string, ValidateFunction>();
export function snapshotValidator(type: "Snapshot" | "SnapshotUnchanged") {
  let validate = validators.get(type);
  if (!validate) {
    const schema = createGenerator({
      path: resolve(__dirname, "../src/bridge/client.ts"),
      tsconfig: resolve(__dirname, "../tsconfig.json"),
      type,
      skipTypeCheck: true,
      additionalProperties: false,
    }).createSchema(type);
    validate = new Ajv({ allErrors: true, strict: false }).compile(schema);
    validators.set(type, validate);
  }
  return validate;
}

export function checkSnapshot(label: string, value: unknown): asserts value is Snapshot {
  const validate = snapshotValidator("Snapshot");
  expect(validate(value), `${label}: ${JSON.stringify(validate.errors, null, 1)}`).toBe(true);
}
