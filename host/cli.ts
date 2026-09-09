import { OpenCodeClient } from "./api.ts";
import { clearSession, loadSession, login, saveSession } from "./auth/index.ts";
import { DeviceConnection } from "./device.ts";
import { UsageService } from "./service.ts";
import { startServer } from "./server.ts";

const help = `OpenCode Go LCD

bun start                       ローカル操作画面を起動
bun start serve --serial COM3    USB接続して起動
bun start login                 公式画面でログインして暗号化保存
bun start login --attach 2828   指定したデバッグ用Firefoxの認証を保存
bun start usage                 現在の3期間の使用量を表示
bun start ports                 USBポート一覧
bun start logout                このPCに保存した認証を削除

--port 8765                     操作画面のポート（serve）
--workspace wrk_...             ワークスペースを指定（login）
`;

function option(args: string[], name: string): string | undefined {
  const index = args.indexOf(name);
  if (index < 0) return;
  const value = args[index + 1];
  if (!value || value.startsWith("--"))
    throw new Error(`${name} の値を指定してください。`);
  return value;
}

function portNumber(text: string | undefined, fallback: number): number {
  const port = text === undefined ? fallback : Number(text);
  if (!Number.isInteger(port) || port < 1024 || port > 65535)
    throw new Error("ポートは1024〜65535で指定してください。");
  return port;
}

async function main(): Promise<void> {
  const args = process.argv.slice(2);
  const command = args[0] ?? "serve";
  if (args.includes("--help") || command === "help") {
    console.log(help);
    return;
  }
  if (command === "login") {
    const attach = option(args, "--attach");
    console.log(
      attach
        ? "指定のFirefoxからOpenCode認証を取得します。"
        : "公式の認証画面でログインを完了してください。",
    );
    const session = await login({
      port: portNumber(attach, 2829),
      launch: !attach,
      workspace: option(args, "--workspace"),
    });
    // Validate real access before reporting login success or overwriting storage.
    await new OpenCodeClient(session.cookie, session.workspace).usage();
    await saveSession(session);
    console.log(
      "ログインを確認し、このWindowsユーザー向けに暗号化保存しました。",
    );
    return;
  }
  if (command === "logout") {
    await clearSession();
    console.log("保存した認証情報を削除しました。");
    return;
  }
  if (command === "ports") {
    console.table(await DeviceConnection.list());
    return;
  }
  const session = await loadSession();
  if (command === "usage") {
    if (!session)
      throw new Error("先に bun start login でログインしてください。");
    console.log(
      JSON.stringify(
        await new OpenCodeClient(session.cookie, session.workspace).usage(),
        null,
        2,
      ),
    );
    return;
  }
  if (command !== "serve") throw new Error(help);
  const service = new UsageService();
  service.setSession(session);
  const serial = option(args, "--serial");
  if (serial) await service.device.connect(serial);
  const port = portNumber(option(args, "--port"), 8765);
  const server = startServer(service, {
    port,
  });
  service.start();
  console.log(`操作画面: http://127.0.0.1:${server.port}`);
  console.log("停止するには Ctrl+C。設定完了後はESP32が単独で更新します。");
  if (args.includes("--open") && process.platform === "win32") {
    Bun.spawn(
      [
        "powershell.exe",
        "-NoLogo",
        "-NoProfile",
        "-NonInteractive",
        "-Command",
        `Start-Process 'http://127.0.0.1:${server.port}'`,
      ],
      { stdout: "ignore", stderr: "ignore" },
    ).unref();
  }
  let stopping = false;
  const stop = () => {
    if (stopping) return;
    stopping = true;
    void service.stop().finally(() => {
      server.stop(true);
      process.exit(0);
    });
  };
  process.on("SIGINT", stop);
  process.on("SIGTERM", stop);
}

if (import.meta.main) {
  main().catch((error: unknown) => {
    console.error(
      error instanceof Error ? error.message : "処理を開始できませんでした。",
    );
    process.exitCode = 1;
  });
}
