import { describe, expect, test } from "bun:test";
import {
  decodeUsageResponse,
  normalizeUsage,
  validateWorkspace,
} from "./usage.ts";

const sample = {
  rollingUsage: {
    usage: 0,
    limit: 1_200_000_000,
    usagePercent: 0,
    resetInSec: 18000,
  },
  weeklyUsage: {
    usage: 24_876_543,
    limit: 3_000_000_000,
    usagePercent: 0.8,
    resetInSec: 400000,
  },
  monthlyUsage: {
    usage: 2_640_123_456,
    limit: 6_000_000_000,
    usagePercent: 44,
    resetInSec: 2000000,
  },
};

describe("official usage", () => {
  test("converts all three periods without deriving dollars from rounded percentages", () => {
    const frame = normalizeUsage(sample, 1_000_000);
    expect(frame.weekly.used).toBe(0.24876543);
    expect(frame.monthly.used).toBe(26.40123456);
    expect(frame.rolling.limit).toBe(12);
    expect(frame.weekly.limit).toBe(30);
    expect(frame.monthly.limit).toBe(60);
    expect(frame.updatedAt).toBe(1000);
  });

  test("allows legitimate exhausted and increased quotas", () => {
    const frame = normalizeUsage({
      ...sample,
      rollingUsage: {
        usage: 2_401_000_000,
        limit: 2_400_000_000,
        usagePercent: 100,
        resetInSec: 0,
      },
    });
    expect(frame.rolling.limit).toBe(24);
    expect(frame.rolling.used).toBe(24.01);
  });

  test.each([
    null,
    {},
    { ...sample, weeklyUsage: null },
    { ...sample, monthlyUsage: { ...sample.monthlyUsage, limit: 0 } },
    { ...sample, rollingUsage: { ...sample.rollingUsage, usage: -1 } },
    { ...sample, weeklyUsage: { ...sample.weeklyUsage, usagePercent: 99 } },
  ])("rejects invalid or incomplete snapshots %#", (input) => {
    expect(() => normalizeUsage(input)).toThrow();
  });

  test("reads current Seroval response without evaluating any script", () => {
    const records = Object.entries(sample)
      .map(
        ([key, value], index) =>
          `${key}:$R[${index}]={status:"ok",${Object.entries(value)
            .map(([k, v]) => `${k}:${v}`)
            .join(",")}}`,
      )
      .join(",");
    const text = `;0x000001ff;(($R)=>({${records}}))($R);throw Error('must not execute')`;
    expect(
      normalizeUsage(decodeUsageResponse(text, "text/javascript")).monthly.used,
    ).toBe(26.40123456);
    expect(() => decodeUsageResponse(text + text, "text/javascript")).toThrow();
    expect(() =>
      decodeUsageResponse(
        text.replace("usage:0", "usage:evil()"),
        "text/javascript",
      ),
    ).toThrow();
  });

  test("accepts JSON while rejecting HTML and unknown serialization", () => {
    expect(
      normalizeUsage(
        decodeUsageResponse(JSON.stringify(sample), "application/json"),
      ).weekly.percent,
    ).toBe(0.8);
    expect(() =>
      decodeUsageResponse("<html>login</html>", "text/html"),
    ).toThrow();
    expect(() =>
      decodeUsageResponse("changedFormat()", "text/javascript"),
    ).toThrow();
  });

  test("rejects a workspace URL injected into the ID", () => {
    expect(validateWorkspace("wrk_0123456789ABCDEF")).toBe(
      "wrk_0123456789ABCDEF",
    );
    expect(() => validateWorkspace("../auth")).toThrow();
  });
});
