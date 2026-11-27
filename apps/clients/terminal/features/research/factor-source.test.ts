import { expect, test } from "vitest";
import { factorFunctionSnippet, factorSourceChanged } from "./factor-source";

const MODULE = `"""Demo factor module."""
import numpy as np

SCALE = 1.0

def _helper(x):
    return x * SCALE


@_scale_invariant
def alpha_a(close, window: int = 20):
    """First factor.
def not_a_real_def():
    pass
    """
    return _helper(close)


def alpha_b(close, window: int = 10):
    return close - window


def alpha_uses_b(close):
    return alpha_b(close) * 2
`;

test("editing or adding unrelated sibling factors does not flag drift for the target factor", () => {
  const editedSibling = MODULE.replace("return close - window", "return close - window * 2");
  expect(factorSourceChanged(MODULE, editedSibling, "demo.alpha_a")).toBe(false);
  expect(factorSourceChanged(MODULE, editedSibling, "demo.alpha_b")).toBe(true);

  const addedSibling = `${MODULE}\ndef alpha_c(close):\n    return close\n`;
  expect(factorSourceChanged(MODULE, addedSibling, "demo.alpha_a")).toBe(false);
});

test("editing the target factor, its decorator, shared helpers, constants, or called siblings flags drift", () => {
  expect(
    factorSourceChanged(MODULE, MODULE.replace("@_scale_invariant\n", ""), "demo.alpha_a"),
  ).toBe(true);
  expect(
    factorSourceChanged(MODULE, MODULE.replace("return x * SCALE", "return x * 2"), "demo.alpha_a"),
  ).toBe(true);
  expect(
    factorSourceChanged(MODULE, MODULE.replace("SCALE = 1.0", "SCALE = 2.0"), "demo.alpha_a"),
  ).toBe(true);
  expect(
    factorSourceChanged(
      MODULE,
      MODULE.replace("return close - window", "return close + window"),
      "demo.alpha_uses_b",
    ),
  ).toBe(true);
});

test("factorFunctionSnippet extracts only the evaluated function for multi-factor modules", () => {
  const snippet = factorFunctionSnippet(MODULE, "demo.alpha_a");
  expect(snippet).toContain("@_scale_invariant");
  expect(snippet).toContain("def alpha_a");
  expect(snippet).not.toContain("def alpha_b");
  expect(
    factorFunctionSnippet("def only_one(close):\n    return close\n", "m.only_one"),
  ).toBeNull();
});
