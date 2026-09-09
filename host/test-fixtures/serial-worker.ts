import { appendFileSync } from "node:fs";
import { createInterface } from "node:readline";

const log = process.argv[2]!;
const broken = process.argv.at(-1) === "BREAK";
const firmware =
  process.argv.at(-1) === "WRONG" ? "other-firmware" : "opencode-go-lcd";
function emit(value: unknown) {
  process.stdout.write(JSON.stringify(value) + "\n");
}
function rx(value: unknown) {
  emit({ type: "data", data: JSON.stringify(value) + "\n" });
}
emit({ type: "open" });
rx({ version: 1, type: "ready", firmware, setupSchema: 2 });
createInterface({ input: process.stdin }).on("line", (line) => {
  const command = JSON.parse(line) as {
    type: string;
    id?: number;
    data?: string;
  };
  if (command.type === "close") {
    process.stdout.write('{"type":"close"}\n', () => process.exit(0));
    return;
  }
  const frame = JSON.parse(command.data!) as {
    type: string;
    requestId?: string;
  };
  if (broken && frame.type !== "ping") {
    process.exit(0);
    return;
  }
  appendFileSync(log, JSON.stringify(frame) + "\n");
  emit({ type: "written", id: command.id });
  if (frame.type === "ping")
    rx({ type: "ack", accepted: "ping", firmware, setupSchema: 2 });
  if (frame.type === "config") {
    rx({ type: "ack", accepted: "config", requestId: "old-request" });
    setTimeout(
      () => rx({ type: "ack", accepted: "config", requestId: frame.requestId }),
      150,
    );
  } else if (frame.type === "usage") rx({ type: "ack", accepted: "usage" });
});
