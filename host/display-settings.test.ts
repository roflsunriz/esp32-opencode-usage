import { expect, test } from "bun:test";
import {
  backlightTimeouts,
  defaultBacklightTimeout,
  parseBacklightTimeout,
} from "./display-settings.ts";

test("allows exactly the requested nine timeouts", () => {
  expect(backlightTimeouts).toEqual([
    15, 30, 60, 120, 300, 600, 1800, 3600, 7200,
  ]);
  expect(defaultBacklightTimeout).toBe(60);
  for (const value of backlightTimeouts)
    expect(parseBacklightTimeout(value)).toBe(value);
});

test.each([
  undefined,
  null,
  "60",
  0,
  -1,
  14,
  16,
  90,
  7199,
  7201,
  NaN,
  Infinity,
])("rejects unsupported display timeout %#", (value) => {
  expect(() => parseBacklightTimeout(value)).toThrow();
});
