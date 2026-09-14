import { join } from "node:path";
import { ROOT } from "./common.ts";

// Optional build variants of one hardware target. Each compiles the same
// sketch with one extra define into its own build directory, so a variant
// never disturbs the normal image's cache or its flashed configuration.
export const buildVariants = {
  runtime: { option: undefined, define: undefined },
  // Raw touch coordinates for per-unit calibration; no playback actions.
  touch: { option: "touch-diagnostic", define: "SURFACE_TOUCH_DIAGNOSTIC" },
} as const;
export type BuildVariant = keyof typeof buildVariants;

export const hardwareTargets = {
  "stick-s3": {
    id: "stick-s3",
    define: "SURFACE_STICK_S3",
    flashSize: "8M",
    partitionScheme: "default_8MB",
    flags: [] as readonly string[],
    variants: [] as readonly BuildVariant[],
    power: {
      mechanism: "esp32-deep-sleep",
      wakeButton: "front (KEY1 / M5 BtnA)",
    },
  },
  "ws-1.8": {
    id: "ws-1.8",
    define: "SURFACE_WAVESHARE_1_8",
    flashSize: "16M",
    partitionScheme: "app3M_fat9M_16MB",
    // LVGL is the ws-1.8 UI; LV_CONF_INCLUDE_SIMPLE selects the repo-owned
    // lv_conf.h on the include path. stick-s3 never links LVGL.
    flags: ["-DLV_CONF_INCLUDE_SIMPLE"] as readonly string[],
    variants: ["touch"] as readonly BuildVariant[],
    power: { mechanism: "esp32-deep-sleep", wakeButton: "BOOT" },
  },
} as const;

export type HardwareTargetId = keyof typeof hardwareTargets;
export type HardwareTarget = (typeof hardwareTargets)[HardwareTargetId];

export function hardwareTargetId(value: string): HardwareTargetId {
  if (!Object.hasOwn(hardwareTargets, value))
    throw new Error(
      `Hardware target must be ${Object.keys(hardwareTargets).join(" or ")}`,
    );
  return value as HardwareTargetId;
}

export function buildPath(
  targetId: HardwareTargetId,
  variant: BuildVariant = "runtime",
) {
  const target = hardwareTargets[targetId];
  if (variant !== "runtime" && !target.variants.includes(variant))
    throw new Error(
      `Hardware target ${target.id} has no ${buildVariants[variant].option} variant`,
    );
  return join(ROOT, ".build", `${target.id}-${variant}`);
}

// Every variant define is always passed explicitly (0 or 1), so guarded
// sources never depend on an undefined macro.
export function variantDefines(variant: BuildVariant): string[] {
  const chosen = buildVariants[variant];
  return Object.values(buildVariants).flatMap((candidate) =>
    candidate.define
      ? [`-D${candidate.define}=${Number(candidate === chosen)}`]
      : [],
  );
}
// The complete extra compiler flags of one build: the target define, the
// target's own flags, and every variant define.
export function buildDefines(
  targetId: HardwareTargetId,
  variant: BuildVariant = "runtime",
): string[] {
  const target = hardwareTargets[targetId];
  return [`-D${target.define}`, ...target.flags, ...variantDefines(variant)];
}

export function fqbn(targetId: HardwareTargetId) {
  const target = hardwareTargets[targetId];
  return (
    "esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,PSRAM=opi,UploadSpeed=460800," +
    `FlashSize=${target.flashSize},PartitionScheme=${target.partitionScheme}`
  );
}
