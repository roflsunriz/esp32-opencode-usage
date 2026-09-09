import { randomBytes, timingSafeEqual } from "node:crypto";
import { clearSession, login, saveSession } from "./auth/index.ts";
import { OpenCodeClient } from "./api.ts";
import { dashboard } from "./dashboard.ts";
import { DeviceConnection } from "./device.ts";
import { UsageService } from "./service.ts";

function equalSecret(actual: string, expected: string): boolean {
  const a = Buffer.from(actual);
  const b = Buffer.from(expected);
  return a.length === b.length && timingSafeEqual(a, b);
}

async function input(request: Request): Promise<Record<string, unknown>> {
  if (!request.headers.get("content-type")?.startsWith("application/json"))
    throw new Error("JSON形式で送信してください。");
  const text = await request.text();
  if (text.length > 4096) throw new Error("設定データが大きすぎます。");
  const body: unknown = JSON.parse(text);
  if (!body || typeof body !== "object" || Array.isArray(body))
    throw new Error("設定形式が不正です。");
  return body as Record<string, unknown>;
}

export function startServer(service: UsageService, options: { port: number }) {
  const controlToken = randomBytes(32).toString("hex");
  const nonce = randomBytes(24).toString("base64");
  let loginPending = false;
  let loginFailure: string | undefined;
  const page = dashboard
    .replaceAll("__CONTROL_TOKEN__", controlToken)
    .replaceAll("__NONCE__", nonce);
  const json = (body: unknown, status = 200) =>
    Response.json(body, {
      status,
      headers: {
        "Cache-Control": "no-store",
        "X-Content-Type-Options": "nosniff",
      },
    });
  const server = Bun.serve({
    hostname: "127.0.0.1",
    port: options.port,
    maxRequestBodySize: 4096,
    idleTimeout: 60,
    async fetch(request, instance) {
      const url = new URL(request.url);
      const address = instance.requestIP(request)?.address;
      const local =
        address === "127.0.0.1" ||
        address === "::1" ||
        address === "::ffff:127.0.0.1";
      const hostAllowed =
        url.hostname === "127.0.0.1" ||
        url.hostname === "localhost" ||
        url.hostname === "[::1]";
      if (!local || !hostAllowed)
        return json({ error: "Control UI is available only on this PC." }, 403);
      if (request.method === "GET" && url.pathname === "/") {
        return new Response(page, {
          headers: {
            "Content-Type": "text/html; charset=utf-8",
            "Cache-Control": "no-store",
            "Content-Security-Policy": `default-src 'none'; script-src 'nonce-${nonce}'; style-src 'unsafe-inline'; connect-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'`,
            "X-Content-Type-Options": "nosniff",
            "Referrer-Policy": "no-referrer",
          },
        });
      }
      if (
        !equalSecret(request.headers.get("X-Control-Token") ?? "", controlToken)
      )
        return json({ error: "操作画面を再読み込みしてください。" }, 403);
      const origin = request.headers.get("origin");
      if (origin && origin !== url.origin)
        return json({ error: "Origin not allowed" }, 403);
      try {
        if (request.method === "GET") {
          if (url.pathname === "/api/status")
            return json({
              ...service.status(),
              loginPending,
              error: loginFailure ?? service.status().error,
            });
          if (url.pathname === "/api/ports")
            return json({ ports: await DeviceConnection.list() });
        }
        if (request.method !== "POST") return json({ error: "Not found" }, 404);
        const body = await input(request);
        switch (url.pathname) {
          case "/api/refresh":
            await service.refresh();
            return json(service.status());
          case "/api/login": {
            if (loginPending)
              return json({ error: "ログイン処理が進行中です。" }, 409);
            loginPending = true;
            loginFailure = undefined;
            void login({
              launch: true,
              port: 2829,
              workspace: service.status().workspace,
            })
              .then(async (session) => {
                await new OpenCodeClient(
                  session.cookie,
                  session.workspace,
                ).usage();
                await saveSession(session);
                service.setSession(session);
                await service.refresh();
              })
              .catch(() => {
                loginFailure =
                  "ログインを完了できませんでした。公式画面を確認して、もう一度ログインしてください。";
              })
              .finally(() => {
                loginPending = false;
              });
            return json({ started: true }, 202);
          }
          case "/api/logout":
            if (loginPending)
              return json(
                { error: "ログイン処理が終わるまでお待ちください。" },
                409,
              );
            await clearSession();
            service.setSession();
            loginFailure = undefined;
            return json({ ok: true });
          case "/api/connect":
            if (typeof body.path !== "string")
              throw new Error("USBポートを選択してください。");
            await service.device.connect(body.path);
            return json({ ok: true });
          case "/api/disconnect":
            await service.device.close();
            return json({ ok: true });
          case "/api/setup": {
            if (
              typeof body.ssid !== "string" ||
              !body.ssid.length ||
              Buffer.byteLength(body.ssid) > 32
            )
              throw new Error("SSIDは1〜32バイトで指定してください。");
            if (
              typeof body.password !== "string" ||
              (body.password.length > 0 &&
                (Buffer.byteLength(body.password) < 8 ||
                  Buffer.byteLength(body.password) > 63))
            )
              throw new Error(
                "Wi-Fiパスワードは8〜63バイト、または空欄にしてください。",
              );
            if (
              typeof body.pollIntervalSec !== "number" ||
              !Number.isInteger(body.pollIntervalSec) ||
              body.pollIntervalSec < 15 ||
              body.pollIntervalSec > 3600
            )
              throw new Error("取得間隔は15〜3600秒にしてください。");
            await service.provisionDevice({
              ssid: body.ssid,
              password: body.password,
              pollIntervalSec: body.pollIntervalSec,
            });
            return json({ ok: true });
          }
          case "/api/clear-device":
            await service.clearDevice();
            return json({ ok: true });
          default:
            return json({ error: "Not found" }, 404);
        }
      } catch (error) {
        return json(
          {
            error:
              error instanceof Error ? error.message : "処理に失敗しました。",
          },
          400,
        );
      }
    },
    error() {
      return json({ error: "サーバーでエラーが発生しました。" }, 500);
    },
  });
  return server;
}
