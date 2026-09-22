export const periods = ["rolling", "weekly", "monthly"] as const;
export type Period = (typeof periods)[number];

export interface UsageWindow {
  used: number;
  limit: number;
  percent: number;
  resetInSec: number;
}

export interface UsageFrame {
  version: 1;
  type: "usage";
  updatedAt: number;
  rolling: UsageWindow;
  weekly: UsageWindow;
  monthly: UsageWindow;
}

/** 金額はmicroCents（1セント=1,000,000）。1ドル=100,000,000として換算する。 */
const microCentsPerDollar = 100_000_000;
const maxSafeInteger = Number.MAX_SAFE_INTEGER;

function object(value: unknown): Record<string, unknown> {
  if (!value || typeof value !== "object" || Array.isArray(value)) {
    throw new Error(
      "使用量データの形式が変わっています。取得処理を更新してください。",
    );
  }
  return value as Record<string, unknown>;
}

function microCents(value: unknown, name: string): number {
  if (typeof value === "number") {
    if (!Number.isSafeInteger(value) || value < 0) {
      throw new Error(`使用量データの ${name} が不正です。`);
    }
    return value;
  }
  if (typeof value === "string" && /^[0-9]{1,19}$/.test(value)) {
    const parsed = Number(value);
    if (Number.isSafeInteger(parsed)) return parsed;
  }
  throw new Error(`使用量データの ${name} が不正です。`);
}

function resetInSecFrom(value: unknown, name: string, now: number): number {
  if (typeof value !== "string" || value.length > 64) {
    throw new Error(`使用量データの ${name} が不正です。`);
  }
  const moment = Date.parse(value);
  if (!Number.isFinite(moment)) {
    throw new Error(`使用量データの ${name} が不正です。`);
  }
  return Math.max(0, Math.round((moment - now) / 1000));
}

const meterNames: Record<Period, string> = {
  rolling: "fiveHour",
  weekly: "week",
  monthly: "month",
};

export function normalizeUsage(value: unknown, now = Date.now()): UsageFrame {
  const source = object(value);
  const access = object(source.access);
  const meters = object(access.meters);
  const windows = {} as Record<Period, UsageWindow>;
  for (const period of periods) {
    const item = object(meters[meterNames[period]]);
    const usedMicro = microCents(item.usedMicroCents, "usedMicroCents");
    const limitMicro = microCents(item.limitMicroCents, "limitMicroCents");
    if (limitMicro <= 0 || limitMicro > maxSafeInteger) {
      throw new Error("使用量または上限額の精度が不正です。");
    }
    const used = usedMicro / microCentsPerDollar;
    const limit = limitMicro / microCentsPerDollar;
    const percent = (usedMicro / limitMicro) * 100;
    if (!Number.isFinite(percent)) {
      throw new Error("使用率の計算に失敗しました。");
    }
    // 月間メーターに resetsAt はなく、契約の endsAt がリセット時刻になる。
    const resetSource = period === "monthly" ? access.endsAt : item.resetsAt;
    const resetInSec = resetInSecFrom(resetSource, "resetsAt", now);
    windows[period] = { used, limit, percent, resetInSec };
  }
  return {
    version: 1,
    type: "usage",
    updatedAt: Math.floor(now / 1000),
    ...windows,
  };
}

/** 公式の使用量JSONだけを読み取る。未知の形式は例外にし、0%表示にしない。 */
export function decodeUsageResponse(
  text: string,
  contentType: string,
): unknown {
  if (text.length > 1_000_000)
    throw new Error("使用量レスポンスが大きすぎます。");
  if (contentType.startsWith("application/json"))
    return JSON.parse(text) as unknown;
  throw new Error("使用量を取得できませんでした。再ログインしてください。");
}

export function validateWorkspace(workspace: string): string {
  if (!/^wrk_[A-Za-z0-9]{10,80}$/.test(workspace)) {
    throw new Error(
      "ワークスペースIDは公式ページのURLにある wrk_ で始まる値を指定してください。",
    );
  }
  return workspace;
}
