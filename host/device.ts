import { randomBytes } from "node:crypto";
import { existsSync } from "node:fs";
import { fileURLToPath } from "node:url";
import type { UsageFrame } from "./usage.ts";

export interface DeviceConfig {
  version: 1;
  type: "config";
  enabled: boolean;
  ssid: string;
  password: string;
  authCookie: string;
  workspace: string;
  queryId: string;
  pollIntervalSec: number;
  backlightTimeoutSec?: number;
  requestId?: string;
}

type Worker = Bun.Subprocess<"pipe", "pipe", "ignore">;
type Completion = {
  resolve(): void;
  reject(error: Error): void;
  timer: ReturnType<typeof setTimeout>;
};

function workerCommand(): string[] {
  const python = Bun.which("python");
  if (!python)
    throw new Error(
      "USB接続にはPythonとpySerialが必要です。セットアップ手順を確認してください。",
    );
  const source = fileURLToPath(new URL("./serial-worker.py", import.meta.url));
  if (!existsSync(source))
    throw new Error("USB補助プログラムがありません。再ビルドしてください。");
  return [python, "-u", source];
}

export class DeviceConnection {
  constructor(
    private readonly command = workerCommand,
    private readonly ports = DeviceConnection.list,
  ) {}
  private worker?: Worker;
  private path?: string;
  private buffer = "";
  private connected = false;
  private reconnect?: ReturnType<typeof setTimeout>;
  private lastFrame?: UsageFrame;
  private stopped = false;
  private message = "USB未接続";
  private pendingConfig?: Completion & { requestId: string; accepted: string };
  private readonly pendingWrites = new Map<number, Completion>();
  private sequence = 0;
  private lastDirectUpdate?: number;
  private compatible = false;
  private backlightTimeoutSec?: number;
  private backlightOn?: boolean;

  clearCachedFrame(): void {
    this.lastFrame = undefined;
  }

  private finishConfig(error?: Error): void {
    const pending = this.pendingConfig;
    if (!pending) return;
    this.pendingConfig = undefined;
    clearTimeout(pending.timer);
    if (error) pending.reject(error);
    else pending.resolve();
  }

  status(): {
    path?: string;
    connected: boolean;
    compatible: boolean;
    message: string;
    lastDirectUpdate?: number;
    backlightTimeoutSec?: number;
    backlightOn?: boolean;
  } {
    return {
      path: this.path,
      connected: this.connected,
      compatible: this.compatible,
      message: this.message,
      lastDirectUpdate: this.lastDirectUpdate,
      backlightTimeoutSec: this.backlightTimeoutSec,
      backlightOn: this.backlightOn,
    };
  }

  static async list(): Promise<{ path: string; label: string }[]> {
    const child = Bun.spawn([...workerCommand(), "--list"], {
      stdout: "pipe",
      stderr: "ignore",
    });
    const timer = setTimeout(() => child.kill(), 10_000);
    try {
      const [output, code] = await Promise.all([
        new Response(child.stdout).text(),
        child.exited,
      ]);
      if (code !== 0)
        throw new Error(
          "USBポートの列挙に失敗しました。PythonとpySerialの導入を確認してください。",
        );
      const value: unknown = JSON.parse(output);
      if (
        !Array.isArray(value) ||
        value.some(
          (item: unknown) =>
            !item ||
            typeof item !== "object" ||
            !("path" in item) ||
            typeof item.path !== "string" ||
            !("label" in item) ||
            typeof item.label !== "string",
        )
      )
        throw new Error("USBポートの応答形式が不正です。");
      return value as { path: string; label: string }[];
    } finally {
      clearTimeout(timer);
    }
  }

  async connect(path: string): Promise<void> {
    if (!(await this.ports()).some((port) => port.path === path))
      throw new Error("指定されたUSBポートが見つかりません。");
    await this.close();
    this.stopped = false;
    this.path = path;
    await this.open();
  }

  private receiveSerial(chunk: string): void {
    this.buffer += chunk;
    if (this.buffer.length > 8192) this.buffer = this.buffer.slice(-4096);
    let index: number;
    while ((index = this.buffer.indexOf("\n")) >= 0) {
      const line = this.buffer.slice(0, index).trim();
      this.buffer = this.buffer.slice(index + 1);
      try {
        const value: unknown = JSON.parse(line);
        if (!value || typeof value !== "object") continue;
        const response = value as Record<string, unknown>;
        if (
          response.firmware === "opencode-go-lcd" &&
          response.setupSchema === 3
        )
          this.compatible = true;
        if (this.compatible) {
          if (typeof response.backlightTimeoutSec === "number")
            this.backlightTimeoutSec = response.backlightTimeoutSec;
          if (typeof response.backlightOn === "boolean")
            this.backlightOn = response.backlightOn;
        }
        if (response.type === "ready") {
          this.message = "LCD起動済み";
          if (this.lastFrame)
            void this.send(this.lastFrame).catch(() => undefined);
        } else if (response.type === "ack") {
          this.message = "LCD受信確認済み";
          if (
            response.accepted === "usage" &&
            response.source === "http" &&
            typeof response.updatedAt === "number"
          ) {
            this.lastDirectUpdate = response.updatedAt;
            this.message = "ESP32の直接更新を確認しました";
          }
          if (
            response.accepted === this.pendingConfig?.accepted &&
            response.requestId === this.pendingConfig?.requestId
          )
            this.finishConfig();
        } else if (response.type === "error") {
          this.message =
            "LCDが入力エラーを報告しました。設定を確認してください。";
          this.finishConfig(new Error(this.message));
        }
      } catch {
        /* Ignore boot ROM or other firmware text; never echo it. */
      }
    }
  }

  private failPending(): void {
    const error = new Error("USB通信が停止しました。接続を確認してください。");
    this.finishConfig(error);
    for (const pending of this.pendingWrites.values()) {
      clearTimeout(pending.timer);
      pending.reject(error);
    }
    this.pendingWrites.clear();
  }

  private async open(): Promise<void> {
    if (!this.path || this.stopped) return;
    this.compatible = false;
    this.buffer = "";
    const child = Bun.spawn([...this.command(), "--port", this.path], {
      stdin: "pipe",
      stdout: "pipe",
      stderr: "ignore",
    });
    this.worker = child;
    const opened = new Promise<void>((resolve, reject) => {
      const timer = setTimeout(() => {
        child.kill();
        reject(new Error("USBポートの接続がタイムアウトしました。"));
      }, 10_000);
      const read = async () => {
        let buffered = "";
        const reader = child.stdout.getReader();
        const decoder = new TextDecoder();
        try {
          while (true) {
            const next = await reader.read();
            if (next.done) break;
            buffered += decoder.decode(next.value, { stream: true });
            if (buffered.length > 32_768)
              throw new Error("USB worker response too large");
            let index: number;
            while ((index = buffered.indexOf("\n")) >= 0) {
              const line = buffered.slice(0, index);
              buffered = buffered.slice(index + 1);
              const message: unknown = JSON.parse(line);
              if (!message || typeof message !== "object") continue;
              const value = message as Record<string, unknown>;
              if (this.worker !== child) continue;
              if (value.type === "open") {
                clearTimeout(timer);
                this.connected = true;
                this.message = "USB接続済み";
                resolve();
              } else if (
                value.type === "data" &&
                typeof value.data === "string"
              )
                this.receiveSerial(value.data);
              else if (
                value.type === "written" &&
                typeof value.id === "number"
              ) {
                const pending = this.pendingWrites.get(value.id);
                if (pending) {
                  this.pendingWrites.delete(value.id);
                  clearTimeout(pending.timer);
                  pending.resolve();
                }
              } else if (value.type === "error") {
                this.message =
                  "USB通信エラー。ほかのアプリがポートを使用していないか確認してください。";
                this.failPending();
                child.kill();
                reject(new Error(this.message));
              }
            }
          }
        } catch {
          child.kill();
          reject(new Error("USB補助プロセスとの通信に失敗しました。"));
        } finally {
          clearTimeout(timer);
          reject(
            new Error(
              "USBポートを開けませんでした。PythonとUSB接続を確認してください。",
            ),
          );
        }
      };
      void read();
    });
    void child.exited.then(() => {
      if (this.worker !== child) return;
      this.connected = false;
      this.worker = undefined;
      this.failPending();
      this.message = this.stopped ? "USB未接続" : "USB切断。再接続待ち";
      this.scheduleReconnect();
    });
    await opened;
    if (!this.compatible) await this.send({ version: 1, type: "ping" });
    if (this.lastFrame) await this.send(this.lastFrame);
  }

  private scheduleReconnect(): void {
    if (this.stopped || this.reconnect) return;
    this.reconnect = setTimeout(() => {
      this.reconnect = undefined;
      void this.open().catch(() => {
        this.message = "USB再接続待ち";
      });
    }, 5000);
  }

  async send(
    frame:
      | UsageFrame
      | DeviceConfig
      | { version: 1; type: "ping" }
      | {
          version: 1;
          type: "display";
          backlightTimeoutSec: number;
          requestId?: string;
        }
      | { version: 1; type: "wake"; requestId?: string }
      | { version: 1; type: "error"; message: string },
  ): Promise<void> {
    if (frame.type === "usage") this.lastFrame = frame;
    if (frame.type === "error") this.clearCachedFrame();
    const child = this.worker;
    if (!child || !this.connected || child.exitCode !== null) {
      if (
        frame.type === "config" ||
        frame.type === "display" ||
        frame.type === "wake"
      )
        throw new Error("Wi-Fi設定の保存にはUSB接続が必要です。");
      return;
    }
    let outgoing = frame;
    let acknowledged: Promise<void> | undefined;
    if (
      frame.type === "config" ||
      frame.type === "display" ||
      frame.type === "wake"
    ) {
      if (!this.compatible)
        throw new Error(
          "初期設定用ファームウェアを確認できません。LCD用ファームウェアを書き込み、再接続してください。",
        );
      if (frame.type === "config") this.lastDirectUpdate = undefined;
      if (this.pendingConfig) throw new Error("別のWi-Fi設定を保存中です。");
      const requestId = randomBytes(8).toString("hex");
      outgoing = { ...frame, requestId };
      acknowledged = new Promise<void>((resolve, reject) => {
        const timer = setTimeout(
          () =>
            this.finishConfig(
              new Error(
                "LCDから保存完了の応答がありません。ファームウェアとUSB接続を確認してください。",
              ),
            ),
          10_000,
        );
        this.pendingConfig = {
          requestId,
          accepted: frame.type,
          resolve,
          reject,
          timer,
        };
      });
    }
    const id = ++this.sequence;
    const written = new Promise<void>((resolve, reject) => {
      const timer = setTimeout(() => {
        child.kill();
        reject(new Error("LCDへの送信がタイムアウトしました。"));
      }, 10_000);
      this.pendingWrites.set(id, { resolve, reject, timer });
      try {
        child.stdin.write(
          JSON.stringify({
            type: "write",
            id,
            data: JSON.stringify(outgoing) + "\n",
          }) + "\n",
        );
        void Promise.resolve(child.stdin.flush()).catch(() => {
          clearTimeout(timer);
          this.pendingWrites.delete(id);
          child.kill();
          reject(new Error("USB補助プロセスへの書き込みに失敗しました。"));
        });
      } catch {
        clearTimeout(timer);
        this.pendingWrites.delete(id);
        reject(new Error("USB補助プロセスへ送信できませんでした。"));
      }
    });
    try {
      await Promise.all([written, acknowledged]);
    } catch (error) {
      this.finishConfig(new Error("LCDへの設定送信に失敗しました。"));
      throw error;
    }
  }

  async close(): Promise<void> {
    this.stopped = true;
    if (this.reconnect) clearTimeout(this.reconnect);
    this.reconnect = undefined;
    this.failPending();
    const child = this.worker;
    this.worker = undefined;
    this.connected = false;
    this.compatible = false;
    this.path = undefined;
    if (child) {
      const timer = setTimeout(() => child.kill(), 2000);
      try {
        if (child.exitCode === null) await child.stdin.end();
        await child.exited;
      } catch {
        child.kill();
      } finally {
        clearTimeout(timer);
      }
    }
    this.message = "USB未接続";
  }
}
