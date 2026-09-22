import { expect, test } from "bun:test";
import { mkdtemp, readFile, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { DeviceConnection } from "./device.ts";
import type { UsageFrame } from "./usage.ts";

const fixture = fileURLToPath(
  new URL("./test-fixtures/serial-worker.ts", import.meta.url),
);
const frame: UsageFrame = {
  version: 1,
  type: "usage",
  updatedAt: 1,
  rolling: { used: 0, limit: 12, percent: 0, resetInSec: 10 },
  weekly: { used: 0, limit: 30, percent: 0, resetInSec: 20 },
  monthly: { used: 0, limit: 60, percent: 0, resetInSec: 30 },
};

test("USB config waits for its own ACK and errors clear replayed usage", async () => {
  const directory = await mkdtemp(join(tmpdir(), "lcd-worker-"));
  const log = join(directory, "frames.jsonl");
  const device = new DeviceConnection(
    () => ["node", fixture, log],
    async () => [{ path: "TEST", label: "fake" }],
  );
  try {
    await device.connect("TEST");
    await device.send(frame);
    const started = Date.now();
    await device.send({
      version: 1,
      type: "config",
      enabled: false,
      ssid: "",
      password: "",
      authCookie: "",
      workspace: "",
      queryId: "",
      pollIntervalSec: 60,
    });
    expect(Date.now() - started).toBeGreaterThanOrEqual(100);
    await device.send({ version: 1, type: "error", message: "expired" });
    await device.close();
    const withoutHandshake = (text: string) =>
      text
        .split("\n")
        .filter(
          (line) =>
            line && (JSON.parse(line) as { type: string }).type !== "ping",
        );
    const before = withoutHandshake(await readFile(log, "utf8"));
    await device.connect("TEST");
    await Bun.sleep(100);
    await device.close();
    expect(withoutHandshake(await readFile(log, "utf8"))).toEqual(before);
    expect(device.status().connected).toBe(false);
  } finally {
    await device.close();
    await rm(directory, { recursive: true, force: true });
  }
});

test("a worker exiting during write rejects and can be closed repeatedly without EPIPE", async () => {
  const directory = await mkdtemp(join(tmpdir(), "lcd-worker-"));
  const device = new DeviceConnection(
    () => ["node", fixture, join(directory, "unused")],
    async () => [{ path: "BREAK", label: "fake" }],
  );
  try {
    await device.connect("BREAK");
    await expect(device.send(frame)).rejects.toThrow();
    await device.close();
    await device.close();
    expect(device.status().connected).toBe(false);
  } finally {
    await device.close();
    await rm(directory, { recursive: true, force: true });
  }
});

test("credentials are never sent to a device without the setup firmware handshake", async () => {
  const directory = await mkdtemp(join(tmpdir(), "lcd-worker-"));
  const log = join(directory, "frames.jsonl");
  const device = new DeviceConnection(
    () => ["node", fixture, log],
    async () => [{ path: "WRONG", label: "fake" }],
  );
  try {
    await device.connect("WRONG");
    await expect(
      device.send({
        version: 1,
        type: "config",
        enabled: true,
        ssid: "test",
        password: "testpass",
        authCookie: "__Host-console_session=must-stay-on-host",
        workspace: "wrk_0123456789",
        queryId: "a".repeat(64),
        pollIntervalSec: 60,
      }),
    ).rejects.toThrow("ファームウェア");
    await device.close();
    expect(await readFile(log, "utf8")).not.toContain("must-stay-on-host");
  } finally {
    await device.close();
    await rm(directory, { recursive: true, force: true });
  }
});

test("display settings and wake each wait for their corresponding ACK", async () => {
  const directory = await mkdtemp(join(tmpdir(), "lcd-worker-"));
  const device = new DeviceConnection(
    () => ["node", fixture, join(directory, "frames.jsonl")],
    async () => [{ path: "TEST", label: "fake" }],
  );
  try {
    await device.connect("TEST");
    for (const frame of [
      {
        version: 1 as const,
        type: "display" as const,
        backlightTimeoutSec: 15,
      },
      { version: 1 as const, type: "wake" as const },
    ]) {
      const started = Date.now();
      await device.send(frame);
      expect(Date.now() - started).toBeGreaterThanOrEqual(100);
    }
  } finally {
    await device.close();
    await rm(directory, { recursive: true, force: true });
  }
});
