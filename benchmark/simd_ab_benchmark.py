#!/usr/bin/env python3
"""Compare simd_compare_benchmark builds at different CPPJIEBA_SIMD levels on one machine.

Each round runs every build once in a rotated order, plus the OFF build a second time, so each SIMD/OFF ratio is
paired with an OFF run from the same round and the report shows this machine's own OFF-versus-OFF noise beside it.
The min statistic suits runners whose whole runs slow down at random: interference only adds time, so the fastest
repetition over all rounds is the most repeatable estimate of each build.
"""

import argparse
import json
from pathlib import Path
import statistics
import subprocess
import sys
import tempfile


LEVELS = ("OFF", "SSE2", "AVX2", "NEON")
BASELINE = "OFF"
NOISE = "A/A"
MODES = ("MIX", "MP", "FULL", "SEARCH", "HMM")
OUTPUTS = ("strings", "borrowed", "reused")
FINGERPRINT_SUFFIXES = ("/tokens", "/ranges_fnv1a64")
UNIT_MS = {"ns": 1e-6, "us": 1e-3, "ms": 1.0, "s": 1e3}
# How one run's repetitions reduce to one time.
STATISTICS = {"median": statistics.median, "min": min}


def parse_variant(value):
    """Parse LEVEL=PATH, where LEVEL is a CPPJIEBA_SIMD value and PATH a simd_compare_benchmark executable."""
    level, separator, path = value.partition("=")
    if not separator or level not in LEVELS or not path:
        raise argparse.ArgumentTypeError(f"expected LEVEL=PATH with LEVEL one of {', '.join(LEVELS)}: {value}")
    return level, Path(path)


def positive_count(value):
    """Reject zero, negative and malformed round or repetition counts."""
    try:
        count = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError(f"expected a positive integer: {value}") from error
    if count <= 0:
        raise argparse.ArgumentTypeError(f"expected a positive integer: {value}")
    return count


def variant_levels(variants):
    """Order the requested levels with OFF first, rejecting duplicates and a missing baseline."""
    levels = [level for level, _ in variants]
    if len(set(levels)) != len(levels):
        raise ValueError("each SIMD level may be given only once")
    if BASELINE not in levels:
        raise ValueError("an OFF build is required as the baseline")
    if len(levels) < 2:
        raise ValueError("at least one SIMD build besides OFF is required")
    return [BASELINE] + [level for level in levels if level != BASELINE]


def round_order(labels, round_index):
    """Rotate the run order by one position per round, so no build always runs first or after the same build."""
    shift = round_index % len(labels)
    return labels[shift:] + labels[:shift]


def benchmark_key(name):
    """Sort benchmark names by mode and output as simd_compare_benchmark registers them; unknown names go last."""
    mode, _, output = name.partition("/")
    if mode in MODES and output in OUTPUTS:
        return (MODES.index(mode), OUTPUTS.index(output), name)
    return (len(MODES), 0, name)


def iteration_times(report):
    """Real time in milliseconds of each benchmark's repetitions; a benchmark error aborts the comparison."""
    times = {}
    for entry in report["benchmarks"]:
        if entry.get("error_occurred"):
            raise RuntimeError(f"{entry['name']}: {entry.get('error_message', 'benchmark error')}")
        if entry.get("run_type") != "iteration":
            continue
        name = entry["run_name"].removesuffix("/manual_time")
        times.setdefault(name, []).append(entry["real_time"] * UNIT_MS[entry["time_unit"]])
    if not times:
        raise RuntimeError("the report contains no benchmark iterations")
    return times


def output_fingerprint(report):
    """The per-mode token counts and token-range digests from the report context."""
    return {key: value for key, value in report["context"].items() if key.endswith(FINGERPRINT_SUFFIXES)}


def check_report(level, report, reference):
    """Reject a report from another build or with output that differs from the reference; return its fingerprint."""
    built = report["context"].get("simd_level")
    if built != level:
        raise RuntimeError(f"the {level} executable reports simd_level {built}")
    fingerprint = output_fingerprint(report)
    if not fingerprint:
        raise RuntimeError(f"the {level} report has no token counts or digests")
    if reference is not None and fingerprint != reference:
        differing = sorted(key for key in fingerprint.keys() | reference.keys()
                           if fingerprint.get(key) != reference.get(key))
        raise RuntimeError(f"{level} output differs from {BASELINE}: {', '.join(differing)}")
    return fingerprint


def collect(levels, run_build, rounds):
    """Run each level once per round in rotated order, OFF twice; return per-round repetition times and the fingerprint.

    run_build(level, run_index) runs one build and returns its Google Benchmark report.
    """
    labels = levels + [NOISE]
    measured = []
    fingerprint = None
    run_index = 0
    for round_index in range(rounds):
        times = {}
        for label in round_order(labels, round_index):
            level = BASELINE if label == NOISE else label
            report = run_build(level, run_index)
            run_index += 1
            fingerprint = check_report(level, report, fingerprint)
            times[label] = iteration_times(report)
        measured.append(times)
    return measured, fingerprint


def round_times(measured, label, name, statistic):
    """One reduced time per round for a label's benchmark."""
    reduce = STATISTICS[statistic]
    return [reduce(times[label][name]) for times in measured]


def fastest_time(measured, label, name):
    """The fastest repetition of a label's benchmark over all rounds."""
    return min(sample for times in measured for sample in times[label][name])


def summarize(measured, levels, statistic):
    """Per benchmark: the OFF time and, for each other level and A/A, its time ratio to OFF.

    [min, max] spans the per-round ratios of each run's reduced repetitions. The OFF time and the center are the
    median over rounds for the median statistic, and come from each label's fastest repetition for the min statistic.
    """
    names = sorted(measured[0][BASELINE], key=benchmark_key)
    rows = {}
    for name in names:
        baseline = round_times(measured, BASELINE, name, statistic)
        ratios = {}
        for label in levels[1:] + [NOISE]:
            per_round = [time / off for time, off in zip(round_times(measured, label, name, statistic), baseline)]
            if statistic == "min":
                center = fastest_time(measured, label, name) / fastest_time(measured, BASELINE, name)
            else:
                center = statistics.median(per_round)
            ratios[label] = {"center": center, "min": min(per_round), "max": max(per_round)}
        off_ms = fastest_time(measured, BASELINE, name) if statistic == "min" else statistics.median(baseline)
        rows[name] = {"off_ms": off_ms, "ratios": ratios}
    return rows


def verdict(ratio, noise):
    """faster or slower when every round's ratio lies outside the A/A range, else an empty string."""
    if ratio["max"] < noise["min"]:
        return "faster"
    if ratio["min"] > noise["max"]:
        return "slower"
    return ""


def format_change(ratio):
    """Central time change with the per-round range, in percent; negative is faster."""
    def percent(value):
        return f"{(value - 1) * 100:+.1f}"
    return f"{percent(ratio['center'])}% [{percent(ratio['min'])}, {percent(ratio['max'])}]"


def statistic_description(statistic):
    """How the table's numbers were computed, for the section header."""
    if statistic == "min":
        return ("Each SIMD column is the time change of the fastest repetition over all rounds against OFF's, "
                "[min, max] over rounds of the same ratio within each round;")
    return "Each SIMD column is the time change against the OFF run of the same round, median [min, max] over rounds;"


def format_markdown(title, settings, levels, rows):
    """Render one Markdown section; changes beyond this run's A/A range are bold."""
    others = levels[1:]
    lines = [
        f"### {title}",
        "",
        f"{settings['text']}, {settings['rounds']} rounds × {settings['repetitions']} repetitions. "
        f"{statistic_description(settings['statistic'])} "
        f"{NOISE} is a second OFF run, the noise on this machine. "
        f"**Bold**: every round lies outside the {NOISE} range.",
        "",
        "| Benchmark | OFF ms | " + " | ".join(others + [NOISE]) + " |",
        "| --- | ---: | " + " | ".join("---:" for _ in others + [NOISE]) + " |",
    ]
    for name, row in rows.items():
        noise = row["ratios"][NOISE]
        cells = []
        for level in others:
            ratio = row["ratios"][level]
            cell = format_change(ratio)
            cells.append(f"**{cell}**" if verdict(ratio, noise) else cell)
        cells.append(format_change(noise))
        lines.append(f"| {name} | {row['off_ms']:.3f} | " + " | ".join(cells) + " |")
    lines += ["", f"Output identical across {', '.join(levels)}: token counts and token-range digests of every mode.",
              ""]
    return "\n".join(lines)


def format_failure(title, error):
    """Render a Markdown section reporting why the comparison stopped."""
    return f"### {title}\n\n**Comparison failed:** {error}\n"


def run_executable(executable, inputs, settings, output):
    """Run one simd_compare_benchmark build and return its JSON report; a failing run raises with its stderr."""
    command = [
        str(executable), *inputs,
        f"--benchmark_repetitions={settings['repetitions']}",
        "--benchmark_enable_random_interleaving=true",
        f"--benchmark_min_time={settings['min_time']}",
        f"--benchmark_out={output}",
        "--benchmark_out_format=json",
    ]
    completed = subprocess.run(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, check=False)
    if completed.returncode != 0:
        raise RuntimeError(f"{executable} exited with {completed.returncode}: {completed.stderr.strip()[-2000:]}")
    return json.loads(output.read_text(encoding="utf-8"))


def write_outputs(arguments, markdown, data):
    """Write the requested Markdown and JSON files."""
    if arguments.markdown:
        arguments.markdown.write_text(markdown, encoding="utf-8")
    if arguments.json:
        arguments.json.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")


def parse_arguments():
    """Command-line interface; see benchmark/README.md."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--variant", type=parse_variant, action="append", required=True, metavar="LEVEL=PATH",
                        help="a simd_compare_benchmark build; OFF and at least one SIMD level are required")
    parser.add_argument("--dict", type=Path, required=True, help="main dictionary")
    parser.add_argument("--model", type=Path, required=True, help="HMM model")
    parser.add_argument("--text", type=Path, required=True, help="UTF-8 corpus")
    parser.add_argument("--rounds", type=positive_count, default=5, help="rounds over all builds (default 5)")
    parser.add_argument("--repetitions", type=positive_count, default=3,
                        help="Google Benchmark repetitions per run (default 3)")
    parser.add_argument("--min-time", default="0.2s", help="--benchmark_min_time per repetition (default 0.2s)")
    parser.add_argument("--statistic", choices=tuple(STATISTICS), default="median",
                        help="reduce repetitions by their median, or compare the fastest ones (default median)")
    parser.add_argument("--title", required=True, help="Markdown heading, such as the platform and compiler")
    parser.add_argument("--markdown", type=Path, help="write the Markdown section here")
    parser.add_argument("--json", type=Path, help="write the summary and per-round repetition times here")
    return parser.parse_args()


def run():
    """Measure, check and report; returns the process exit status."""
    arguments = parse_arguments()
    settings = {"text": arguments.text.name, "rounds": arguments.rounds, "repetitions": arguments.repetitions,
                "min_time": arguments.min_time, "statistic": arguments.statistic}
    try:
        levels = variant_levels(arguments.variant)
        executables = dict(arguments.variant)
        inputs = [str(arguments.dict), str(arguments.model), str(arguments.text)]
        with tempfile.TemporaryDirectory(prefix="simd-ab-") as directory:
            def run_build(level, run_index):
                output = Path(directory) / f"run-{run_index}-{level}.json"
                return run_executable(executables[level], inputs, settings, output)

            measured, fingerprint = collect(levels, run_build, arguments.rounds)
    except (OSError, KeyError, ValueError, RuntimeError) as error:
        failure = {"title": arguments.title, "error": str(error)}
        write_outputs(arguments, format_failure(arguments.title, error), failure)
        print(error, file=sys.stderr)
        return 1
    rows = summarize(measured, levels, arguments.statistic)
    markdown = format_markdown(arguments.title, settings, levels, rows)
    write_outputs(arguments, markdown, {"title": arguments.title, "settings": settings, "levels": levels,
                                        "benchmarks": rows, "fingerprint": fingerprint, "rounds": measured})
    print(markdown)
    return 0


if __name__ == "__main__":
    sys.exit(run())
