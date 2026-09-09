import { describe, expect, test } from "bun:test";
import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { createServer, Socket } from "node:net";
import { tmpdir } from "node:os";
import { join } from "node:path";

import { extractAuthCookie, login } from "./auth/login.ts";
import { protect, unprotect } from "./auth/protection.ts";
import {
  createSessionStore,
  type TextProtector,
} from "./auth/session-store.ts";
import type { StoredSession } from "./auth/types.ts";

type FakeMarionette = {
  port: number;
  commands(): Promise<string[]>;
  close(): Promise<void>;
};

const fakeMarionetteProgram = String.raw`
const { createServer } = require("node:net");
const { appendFileSync } = require("node:fs");

const frame = (value) => {
  const body = Buffer.from(JSON.stringify(value), "utf8");
  return Buffer.concat([Buffer.from(String(body.length) + ":", "ascii"), body]);
};

const port = Number(process.argv[1]);
if (!Number.isInteger(port) || port < 1) process.exit(1);
const logPath = process.argv[2];
if (!logPath) process.exit(1);

let selectedHandle = "original-tab";
const server = createServer((socket) => {
  socket.on("error", () => {});
  socket.write(frame({ applicationType: "gecko", marionetteProtocol: 3 }));
  let buffered = Buffer.alloc(0);
  socket.on("data", (chunk) => {
    buffered = Buffer.concat([buffered, chunk]);
    while (true) {
      const colon = buffered.indexOf(0x3a);
      if (colon < 0) return;
      const length = Number(buffered.subarray(0, colon).toString("ascii"));
      if (!Number.isSafeInteger(length) || length < 1 || buffered.length < colon + 1 + length) return;
      const request = JSON.parse(buffered.subarray(colon + 1, colon + 1 + length).toString("utf8"));
      buffered = buffered.subarray(colon + 1 + length);
      if (!Array.isArray(request) || request[0] !== 0 || typeof request[1] !== "number" || typeof request[2] !== "string") {
        socket.destroy();
        return;
      }
      appendFileSync(logPath, request[2] + "\n", "utf8");
      let result = null;
      switch (request[2]) {
        case "WebDriver:NewSession":
          result = { value: { sessionId: "test-session", capabilities: {} } };
          break;
        case "WebDriver:GetWindowHandle":
          result = { value: selectedHandle };
          break;
        case "WebDriver:GetWindowHandles":
          result = ["original-tab", "go-tab"];
          break;
        case "WebDriver:SwitchToWindow":
          if (request[3] && typeof request[3].handle === "string") selectedHandle = request[3].handle;
          result = { value: null };
          break;
        case "WebDriver:GetCurrentURL":
          result = { value: selectedHandle === "go-tab" ? "https://opencode.ai/workspace/wrk_test/go" : "https://example.invalid/" };
          break;
        case "WebDriver:GetCookies":
          result = [{ name: "auth", value: "test-value", domain: "opencode.ai", expiry: 2000000000 }];
          break;
      }
      socket.write(frame([1, request[1], null, result]));
    }
  });
});
server.listen(port, "127.0.0.1");
setTimeout(() => process.exit(1), 10000);
`;

function delay(milliseconds: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}

async function reservePort(): Promise<number> {
  const server = createServer();
  await new Promise<void>((resolve, reject) => {
    server.once("error", reject);
    server.listen(0, "127.0.0.1", resolve);
  });
  const address = server.address();
  if (!address || typeof address === "string")
    throw new Error("test server address unavailable");
  await new Promise<void>((resolve, reject) =>
    server.close((error) => (error ? reject(error) : resolve())),
  );
  return address.port;
}

function canConnect(port: number): Promise<boolean> {
  return new Promise<boolean>((resolve) => {
    const socket = new Socket();
    const timer = setTimeout(() => {
      socket.destroy();
      resolve(false);
    }, 100);
    socket.once("connect", () => {
      clearTimeout(timer);
      socket.destroy();
      resolve(true);
    });
    socket.once("error", () => {
      clearTimeout(timer);
      resolve(false);
    });
    socket.connect({ host: "127.0.0.1", port });
  });
}

async function waitForFakePort(port: number): Promise<void> {
  const deadline = Date.now() + 2_000;
  while (Date.now() < deadline) {
    if (await canConnect(port)) return;
    await delay(20);
  }
  throw new Error("fake Marionette did not start");
}

async function fakeMarionette(): Promise<FakeMarionette> {
  const port = await reservePort();
  const directory = await mkdtemp(join(tmpdir(), "opencode-marionette-"));
  const commandLog = join(directory, "commands.log");
  const node = Bun.which("node");
  if (!node) {
    await rm(directory, { recursive: true, force: true });
    throw new Error("Node.js is required for the Marionette fixture");
  }
  const child = Bun.spawn(
    [node, "-e", fakeMarionetteProgram, String(port), commandLog],
    {
      stdin: "ignore",
      stdout: "ignore",
      stderr: "ignore",
    },
  );
  try {
    await waitForFakePort(port);
  } catch (error) {
    child.kill();
    await child.exited;
    await rm(directory, { recursive: true, force: true });
    throw error;
  }

  return {
    port,
    async commands(): Promise<string[]> {
      try {
        return (await readFile(commandLog, "utf8"))
          .split(/\r?\n/)
          .filter((command) => command.length > 0);
      } catch (error) {
        if (
          error instanceof Error &&
          "code" in error &&
          error.code === "ENOENT"
        )
          return [];
        throw error;
      }
    },
    async close(): Promise<void> {
      child.kill();
      await child.exited;
      await rm(directory, { recursive: true, force: true });
    },
  };
}

function memoryProtector(): TextProtector {
  let protectedValue = "";
  return {
    async protect(value: string): Promise<string> {
      protectedValue = value;
      return "opaque-test-value";
    },
    async unprotect(value: string): Promise<string> {
      if (value !== "opaque-test-value")
        throw new Error("invalid protected value");
      return protectedValue;
    },
  };
}

describe("認証情報の保存", () => {
  test("Windows DPAPI は現在のユーザーとしてだけ保護値を復号する", async () => {
    const source = "dpapi-self-test";
    if (process.platform !== "win32") {
      await expect(protect(source)).rejects.toThrow("Windows DPAPI");
      return;
    }

    const protectedValue = await protect(source);
    expect(protectedValue).not.toContain(source);
    expect(await unprotect(protectedValue)).toBe(source);
  });

  test("暗号化済みの封筒だけを保存し、読み戻しと削除ができる", async () => {
    const directory = await mkdtemp(join(tmpdir(), "opencode-auth-"));
    try {
      const store = createSessionStore(directory, memoryProtector());
      const session: StoredSession = {
        version: 1,
        cookie: "auth=test-value",
        workspace: "wrk_test",
        expiresAt: 2_000_000_000_000,
      };
      await store.save(session);
      const disk = await readFile(join(directory, "session.json"), "utf8");
      expect(disk).not.toContain(session.cookie);
      expect(await store.load()).toEqual(session);
      await store.clear();
      expect(await store.load()).toBeUndefined();
    } finally {
      await rm(directory, { recursive: true, force: true });
    }
  });

  test("破損した保存情報は削除して再ログイン可能な状態へ戻す", async () => {
    const directory = await mkdtemp(join(tmpdir(), "opencode-auth-"));
    try {
      const store = createSessionStore(directory, memoryProtector());
      await writeFile(
        join(directory, "session.json"),
        JSON.stringify({ version: 1, protected: "broken" }),
        "utf8",
      );
      expect(await store.load()).toBeUndefined();
      await expect(
        readFile(join(directory, "session.json"), "utf8"),
      ).rejects.toMatchObject({ code: "ENOENT" });
    } finally {
      await rm(directory, { recursive: true, force: true });
    }
  });
});

describe("Firefox Marionette の認証取得", () => {
  test("既存 Firefox から opencode.ai の auth Cookie とワークスペースだけを取り込む", async () => {
    const firefox = await fakeMarionette();
    try {
      await expect(
        login({
          launch: false,
          port: firefox.port,
          workspace: "wrk_test",
          timeoutMs: 2_000,
        }),
      ).resolves.toEqual({
        version: 1,
        cookie: "auth=test-value",
        workspace: "wrk_test",
        expiresAt: 2_000_000_000_000,
      });
      expect(await firefox.commands()).toEqual([
        "WebDriver:NewSession",
        "WebDriver:GetWindowHandle",
        "WebDriver:GetWindowHandles",
        "WebDriver:SwitchToWindow",
        "WebDriver:GetCurrentURL",
        "WebDriver:SwitchToWindow",
        "WebDriver:GetCurrentURL",
        "WebDriver:GetCookies",
        "WebDriver:SwitchToWindow",
        "WebDriver:DeleteSession",
      ]);
    } finally {
      await firefox.close();
    }
  });

  test("auth Cookie は opencode.ai ドメイン以外から受け取らない", () => {
    expect(
      extractAuthCookie([
        { name: "auth", value: "test-value", domain: "example.invalid" },
      ]),
    ).toBeUndefined();
    expect(
      extractAuthCookie([
        { name: "auth", value: "test-value", domain: ".opencode.ai" },
      ]),
    ).toEqual({
      cookie: "auth=test-value",
    });
  });
});
