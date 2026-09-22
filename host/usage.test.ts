import { describe, expect, test } from "bun:test";
import {
  decodeUsageResponse,
  normalizeUsage,
  validateWorkspace,
} from "./usage.ts";

const sample = {
  access: {
    endsAt: "2026-10-02T22:58:39.000Z",
    meters: {
      fiveHour: {
        startsAt: "2026-09-22T06:34:36.282Z",
        resetsAt: "2026-09-22T11:34:36.282Z",
        limitMicroCents: "1200000000",
        usedMicroCents: "3647255",
      },
      week: {
        startsAt: "2026-09-21T00:00:00.000Z",
        resetsAt: "2026-09-28T00:00:00.000Z",
        limitMicroCents: "3000000000",
        usedMicroCents: "3647255",
      },
      month: {
        limitMicroCents: "6000000000",
        usedMicroCents: "3980909783",
      },
    },
  },
};
// 2026-09-22T06:34:36.282Z と同じ瞬間。fiveHour の残りはちょうど5時間。
const now = Date.parse("2026-09-22T06:34:36.282Z");

describe("official usage", () => {
  test("converts all three meters from microCents to dollars", () => {
    const frame = normalizeUsage(sample, now);
    expect(frame.rolling.used).toBeCloseTo(0.03647255, 6);
    expect(frame.rolling.limit).toBe(12);
    expect(frame.rolling.percent).toBeCloseTo(0.3039, 3);
    expect(frame.rolling.resetInSec).toBe(18000);
    expect(frame.weekly.limit).toBe(30);
    expect(frame.monthly.used).toBeCloseTo(39.80909783, 6);
    expect(frame.monthly.limit).toBe(60);
    expect(frame.monthly.percent).toBeCloseTo(66.348, 2);
    // 月間は契約の endsAt まで: 10日16時間24分02.718秒の切り上げ。
    expect(frame.monthly.resetInSec).toBe(923043);
    expect(frame.updatedAt).toBe(Math.floor(now / 1000));
  });

  test("allows legitimate exhausted quotas", () => {
    const frame = normalizeUsage(
      {
        access: {
          endsAt: new Date(now).toISOString(),
          meters: {
            fiveHour: {
              resetsAt: new Date(now).toISOString(),
              limitMicroCents: "2400000000",
              usedMicroCents: "2401000000",
            },
            week: {
              resetsAt: new Date(now).toISOString(),
              limitMicroCents: 3000000000,
              usedMicroCents: 0,
            },
            month: {
              limitMicroCents: 6000000000,
              usedMicroCents: 6000000000,
            },
          },
        },
      },
      now,
    );
    expect(frame.rolling.limit).toBe(24);
    expect(frame.rolling.used).toBe(24.01);
    expect(frame.rolling.resetInSec).toBe(0);
  });

  test.each([
    null,
    {},
    { ...sample, access: null },
    { access: {} },
    {
      access: { ...sample.access, meters: null },
    },
    {
      access: {
        ...sample.access,
        meters: { ...sample.access.meters, week: null },
      },
    },
    {
      access: {
        ...sample.access,
        meters: {
          ...sample.access.meters,
          month: { ...sample.access.meters.month, limitMicroCents: "0" },
        },
      },
    },
    {
      access: {
        ...sample.access,
        meters: {
          ...sample.access.meters,
          fiveHour: {
            ...sample.access.meters.fiveHour,
            usedMicroCents: "-1",
          },
        },
      },
    },
    {
      access: {
        ...sample.access,
        meters: {
          ...sample.access.meters,
          week: { ...sample.access.meters.week, resetsAt: "not-a-date" },
        },
      },
    },
  ])("rejects invalid or incomplete snapshots %#", (input) => {
    expect(() => normalizeUsage(input)).toThrow();
  });

  test("accepts JSON while rejecting HTML and unknown serialization", () => {
    expect(
      normalizeUsage(
        decodeUsageResponse(JSON.stringify(sample), "application/json"),
        now,
      ).weekly.limit,
    ).toBe(30);
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
