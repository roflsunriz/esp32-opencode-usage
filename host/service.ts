import { OpenCodeClient } from "./api.ts";
import type { StoredSession } from "./auth/index.ts";
import { DeviceConnection, type DeviceConfig } from "./device.ts";
import type { UsageFrame } from "./usage.ts";
import { defaultBacklightTimeout } from "./display-settings.ts";

export class UsageService {
  readonly device = new DeviceConnection();
  private client?: { usage(): Promise<UsageFrame> };
  private snapshot?: UsageFrame;
  private failure?: string;
  private inFlight?: Promise<void>;
  private interval?: ReturnType<typeof setInterval>;
  private workspace?: string;
  private session?: StoredSession;
  private generation = 0;

  constructor(
    private readonly createClient: (session: StoredSession) => {
      usage(): Promise<UsageFrame>;
    } = (session) => new OpenCodeClient(session.cookie, session.workspace),
  ) {}

  setSession(session?: StoredSession): void {
    this.generation++;
    this.device.clearCachedFrame();
    this.session = session;
    this.workspace = session?.workspace;
    this.client = session ? this.createClient(session) : undefined;
    this.snapshot = undefined;
    this.failure = session ? undefined : "ログインしてください。";
  }

  status() {
    return {
      authenticated: !!this.client,
      workspace: this.workspace,
      usage: this.snapshot,
      error: this.failure,
      updating: !!this.inFlight,
      device: this.device.status(),
    };
  }

  frame(): UsageFrame | undefined {
    // Stale data must not look like a successful fresh poll to the device.
    return this.failure ? undefined : this.snapshot;
  }

  async refresh(): Promise<void> {
    if (this.inFlight) return this.inFlight;
    const client = this.client;
    if (!client) {
      this.failure = "ログインしてください。";
      return;
    }
    const generation = this.generation;
    this.inFlight = (async () => {
      try {
        const frame = await client.usage();
        if (generation !== this.generation) return;
        this.snapshot = frame;
        this.failure = undefined;
      } catch (error) {
        if (generation !== this.generation) return;
        this.failure =
          error instanceof Error
            ? error.message
            : "使用量の取得に失敗しました。";
      } finally {
        this.inFlight = undefined;
      }
    })();
    return this.inFlight;
  }

  start(): void {
    if (this.interval) return;
    void this.refresh();
    this.interval = setInterval(() => void this.refresh(), 60_000);
  }

  async provisionDevice(settings: {
    ssid: string;
    password: string;
    pollIntervalSec: number;
    backlightTimeoutSec?: number;
  }): Promise<void> {
    const session = this.session;
    if (!session) throw new Error("先にOpenCodeへログインしてください。");
    if (session.cookie.length > 4096)
      throw new Error(
        "認証情報が機器の保存上限を超えています。専用のログイン画面から再ログインしてください。",
      );
    const client = new OpenCodeClient(session.cookie, session.workspace);
    await client.usage();
    await this.device.send({
      version: 1,
      type: "config",
      enabled: true,
      ...settings,
      backlightTimeoutSec:
        settings.backlightTimeoutSec ?? defaultBacklightTimeout,
      authCookie: session.cookie,
      workspace: session.workspace,
      // 直接取得は本体がGET /console/api/go/statusへ接続し、
      // 取得先IDを使わない。USB設定形式の互換のため空文字を送る。
      queryId: "",
    });
  }

  async clearDevice(): Promise<void> {
    const config: DeviceConfig = {
      version: 1,
      type: "config",
      enabled: false,
      ssid: "",
      password: "",
      authCookie: "",
      workspace: "",
      queryId: "",
      pollIntervalSec: 60,
      backlightTimeoutSec: defaultBacklightTimeout,
    };
    await this.device.send(config);
  }

  async stop(): Promise<void> {
    if (this.interval) clearInterval(this.interval);
    this.interval = undefined;
    this.generation++;
    await this.device.close();
  }
}
