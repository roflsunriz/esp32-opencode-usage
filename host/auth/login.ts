import { existsSync } from "node:fs";
import {
  mkdir,
  mkdtemp,
  readFile,
  readdir,
  rm,
  writeFile,
} from "node:fs/promises";
import { basename, join } from "node:path";

import { connectMarionette, type MarionetteClient } from "./marionette.ts";
import type { LoginOptions, StoredSession } from "./types.ts";

const defaultLoginTimeoutMs = 5 * 60_000;
const commandTimeoutMs = 10_000;
const pollIntervalMs = 500;
const loginOrigin = "https://opencode.ai";

type AuthCookie = Pick<StoredSession, "cookie" | "expiresAt">;

function loginProfilesDirectory(): string {
  return join(process.cwd(), ".private", "firefox-logins");
}

export async function cleanupLoginProfiles(): Promise<void> {
  const directory = loginProfilesDirectory();
  let names: string[];
  try {
    names = await readdir(directory);
  } catch (error) {
    if (isMissingFile(error)) return;
    throw new Error("Firefox の認証用プロファイルを整理できませんでした。", {
      cause: error,
    });
  }
  await Promise.all(
    names
      .filter((name) => /^profile-[A-Za-z0-9_-]+$/.test(name))
      .map(async (name) => {
        try {
          await rm(join(directory, name), {
            recursive: true,
            force: true,
            maxRetries: 20,
            retryDelay: 250,
          });
        } catch {
          // A still-running login owns this profile. The login's own cleanup
          // retries it, and the next host start will recover any residue.
        }
      }),
  );
}

function authenticationError(): Error {
  return new Error(
    "OpenCode へのログインを確認できませんでした。Firefox でログイン後、対象のワークスペースを開いてから再試行してください。",
  );
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}

function isWorkspace(value: string): boolean {
  return /^wrk_[A-Za-z0-9]{10,80}$/.test(value);
}

function isMissingFile(error: unknown): boolean {
  return isRecord(error) && error.code === "ENOENT";
}

function isPort(value: number): boolean {
  return Number.isSafeInteger(value) && value >= 1 && value <= 65535;
}

function remaining(deadline: number): number {
  return Math.max(0, deadline - Date.now());
}

function delay(milliseconds: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}

function buildLoginUrl(workspace?: string): string {
  const url = new URL("/auth/authorize", loginOrigin);
  if (workspace) url.searchParams.set("continue", `/console/${workspace}/go`);
  return url.toString();
}

function workspaceFromUrl(value: unknown): string | undefined {
  if (typeof value !== "string") return undefined;
  try {
    const url = new URL(value);
    if (url.protocol !== "https:" || url.hostname !== "opencode.ai")
      return undefined;
    const match = /^\/(?:console|workspace)\/([^/]+)(?:\/|$)/.exec(
      url.pathname,
    );
    if (!match?.[1]) return undefined;
    const workspace = decodeURIComponent(match[1]);
    return isWorkspace(workspace) ? workspace : undefined;
  } catch {
    return undefined;
  }
}

function isOpencodeUrl(value: unknown): boolean {
  if (typeof value !== "string") return false;
  try {
    const url = new URL(value);
    return url.protocol === "https:" && url.hostname === "opencode.ai";
  } catch {
    return false;
  }
}

/** `WebDriver:GetCookies` の応答から、コンソール用のセッション Cookie だけを選ぶ。 */
export function extractAuthCookie(value: unknown): AuthCookie | undefined {
  const cookies = Array.isArray(value)
    ? value
    : isRecord(value) && Array.isArray(value.value)
      ? value.value
      : undefined;
  if (!cookies) return undefined;

  for (const candidate of cookies) {
    if (
      !isRecord(candidate) ||
      candidate.name !== "__Host-console_session" ||
      typeof candidate.value !== "string" ||
      typeof candidate.domain !== "string"
    ) {
      continue;
    }
    const domain = candidate.domain.toLowerCase().replace(/^\./, "");
    if (
      domain !== "opencode.ai" ||
      candidate.value.length === 0 ||
      /[;\r\n]/.test(candidate.value)
    )
      continue;

    const expiresAt =
      typeof candidate.expiry === "number" &&
      Number.isFinite(candidate.expiry) &&
      candidate.expiry > 0
        ? Math.floor(candidate.expiry * 1000)
        : undefined;
    if (expiresAt !== undefined && expiresAt <= Date.now()) continue;
    return expiresAt === undefined
      ? { cookie: `__Host-console_session=${candidate.value}` }
      : {
          cookie: `__Host-console_session=${candidate.value}`,
          expiresAt,
        };
  }
  return undefined;
}

function isFirefoxExecutable(path: string): boolean {
  return basename(path).toLowerCase() === "firefox.exe" && existsSync(path);
}

function findFirefoxExecutable(): string {
  const onPath = Bun.which("firefox.exe");
  if (onPath && isFirefoxExecutable(onPath)) return onPath;

  const candidates = [
    process.env.ProgramFiles,
    process.env["ProgramFiles(x86)"],
    process.env.LOCALAPPDATA,
  ]
    .filter(
      (directory): directory is string =>
        typeof directory === "string" && directory.length > 0,
    )
    .map((directory) => join(directory, "Mozilla Firefox", "firefox.exe"));
  const found = candidates.find(isFirefoxExecutable);
  if (!found)
    throw new Error(
      "Firefox が見つかりません。Mozilla Firefox をインストールしてから再試行してください。",
    );
  return found;
}

async function prepareFirefoxProfile(
  profileDirectory: string,
  port: number,
): Promise<void> {
  const userPreferences = join(profileDirectory, "user.js");
  const activePort = join(profileDirectory, "MarionetteActivePort");
  await mkdir(profileDirectory, { recursive: true });

  let currentPreferences = "";
  try {
    currentPreferences = await readFile(userPreferences, "utf8");
  } catch (error) {
    if (!isMissingFile(error)) {
      throw new Error("Firefox の認証用プロファイルを準備できませんでした。", {
        cause: error,
      });
    }
  }
  if (currentPreferences.length > 1_000_000)
    throw new Error("Firefox の認証用プロファイルを準備できませんでした。");
  const preferencesWithoutPort = currentPreferences.replace(
    /^[\t ]*user_pref\(\s*["']marionette\.port["']\s*,\s*[^)]*\);\s*(?:\r?\n)?/gm,
    "",
  );
  const newline =
    preferencesWithoutPort.length === 0 || preferencesWithoutPort.endsWith("\n")
      ? ""
      : "\n";
  await writeFile(
    userPreferences,
    `${preferencesWithoutPort}${newline}user_pref("marionette.port", ${port});\n`,
    "utf8",
  );
  await rm(activePort, { force: true });
}

async function readProfilePort(
  profileDirectory: string,
): Promise<number | undefined> {
  try {
    const candidate = Number(
      (
        await readFile(join(profileDirectory, "MarionetteActivePort"), "utf8")
      ).trim(),
    );
    return isPort(candidate) ? candidate : undefined;
  } catch (error) {
    if (isMissingFile(error)) return undefined;
    throw authenticationError();
  }
}

function launchFirefox(
  executable: string,
  profileDirectory: string,
  loginUrl: string,
): Bun.Subprocess {
  const child = Bun.spawn(
    [
      executable,
      "-no-remote",
      "-profile",
      profileDirectory,
      "-marionette",
      loginUrl,
    ],
    {
      stdin: "ignore",
      stdout: "ignore",
      stderr: "ignore",
    },
  );
  child.unref();
  return child;
}

async function waitForProfilePort(
  profileDirectory: string,
  deadline: number,
): Promise<number> {
  while (remaining(deadline) > 0) {
    const candidate = await readProfilePort(profileDirectory);
    if (candidate) return candidate;
    await delay(Math.min(100, remaining(deadline)));
  }
  throw authenticationError();
}

function validateOptions(
  options: LoginOptions,
): Required<Pick<LoginOptions, "launch" | "timeoutMs">> & LoginOptions {
  const timeoutMs = options.timeoutMs ?? defaultLoginTimeoutMs;
  if (
    !Number.isSafeInteger(timeoutMs) ||
    timeoutMs < 1 ||
    timeoutMs > 30 * 60_000
  )
    throw authenticationError();
  if (
    options.port !== undefined &&
    (!Number.isSafeInteger(options.port) ||
      options.port < 1 ||
      options.port > 65535)
  ) {
    throw authenticationError();
  }
  if (options.workspace !== undefined && !isWorkspace(options.workspace))
    throw authenticationError();
  return { ...options, launch: options.launch ?? true, timeoutMs };
}

async function startSession(client: MarionetteClient): Promise<void> {
  const result = await client.command("WebDriver:NewSession", {});
  const session =
    isRecord(result) && isRecord(result.value) ? result.value : result;
  if (
    !isRecord(session) ||
    typeof session.sessionId !== "string" ||
    !isRecord(session.capabilities)
  )
    throw authenticationError();
}

function valueString(value: unknown): string | undefined {
  return isRecord(value) && typeof value.value === "string"
    ? value.value
    : undefined;
}

function windowHandles(value: unknown): string[] | undefined {
  const candidate = Array.isArray(value)
    ? value
    : isRecord(value) && Array.isArray(value.value)
      ? value.value
      : undefined;
  return candidate && candidate.every((handle) => typeof handle === "string")
    ? candidate
    : undefined;
}

function stringArray(value: unknown): string[] | undefined {
  const candidate = Array.isArray(value)
    ? value
    : isRecord(value) && Array.isArray(value.value)
      ? value.value
      : undefined;
  return candidate && candidate.every((item) => typeof item === "string")
    ? candidate
    : undefined;
}

async function workspaceFromCurrentPage(
  client: MarionetteClient,
  currentUrl: string,
  expectedWorkspace: string | undefined,
): Promise<string | undefined> {
  const current = workspaceFromUrl(currentUrl);
  if (current) return current;
  if (!isOpencodeUrl(currentUrl)) return undefined;

  const links = stringArray(
    await client.command("WebDriver:ExecuteScript", {
      script:
        "return Array.from(document.querySelectorAll('a[href]'), a => a.href);",
      args: [],
    }),
  );
  if (!links) return undefined;
  const candidates = [
    ...new Set(
      links
        .map((link) => workspaceFromUrl(link))
        .filter((item): item is string => item !== undefined),
    ),
  ];
  if (expectedWorkspace)
    return candidates.includes(expectedWorkspace)
      ? expectedWorkspace
      : undefined;
  return candidates.length === 1 ? candidates[0] : undefined;
}

async function sessionInCurrentTab(
  client: MarionetteClient,
  expectedWorkspace: string | undefined,
): Promise<StoredSession | undefined> {
  const currentUrl = valueString(
    await client.command("WebDriver:GetCurrentURL", {}),
  );
  if (!currentUrl) return undefined;
  const workspace = await workspaceFromCurrentPage(
    client,
    currentUrl,
    expectedWorkspace,
  );
  if (
    !workspace ||
    (expectedWorkspace && workspace !== expectedWorkspace) ||
    !isOpencodeUrl(currentUrl)
  )
    return undefined;

  const cookie = extractAuthCookie(
    await client.command("WebDriver:GetCookies", {}),
  );
  return cookie ? { version: 1, workspace, ...cookie } : undefined;
}

async function waitForAuthenticatedSession(
  client: MarionetteClient,
  expectedWorkspace: string | undefined,
  deadline: number,
): Promise<StoredSession> {
  const originalHandle = valueString(
    await client.command("WebDriver:GetWindowHandle", {}),
  );
  if (!originalHandle) throw authenticationError();

  try {
    while (remaining(deadline) > 0) {
      const handles = windowHandles(
        await client.command("WebDriver:GetWindowHandles", {}),
      );
      if (!handles || handles.length === 0) throw authenticationError();
      const orderedHandles = handles.includes(originalHandle)
        ? [
            originalHandle,
            ...handles.filter((handle) => handle !== originalHandle),
          ]
        : handles;

      for (const handle of orderedHandles) {
        await client.command("WebDriver:SwitchToWindow", { handle });
        const session = await sessionInCurrentTab(client, expectedWorkspace);
        if (session) return session;
      }
      await delay(Math.min(pollIntervalMs, remaining(deadline)));
    }
    throw authenticationError();
  } finally {
    await client.command("WebDriver:SwitchToWindow", {
      handle: originalHandle,
    });
  }
}

async function connectAfterLaunch(
  port: number,
  deadline: number,
): Promise<MarionetteClient> {
  while (remaining(deadline) > 0) {
    try {
      return await connectMarionette(
        port,
        Math.min(commandTimeoutMs, remaining(deadline)),
      );
    } catch {
      if (remaining(deadline) <= 0) break;
      await delay(Math.min(100, remaining(deadline)));
    }
  }
  throw authenticationError();
}

async function connectForLogin(
  options: ReturnType<typeof validateOptions>,
  deadline: number,
): Promise<{
  client: MarionetteClient;
  cleanup(): Promise<void>;
}> {
  if (!options.launch) {
    const connectTimeout = Math.min(commandTimeoutMs, remaining(deadline));
    if (connectTimeout < 1) throw authenticationError();
    return {
      client: await connectMarionette(options.port ?? 2828, connectTimeout),
      async cleanup() {},
    };
  }

  if (process.platform !== "win32") {
    throw new Error(
      "Firefox を使ったログインは現在 Windows のみ対応しています。",
    );
  }
  // DeleteSession stops the Marionette listener but can leave Firefox open.
  // A persistent profile then remains locked and later login attempts merely
  // add tabs to that listener-less process. Use an isolated profile per login
  // and remove both the app-owned process and its cookie store afterwards.
  const profilesDirectory = loginProfilesDirectory();
  await cleanupLoginProfiles();
  await mkdir(profilesDirectory, { recursive: true });
  const profileDirectory = await mkdtemp(join(profilesDirectory, "profile-"));
  const configuredPort = options.port ?? 0;
  let child: Bun.Subprocess | undefined;
  try {
    await prepareFirefoxProfile(profileDirectory, configuredPort);
    child = launchFirefox(
      findFirefoxExecutable(),
      profileDirectory,
      buildLoginUrl(options.workspace),
    );
    const port =
      configuredPort === 0
        ? await waitForProfilePort(profileDirectory, deadline)
        : configuredPort;
    const client = await connectAfterLaunch(port, deadline);
    return {
      client,
      async cleanup() {
        if (child?.exitCode === null) child.kill();
        if (child) {
          await Promise.race([child.exited, delay(5_000)]);
          if (child.exitCode === null) child.kill();
        }
        try {
          await rm(profileDirectory, {
            recursive: true,
            force: true,
            maxRetries: 20,
            retryDelay: 250,
          });
        } catch {
          // The session is already copied into DPAPI-protected storage by the
          // caller. A delayed Firefox child must not turn that success into a
          // login failure; the ignored .private profile can be retried later.
        }
      },
    };
  } catch (error) {
    if (child?.exitCode === null) child.kill();
    await rm(profileDirectory, {
      recursive: true,
      force: true,
      maxRetries: 5,
      retryDelay: 100,
    });
    throw error;
  }
}

export async function login(
  options: LoginOptions = {},
): Promise<StoredSession> {
  const validated = validateOptions(options);
  const deadline = Date.now() + validated.timeoutMs;
  const connection = await connectForLogin(validated, deadline);
  const { client } = connection;
  let sessionStarted = false;
  try {
    await startSession(client);
    sessionStarted = true;
    if (validated.launch) {
      await client.command("WebDriver:Navigate", {
        url: buildLoginUrl(validated.workspace),
      });
    }
    return await waitForAuthenticatedSession(
      client,
      validated.workspace,
      deadline,
    );
  } catch (error) {
    if (error instanceof Error) throw error;
    throw authenticationError();
  } finally {
    try {
      if (sessionStarted) {
        try {
          await client.command(
            validated.launch ? "Marionette:Quit" : "WebDriver:DeleteSession",
            {},
          );
        } catch {
          // Session teardown must not replace a successfully captured login.
        }
      }
    } finally {
      // 外部から接続したFirefoxはセッションだけを解放する。アプリが
      // 起動した分離Firefoxはcleanupで終了し、Cookie保存先も削除する。
      client.close();
      await connection.cleanup();
    }
  }
}
