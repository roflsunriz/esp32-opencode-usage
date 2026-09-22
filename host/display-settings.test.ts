import { expect, test } from "bun:test";
import {
  backlightTimeouts,
  defaultBacklightTimeout,
  parseBacklightTimeout,
} from "./display-settings.ts";

test("allows legacy presets and slider range values", () => {
  expect(backlightTimeouts).toEqual([
    15, 30, 60, 120, 300, 600, 1800, 3600, 7200,
  ]);
  expect(defaultBacklightTimeout).toBe(60);
  for (const value of backlightTimeouts)
    expect(parseBacklightTimeout(value)).toBe(value);
  expect(parseBacklightTimeout(0)).toBe(0);
  expect(parseBacklightTimeout(150)).toBe(150);
  expect(parseBacklightTimeout(89940)).toBe(89940);
});

test.each([undefined, null, "60", -1, 14.5, 89941, NaN, Infinity])(
  "rejects unsupported display timeout %#",
  (value) => {
    expect(() => parseBacklightTimeout(value)).toThrow();
  },
);
