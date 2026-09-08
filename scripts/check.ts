import { cli, main, run } from "./common.ts";
await main(async () => {
  const { values, positionals } = cli([], ["full"]);
  if (values.help) return console.log("node --run check[:full]");
  if (positionals.length) throw new Error("Unexpected positional arguments");
  for (const task of [
    "typecheck",
    "format:check",
    "format:cpp:check",
    "test",
    ...(values.full ? ["build:stick", "build:waveshare"] : []),
  ]) {
    console.log(`\nChecking ${task}`);
    await run(process.execPath, ["--run", task]);
  }
});
