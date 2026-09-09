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

function object(value: unknown): Record<string, unknown> {
  if (!value || typeof value !== "object" || Array.isArray(value)) {
    throw new Error(
      "使用量データの形式が変わっています。取得処理を更新してください。",
    );
  }
  return value as Record<string, unknown>;
}

function number(value: unknown, name: string): number {
  if (typeof value !== "number" || !Number.isFinite(value) || value < 0) {
    throw new Error(`使用量データの ${name} が不正です。`);
  }
  return value;
}

export function normalizeUsage(value: unknown, now = Date.now()): UsageFrame {
  const source = object(value);
  const windows = {} as Record<Period, UsageWindow>;
  for (const period of periods) {
    const item = object(source[`${period}Usage`]);
    const used = number(item.usage, "usage") / 1e8;
    const limit = number(item.limit, "limit") / 1e8;
    const percent = number(item.usagePercent, "usagePercent");
    const resetInSec = number(item.resetInSec, "resetInSec");
    if (
      limit <= 0 ||
      !Number.isSafeInteger(item.usage) ||
      !Number.isSafeInteger(item.limit)
    ) {
      throw new Error("使用量または上限額の精度が不正です。");
    }
    // The console rounds percentages; keep its reported value and do not clamp
    // the displayed number. Only the LCD/bar drawing clamps at 100%.
    if (Math.abs(percent - (used / limit) * 100) > 0.11) {
      throw new Error(
        "使用率と金額が一致しません。公式データの形式を確認してください。",
      );
    }
    windows[period] = { used, limit, percent, resetInSec };
  }
  return {
    version: 1,
    type: "usage",
    updatedAt: Math.floor(now / 1000),
    ...windows,
  };
}

/** Read only the three numeric records, never execute server-supplied JavaScript. */
export function decodeUsageResponse(
  text: string,
  contentType: string,
): unknown {
  if (text.length > 1_000_000)
    throw new Error("使用量レスポンスが大きすぎます。");
  if (contentType.startsWith("application/json"))
    return JSON.parse(text) as unknown;
  if (!contentType.startsWith("text/javascript")) {
    throw new Error("使用量を取得できませんでした。再ログインしてください。");
  }
  const output: Record<string, unknown> = {};
  for (const period of periods) {
    const regex = new RegExp(
      `\\b${period}Usage\\s*:\\s*(?:\\$R\\[\\d+\\]\\s*=\\s*)?\\{([^{}]*)\\}`,
      "g",
    );
    const matches = [...text.matchAll(regex)];
    if (matches.length !== 1)
      throw new Error("公式の使用量レスポンス形式が変わっています。");
    const record = matches[0]?.[1] ?? "";
    const parsed: Record<string, unknown> = {};
    for (const name of ["usage", "limit", "usagePercent", "resetInSec"]) {
      const values = [
        ...record.matchAll(
          new RegExp(
            `(?:^|,)\\s*${name}\\s*:\\s*([0-9]+(?:\\.[0-9]+)?(?:e[+-]?[0-9]+)?)\\s*(?=,|$)`,
            "gi",
          ),
        ),
      ];
      if (values.length !== 1)
        throw new Error("公式の使用量データに必要な数値がありません。");
      parsed[name] = Number(values[0]?.[1]);
    }
    output[`${period}Usage`] = parsed;
  }
  return output;
}

export function validateWorkspace(workspace: string): string {
  if (!/^wrk_[A-Za-z0-9]{10,80}$/.test(workspace)) {
    throw new Error(
      "ワークスペースIDは公式ページのURLにある wrk_ で始まる値を指定してください。",
    );
  }
  return workspace;
}
