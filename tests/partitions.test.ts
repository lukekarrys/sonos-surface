import { test } from "node:test";
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { partitionBinary } from "../scripts/partitions.ts";
const prefix = `nvs,data,nvs,0x9000,0x5000,
otadata,data,ota,0xe000,0x2000,
`;
test("current board partition binaries match independently generated pinned toolchain outputs", () => {
  const fixtures = [
    [
      prefix +
        "app0,app,ota_0,0x10000,0x330000,\napp1,app,ota_1,0x340000,0x330000,\nspiffs,data,spiffs,0x670000,0x180000,\ncoredump,data,coredump,0x7F0000,0x10000,",
      "1d9cca96de0fe07ad7fc0648b9878ddecd9ce565e38b589ad20fea698ed4c80c",
    ],
    [
      prefix +
        "app0,app,ota_0,0x10000,0x300000,\napp1,app,ota_1,0x310000,0x300000,\nffat,data,fat,0x610000,0x9E0000,\ncoredump,data,coredump,0xFF0000,0x10000,",
      "ace02503447d0f470692e65fa76002f2d77a92dc81cd3813d8aa66718d716da9",
    ],
  ];
  for (const [csv, digest] of fixtures) {
    const binary = partitionBinary(csv);
    assert.equal(binary.length, 3072);
    assert.equal(createHash("sha256").update(binary).digest("hex"), digest);
  }
  for (const csv of [
    "",
    prefix + prefix,
    "x,app,ota_0,0x9000,0x10000,",
    "x,data,nvs,0x9000,0,",
    "x,data,nvs,0x9000,0x1000,encrypted",
    "x,unknown,nvs,0x9000,0x1000,",
  ])
    assert.throws(() => partitionBinary(csv));
});
