import { test } from "node:test";
import assert from "node:assert/strict";
import { discover } from "../scripts/discover.ts";
test("discovery fails without usable replies and probes only discovered addresses", async () => {
  const seen: string[] = [];
  const probe = async (ip: string) => {
    seen.push(ip);
    return ip;
  };
  const print = () => {};
  await assert.rejects(discover(async () => [], probe, print));
  await assert.rejects(
    discover(
      async () => {
        throw new Error("network");
      },
      probe,
      print,
    ),
  );
  assert.deepEqual(seen, []);
  await assert.rejects(
    discover(
      async () => ["192.0.2.1"],
      async () => {
        throw new Error("unreachable");
      },
      print,
    ),
  );
  await discover(async () => ["192.0.2.1"], probe, print);
  assert.deepEqual(seen, ["192.0.2.1"]);
});
