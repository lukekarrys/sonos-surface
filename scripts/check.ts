import { cli, main, run } from "./common.ts";
import { buildVariants, hardwareTargets } from "./hardware-targets.ts";

export function checkTasks(full = false) {
  return [
    "typecheck",
    "format:check",
    "format:cpp:check",
    "writer:page:check",
    "test",
    ...(full
      ? Object.values(hardwareTargets).flatMap((target) => [
          `build:${target.id}`,
          ...target.variants
            .filter((variant) => buildVariants[variant].checked)
            .map((variant) => `build:${target.id}:${variant}`),
        ])
      : []),
  ];
}

if (import.meta.main)
  await main(async () => {
    const { values, positionals } = cli([], ["full"]);
    if (values.help) return console.log("node --run check[:full]");
    if (positionals.length) throw new Error("Unexpected positional arguments");
    for (const task of checkTasks(Boolean(values.full))) {
      console.log(`\nChecking ${task}`);
      await run(process.execPath, ["--run", task]);
    }
  });
