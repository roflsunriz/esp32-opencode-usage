import { describe, expect, test } from "bun:test";
import { UsageService } from "./service.ts";
import type { UsageFrame } from "./usage.ts";

const frame: UsageFrame = {
  version: 1,
  type: "usage",
  updatedAt: 1788950000,
  rolling: { used: 1, limit: 12, percent: 8.3, resetInSec: 100 },
  weekly: { used: 1, limit: 30, percent: 3.3, resetInSec: 200 },
  monthly: { used: 1, limit: 60, percent: 1.7, resetInSec: 300 },
};
const session = {
  version: 1 as const,
  cookie: "auth=test",
  workspace: "wrk_0123456789ABCDEF",
};

describe("usage service states", () => {
  test("keeps last known values for the UI but stops serving stale data as success", async () => {
    let fail = false;
    const service = new UsageService(() => ({
      usage: async () => {
        if (fail) throw new Error("upstream unavailable");
        return frame;
      },
    }));
    service.setSession(session);
    await service.refresh();
    expect(service.frame()).toEqual(frame);
    fail = true;
    await service.refresh();
    expect(service.frame()).toBeUndefined();
    expect(service.status().usage).toEqual(frame);
    expect(service.status().error).toBe("upstream unavailable");
    fail = false;
    await service.refresh();
    expect(service.status().error).toBeUndefined();
    expect(service.frame()).toEqual(frame);
    await service.stop();
  });

  test("coalesces concurrent refreshes and ignores an in-flight result after logout", async () => {
    let complete!: (value: UsageFrame) => void;
    let calls = 0;
    const pending = new Promise<UsageFrame>((resolve) => {
      complete = resolve;
    });
    const service = new UsageService(() => ({
      usage: () => {
        calls++;
        return pending;
      },
    }));
    service.setSession(session);
    const a = service.refresh();
    const b = service.refresh();
    expect(calls).toBe(1);
    service.setSession();
    complete(frame);
    await Promise.all([a, b]);
    expect(service.frame()).toBeUndefined();
    expect(service.status().authenticated).toBe(false);
    await service.stop();
  });
});
