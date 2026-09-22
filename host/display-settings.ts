export const backlightTimeouts = [
  15, 30, 60, 120, 300, 600, 1800, 3600, 7200,
] as const;
export const defaultBacklightTimeout = 60;
// Device Display-tab sliders (esp32-ui-style.md): 0-59 minutes + 0-24 hours,
// i.e. 0..89940 seconds. 0 disables the auto-off (always on). Legacy preset
// values stay valid; the dropdown keeps offering them.
export const maxBacklightTimeout = 24 * 3600 + 59 * 60;

export function parseBacklightTimeout(value: unknown): number {
  if (
    typeof value !== "number" ||
    !Number.isInteger(value) ||
    value < 0 ||
    value > maxBacklightTimeout
  ) {
    throw new Error(
      "消灯時間は0秒（常時点灯）から89940秒（24時間59分）までの整数で指定してください。",
    );
  }
  return value;
}
