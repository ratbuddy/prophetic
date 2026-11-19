#!/usr/bin/env python3
"""Run the MPC SDK's tools/build.py with a different arm64 CPU target and optimization level, without
changing the SDK (it is a git submodule): the script is loaded as a module and two of its settings
are replaced before its main() runs.

Usage: py tools/sdk_build_tuned.py <sdk dir> <arm64 cpu, e.g. cortex_a76> <opt level, e.g. 3> <build.py args...>
The arm64 CPU must be one the device's cores all support (the MPC Live III's Rockchip RK3588: Cortex-A76
performance and Cortex-A55 efficiency cores, both ARMv8.2-A); the arm32 build is left as the SDK has it.
"""
import importlib.util
import sys
from pathlib import Path

sdk, cpu, opt, rest = Path(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4:]
spec = importlib.util.spec_from_file_location("sdk_build", sdk / "tools" / "build.py")
build = importlib.util.module_from_spec(spec)
sys.modules["sdk_build"] = build
spec.loader.exec_module(build)

target = list(build.ARM_TARGETS["arm64"])
target[1] = cpu                                    # zig -mcpu
target[3] = ["-mcpu=" + cpu.replace("_", "-")]     # gcc arch flags
build.ARM_TARGETS["arm64"] = tuple(target)

original = build.Builder.common_flags


def common_flags(self):
    return ["-O" + opt if f == "-O2" else f for f in original(self)]


build.Builder.common_flags = common_flags
print(f"[sdk_build_tuned] arm64 -mcpu={cpu}, -O{opt}", flush=True)
sys.exit(build.main(rest))
