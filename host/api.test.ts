import { describe, expect, test } from "bun:test";
import { OpenCodeClient } from "./api.ts";

const workspace = "wrk_0123456789ABCDEF";
const cookie = "__Host-console_session=test";
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

describe("OpenCode transport", () => {
  test("fetches go status with the session cookie and org header", async () => {
    const calls: { url: string; init?: RequestInit }[] = [];
    const fake = (async (input: string | URL | Request, init?: RequestInit) => {
      const url = String(input);
      calls.push({ url, init });
      return Response.json(sample);
    }) as typeof fetch;
    const frame = await new OpenCodeClient(cookie, workspace, fake).usage();
    expect(frame.rolling.used).toBeCloseTo(0.03647255, 6);
    expect(frame.rolling.limit).toBe(12);
    expect(frame.monthly.limit).toBe(60);
    expect(calls).toHaveLength(1);
    expect(calls[0]?.url).toBe("https://opencode.ai/console/api/go/status");
    const headers = new Headers(calls[0]?.init?.headers);
    expect(headers.get("Cookie")).toBe(cookie);
    expect(headers.get("x-org-id")).toBe(workspace);
    expect(calls[0]?.init?.redirect).toBe("manual");
  });

  test("does not follow authentication redirects or forward credentials elsewhere", async () => {
    let count = 0;
    const fake = (async () => {
      count++;
      return new Response(null, {
        status: 302,
        headers: { Location: "https://other.invalid/" },
      });
    }) as unknown as typeof fetch;
    await expect(
      new OpenCodeClient(cookie, workspace, fake).usage(),
    ).rejects.toThrow("再ログイン");
    expect(count).toBe(1);
    await expect(
      new OpenCodeClient(
        cookie,
        workspace,
        (async () =>
          new Response(null, { status: 401 })) as unknown as typeof fetch,
      ).usage(),
    ).rejects.toThrow("再ログイン");
    expect(
      () => new OpenCodeClient("auth=test\r\nX-Bad: 1", workspace, fake),
    ).toThrow();
    expect(() => new OpenCodeClient("auth=test", workspace, fake)).toThrow();
  });

  test("fails visibly for missing Go data instead of treating it as zero", async () => {
    const fake = (async () => Response.json(null)) as unknown as typeof fetch;
    await expect(
      new OpenCodeClient(cookie, workspace, fake).usage(),
    ).rejects.toThrow();
  });
});
