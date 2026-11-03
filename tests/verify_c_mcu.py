"""Host-only C99 MCU profile matrix; no board, network or paid API required."""
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
profiles = {
    "base": [],
    "minimal": ["-DEMBED_AI_C_TOOLS=OFF", "-DEMBED_AI_C_HISTORY=OFF", "-DEMBED_AI_C_AUDIT=OFF"],
    "summary": ["-DEMBED_AI_C_COMPACTION=ON"],
}


def run(args):
    result = subprocess.run(args, cwd=ROOT, text=True, encoding="utf-8", errors="replace", capture_output=True)
    if result.returncode:
        print(result.stdout)
        print(result.stderr)
        raise SystemExit(result.returncode)
    return result.stdout


for name, flags in profiles.items():
    build = ROOT / "build" / ("c-mcu-verify-" + name)
    run(["cmake", "-S", str(ROOT / "c"), "-B", str(build), "-DEMBED_AI_C_PROFILE=mcu", "-DEMBED_AI_C_CURL=OFF", "-DEMBED_AI_C_MCP=OFF", *flags])
    run(["cmake", "--build", str(build), "--config", "MinSizeRel"])
    output = run(["ctest", "--test-dir", str(build), "-C", "MinSizeRel", "-V"])
    measurement = re.search(r"MCU profile passed: ([^\r\n]+)", output)
    if not measurement:
        raise RuntimeError("Missing allocator measurement")
    print(name + ": " + measurement.group(1), flush=True)
