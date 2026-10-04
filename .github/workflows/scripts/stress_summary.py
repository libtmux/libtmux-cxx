"""Count each test's failures over the junit files ctest wrote.

Usage: stress_summary.py RESULTS_DIR PLATFORM REPEAT

Writes RESULTS_DIR/stress-PLATFORM.csv, one row per test, and appends a table
of the tests that failed at least once to $GITHUB_STEP_SUMMARY.
"""

import csv
import os
import re
import sys
import xml.etree.ElementTree as ET
from collections import Counter
from pathlib import Path


def main() -> int:
    """Write the CSV and step summary; return 1 when no result file exists."""
    results, platform, repeat = Path(sys.argv[1]), sys.argv[2], sys.argv[3]
    runs: Counter[str] = Counter()
    failures: Counter[str] = Counter()
    skips: Counter[str] = Counter()
    parents: dict[str, str] = {}
    files = sorted(results.glob("run-*.xml"))
    for path in files:
        for case in ET.parse(path).getroot().iter("testcase"):
            name = case.get("name", "")
            runs[name] += 1
            if case.find("failure") is not None or case.find("error") is not None:
                failures[name] += 1
                # A suite is one ctest test; name the cases inside it that failed.
                output = case.findtext("system-out") or ""
                for inner in re.findall(r"\[  FAILED  \] (\S+) \(", output):
                    key = f"{name} :: {inner}"
                    parents[key] = name
                    failures[key] += 1
            elif case.find("skipped") is not None or case.get("status") == "notrun":
                skips[name] += 1

    for key, parent in parents.items():
        runs[key] = runs[parent]

    out = results / f"stress-{platform}.csv"
    with out.open("w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["platform", "test", "runs", "failures", "skipped"])
        for name in sorted(runs):
            writer.writerow([platform, name, runs[name], failures[name], skips[name]])

    flaky = [name for name in sorted(runs) if failures[name]]
    lines = [
        (
            f"### {platform}: {len(files)} runs (up to {repeat}), "
            f"{len(runs)} tests, {len(flaky)} failed at least once"
        ),
        "",
    ]
    if flaky:
        lines += ["| test | failures | runs |", "| --- | ---: | ---: |"]
        lines += [f"| `{name}` | {failures[name]} | {runs[name]} |" for name in flaky]
    else:
        lines.append("No test failed.")
    text = "\n".join(lines) + "\n"
    print(text)
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with Path(summary).open("a") as handle:
            handle.write(text)
    # No result files means the loop itself broke.
    return 0 if files else 1


if __name__ == "__main__":
    sys.exit(main())
