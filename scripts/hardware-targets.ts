import { join } from "node:path";
import { ROOT } from "./common.ts";

export const hardwareTargets = {
  "stick-s3": {
    id: "stick-s3",
    define: "SURFACE_STICK_S3",
    flashSize: "8M",
    partitionScheme: "default_8MB",
    touchDiagnostic: false,
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
    touchDiagnostic: true,
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

export function buildPath(targetId: HardwareTargetId, touch = false) {
  const target = hardwareTargets[targetId];
  if (touch && !target.touchDiagnostic)
    throw new Error(`Hardware target ${target.id} has no touch diagnostic`);
  return join(ROOT, ".build", `${target.id}-${touch ? "touch" : "runtime"}`);
}

export function fqbn(targetId: HardwareTargetId) {
  const target = hardwareTargets[targetId];
  return (
    "esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,PSRAM=opi,UploadSpeed=460800," +
    `FlashSize=${target.flashSize},PartitionScheme=${target.partitionScheme}`
  );
}
