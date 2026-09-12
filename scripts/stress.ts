import { cli, main, run } from "./common.ts";
import { compileHost } from "./host-build.ts";
import { hostTestTargets } from "./host-test-targets.ts";

await main(async () => {
  const { values, positionals } = cli(["seed", "steps"]);
  if (values.help)
    return console.log(
      "node --run stress -- [--seed UINT64] [--steps 1000000]",
    );
  if (positionals.length) throw new Error("Unexpected positional arguments");
  function integer(
    value: string | boolean | undefined,
    fallback: bigint,
    max: bigint,
  ) {
    if (value === undefined) return fallback;
    if (typeof value !== "string" || !/^(?:[0-9]+|0x[0-9a-f]+)$/i.test(value))
      throw new Error("Expected an unsigned integer");
    const number = BigInt(value);
    if (number > max) throw new Error("Integer out of range");
    return number;
  }
  const seeds =
    values.seed === undefined
      ? [0x5eed1234n, 1n, 0xc0ffee42n]
      : [integer(values.seed, 0n, 0xffffffffffffffffn)];
  const steps = integer(values.steps, 1000000n, 10000000n);
  if (!steps) throw new Error("Steps must be positive");
  for (const target of hostTestTargets().filter(
    (t) =>
      t.name.endsWith("_lifecycle_test") || t.name === "runtime_fault_test",
  )) {
    const binary = await compileHost(target);
    for (const seed of seeds) await run(binary, [String(seed), String(steps)]);
  }
});
