import { createHash } from "node:crypto";
import { readFileSync, writeFileSync } from "node:fs";
import { cli, main } from "./common.ts";

// ESP-IDF partition records: little-endian magic/type/subtype/offset/size/name/flags,
// followed by an MD5 record and erased padding. Only the current board CSVs are supported.
const types: Record<string, number> = { app: 0, data: 1 };
const subtypes: Record<string, number> = {
  nvs: 2,
  ota: 0,
  ota_0: 16,
  ota_1: 17,
  spiffs: 130,
  fat: 129,
  coredump: 3,
};
export function partitionBinary(csv: string): Buffer {
  const records: Buffer[] = [];
  const names = new Set<string>();
  let end = 0x9000;
  for (const line of csv.split(/\r?\n/)) {
    const text = line.replace(/#.*/, "").trim();
    if (!text) continue;
    const fields = text.split(",").map((value) => value.trim());
    const [name, type, subtype, offsetText, sizeText, flags = ""] = fields;
    const offset = Number(offsetText),
      size = Number(sizeText);
    if (
      fields.length < 5 ||
      fields.length > 6 ||
      !name ||
      Buffer.byteLength(name) > 15 ||
      names.has(name) ||
      !Object.hasOwn(types, type) ||
      !Object.hasOwn(subtypes, subtype) ||
      flags ||
      !Number.isSafeInteger(offset) ||
      !Number.isSafeInteger(size) ||
      size <= 0 ||
      offset < end ||
      offset + size > 0x1000000 ||
      offset % (type === "app" ? 0x10000 : 0x1000) ||
      size % 0x1000 ||
      (type === "app") !== subtype.startsWith("ota_")
    )
      throw new Error("Unsupported or invalid pinned partition CSV");
    names.add(name);
    end = offset + size;
    const record = Buffer.alloc(32);
    record.writeUInt16LE(0x50aa, 0);
    record[2] = types[type];
    record[3] = subtypes[subtype];
    record.writeUInt32LE(offset, 4);
    record.writeUInt32LE(size, 8);
    record.write(name, 12, 16, "utf8");
    records.push(record);
  }
  if (!records.length || records.length > 94)
    throw new Error("Invalid partition count");
  const entries = Buffer.concat(records);
  const md5 = Buffer.concat([
    Buffer.from([0xeb, 0xeb]),
    Buffer.alloc(14, 0xff),
    createHash("md5").update(entries).digest(),
  ]);
  return Buffer.concat([
    entries,
    md5,
    Buffer.alloc(0xc00 - entries.length - md5.length, 0xff),
  ]);
}
if (import.meta.main)
  await main(() => {
    const { values, positionals } = cli(
      [],
      ["quiet"],
      process.argv.slice(2).map((arg) => (arg === "-q" ? "--quiet" : arg)),
    );
    if (values.help)
      return console.log(
        "Internal Arduino hook: partitions.ts -q INPUT.csv OUTPUT.bin",
      );
    if (positionals.length !== 2)
      throw new Error("Partition hook requires input CSV and output binary");
    writeFileSync(
      positionals[1],
      partitionBinary(readFileSync(positionals[0], "utf8")),
    );
  });
