import { existsSync } from "node:fs";
import { join } from "node:path";
import { ROOT, LIBS, DEPS } from "./common.ts";
import {
  includes,
  deviceInclude,
  coreSources,
  sharedSources,
} from "./host-build.ts";
import type { HostTarget } from "./host-build.ts";

export function hostTestTargets(): HostTarget[] {
  const buttons = existsSync(
    join(LIBS, "M5Unified/src/utility/Button_Class.hpp"),
  )
    ? join(LIBS, "M5Unified/src")
    : join(DEPS, "host/M5Buttons/src");
  return [
    { name: "core_test", sources: sharedSources(), includes },
    { name: "touch_test", sources: [], includes: [deviceInclude, includes[2]] },
    {
      name: "stick_button_test",
      sources: [join(buttons, "utility/Button_Class.cpp")],
      includes: [deviceInclude, includes[0], buttons],
    },
    {
      name: "waveshare_ui_test",
      sources: coreSources(),
      includes: [deviceInclude, ...includes],
    },
    {
      name: "artwork_test",
      sources: [],
      includes: [deviceInclude, includes[0]],
    },
  ].map((target) => ({
    ...target,
    sources: [...target.sources, join(ROOT, `tests/${target.name}.cpp`)],
  }));
}

// The read-only probe also owns a translation unit under tests/, but is never
// executed by the autonomous test suite.
export function hostProbeTarget(): HostTarget {
  return {
    name: "sonos_read",
    sources: [...sharedSources(), join(ROOT, "tests/sonos_read.cpp")],
    includes,
    sanitize: false,
    linkOptions: ["-lcurl"],
  };
}
