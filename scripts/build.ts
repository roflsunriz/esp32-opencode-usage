export {};
const result = await Bun.build({
  entrypoints: ["host/cli.ts"],
  outdir: "dist",
  target: "bun",
});
if (!result.success) {
  for (const log of result.logs) console.error(log);
  process.exit(1);
}
await Bun.write("dist/serial-worker.py", Bun.file("host/serial-worker.py"));
console.log("ホストとUSB補助プログラムをdistへ出力しました。");
