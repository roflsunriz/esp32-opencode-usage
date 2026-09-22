import { mkdir, readFile, rename, rm, writeFile } from "node:fs/promises";
import { join } from "node:path";

import { protect, unprotect } from "./protection.ts";
import type { StoredSession } from "./types.ts";

export interface TextProtector {
  protect(value: string): Promise<string>;
  unprotect(value: string): Promise<string>;
}

export interface SessionStore {
  load(): Promise<StoredSession | undefined>;
  save(session: StoredSession): Promise<void>;
  clear(): Promise<void>;
}

type ProtectedSession = {
  version: 1;
  protected: string;
};

function isRecord(value: unknown): value is Record<string, unknown> {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}

function isWorkspace(value: string): boolean {
  return /^[A-Za-z0-9_-]{1,200}$/.test(value);
}

function isCookie(value: string): boolean {
  return /^__Host-console_session=[^\r\n;]+$/.test(value);
}

function asSession(value: unknown): StoredSession | undefined {
  if (
    !isRecord(value) ||
    value.version !== 1 ||
    typeof value.cookie !== "string" ||
    typeof value.workspace !== "string"
  ) {
    return undefined;
  }
  if (!isCookie(value.cookie) || !isWorkspace(value.workspace))
    return undefined;

  if (value.expiresAt === undefined) {
    return { version: 1, cookie: value.cookie, workspace: value.workspace };
  }
  if (
    typeof value.expiresAt !== "number" ||
    !Number.isSafeInteger(value.expiresAt) ||
    value.expiresAt <= 0
  ) {
    return undefined;
  }
  return {
    version: 1,
    cookie: value.cookie,
    workspace: value.workspace,
    expiresAt: value.expiresAt,
  };
}

function asProtectedSession(value: unknown): ProtectedSession | undefined {
  if (
    !isRecord(value) ||
    value.version !== 1 ||
    typeof value.protected !== "string" ||
    value.protected.length === 0
  ) {
    return undefined;
  }
  return { version: 1, protected: value.protected };
}

function isMissingFile(error: unknown): boolean {
  return isRecord(error) && error.code === "ENOENT";
}

export function createSessionStore(
  directory: string,
  protector: TextProtector,
): SessionStore {
  const sessionPath = join(directory, "session.json");
  const temporaryPath = join(directory, "session.json.tmp");

  return {
    async load(): Promise<StoredSession | undefined> {
      let serialized: string;
      try {
        serialized = await readFile(sessionPath, "utf8");
      } catch (error) {
        if (isMissingFile(error)) return undefined;
        throw new Error(
          "保存済みの認証情報を読み取れません。ファイルのアクセス権を確認してください。",
          { cause: error },
        );
      }

      try {
        const protectedSession = asProtectedSession(
          JSON.parse(serialized) as unknown,
        );
        if (!protectedSession) throw new Error("invalid session envelope");
        const session = asSession(
          JSON.parse(
            await protector.unprotect(protectedSession.protected),
          ) as unknown,
        );
        if (
          !session ||
          (session.expiresAt !== undefined && session.expiresAt <= Date.now())
        ) {
          throw new Error("invalid or expired session");
        }
        return session;
      } catch {
        await rm(sessionPath, { force: true });
        return undefined;
      }
    },

    async save(session: StoredSession): Promise<void> {
      const validSession = asSession(session);
      if (!validSession)
        throw new Error("認証情報の形式が不正です。再ログインしてください。");

      const protectedValue = await protector.protect(
        JSON.stringify(validSession),
      );
      if (protectedValue.length === 0)
        throw new Error("認証情報を安全に保存できませんでした。");

      await mkdir(directory, { recursive: true });
      await writeFile(
        temporaryPath,
        JSON.stringify({
          version: 1,
          protected: protectedValue,
        } satisfies ProtectedSession),
        { encoding: "utf8", mode: 0o600 },
      );
      await rename(temporaryPath, sessionPath);
    },

    async clear(): Promise<void> {
      await rm(sessionPath, { force: true });
    },
  };
}

const defaultStore = createSessionStore(join(process.cwd(), ".private"), {
  protect,
  unprotect,
});

export function loadSession(): Promise<StoredSession | undefined> {
  return defaultStore.load();
}

export function saveSession(session: StoredSession): Promise<void> {
  return defaultStore.save(session);
}

export function clearSession(): Promise<void> {
  return defaultStore.clear();
}
