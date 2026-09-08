import { join } from "node:path";
import { ROOT, main, run, sourceFiles } from "./common.ts";
import { compileHost } from "./host-build.ts";
import { hostTestTargets } from "./host-test-targets.ts";

await main(async () => {
  await run(process.execPath, [
    "--test",
    ...sourceFiles(join(ROOT, "tests"), /\.test\.ts$/),
  ]);
  for (const target of hostTestTargets()) {
    const binary = await compileHost(target);
    await run(binary);
  }
});
