import { describe, expect, test } from "bun:test";
import { UsageService } from "./service.ts";
import { startServer } from "./server.ts";

describe("local control and device access", () => {
  test("protects local setup controls and rejects cross-origin writes", async () => {
    const service = new UsageService();
    const server = startServer(service, {
      port: 0,
    });
    const origin = `http://127.0.0.1:${server.port}`;
    try {
      const page = await fetch(origin);
      expect(page.status).toBe(200);
      expect(page.headers.get("content-security-policy")).toContain(
        "frame-ancestors 'none'",
      );
      const text = await page.text();
      expect(text).toContain("日本語");
      expect(text).not.toContain("\\u65E5");
      const control = /const controlToken="([a-f0-9]+)"/.exec(text)?.[1];
      expect(control).toHaveLength(64);
      expect((await fetch(origin + "/api/status")).status).toBe(403);
      const status = await fetch(origin + "/api/status", {
        headers: { "X-Control-Token": control! },
      });
      expect(status.status).toBe(200);
      expect((await status.json()).authenticated).toBe(false);
      expect(
        (
          await fetch(origin + "/api/refresh", {
            method: "POST",
            headers: {
              "Content-Type": "application/json",
              "X-Control-Token": control!,
              Origin: "https://evil.invalid",
            },
            body: "{}",
          })
        ).status,
      ).toBe(403);
      expect(
        (
          await fetch(origin + "/api/setup", {
            method: "POST",
            headers: {
              "Content-Type": "application/json",
              "X-Control-Token": control!,
            },
            body: JSON.stringify({
              ssid: "test",
              password: "testtest",
              pollIntervalSec: 60,
            }),
          })
        ).status,
      ).toBe(400);
      expect(
        (await fetch(origin, { headers: { Host: "attacker.invalid" } })).status,
      ).toBe(403);
    } finally {
      server.stop(true);
      await service.stop();
    }
  });
});
