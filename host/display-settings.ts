export const backlightTimeouts = [
  15, 30, 60, 120, 300, 600, 1800, 3600, 7200,
] as const;
export const defaultBacklightTimeout = 60;

export function parseBacklightTimeout(value: unknown): number {
  if (
    typeof value !== "number" ||
    !backlightTimeouts.some((timeout) => timeout === value)
  ) {
    throw new Error(
      "消灯時間は15s、30s、1m、2m、5m、10m、30m、1h、2hから選択してください。",
    );
  }
  return value;
}
