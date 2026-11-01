"""Generate firmware register definitions from the same profile used by the CLI."""
import json
from pathlib import Path
import sys


def generate(source, output):
    profile = json.loads(Path(source).read_text(encoding="utf-8"))
    lines = ["// Generated; edit profiles/deye-single.json instead.", "#pragma once",
             '#include "modbus_protocol.h"', "namespace deye {",
             f"constexpr uint8_t UNIT_ID = {profile['unit_id']};",
             "constexpr Group GROUPS[] = {"]
    for start, count in profile["groups"]:
        assert 0 <= start < 65536 and 1 <= count <= 125 and start + count <= 65536
        lines.append(f"    {{{start}, {count}}},")
    lines += ["};", "constexpr Metric METRICS[] = {"]
    for metric in profile["metrics"]:
        address = metric["register"]
        assert any(start <= address < start + count for start, count in profile["groups"])
        lines.append('    {"%s", %d, %s, %d, %d, %d},' % (
            metric["key"], address, str(metric.get("signed", False)).lower(),
            metric["multiplier"], metric.get("offset", 0), metric.get("maximum_raw", 65535)))
    lines += ["};", "enum Index {"]
    lines += [f"    {metric['key']} = {index}," for index, metric in enumerate(profile["metrics"])]
    lines += [f"    METRIC_COUNT = {len(profile['metrics'])}", "};", "} // namespace deye", ""]
    output = Path(output)
    content = "\n".join(lines)
    if not output.exists() or output.read_text(encoding="utf-8") != content:
        output.write_text(content, encoding="utf-8")


if __name__ == "__main__":
    generate(*sys.argv[1:])
