#!/usr/bin/env python3

import argparse
import hashlib
import json
import pathlib
import sys


ROOT = pathlib.Path(__file__).resolve().parent
DEFAULT_EXPECTATIONS = ROOT / "fixtures" / "gist-3f88bf26-expectations.json"
POLICY_GROUPS = (
    "ACTIONABLE_PRESENTATION",
    "OPT_IN_CPU_AGGREGATE",
    "EXPECTED_IMMUTABLE",
)


def fail(message):
    print(f"FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


def load_json(path):
    try:
        with path.open("r", encoding="utf-8") as handle:
            return json.load(handle)
    except (OSError, json.JSONDecodeError) as exc:
        fail(f"cannot read {path}: {exc}")


def validate_expectations(data):
    if data.get("schemaVersion") != 1:
        fail("unsupported expectation schema")
    source = data.get("source", {})
    digest = source.get("sha256", "")
    if len(digest) != 64 or any(c not in "0123456789abcdef" for c in digest):
        fail("source.sha256 must be lowercase 64-hex")

    counts = data.get("verdictCounts", {})
    if sum(counts.values()) != source.get("probeCount"):
        fail("verdict counts do not add up to probeCount")
    if counts.get("MISMATCH") != 16:
        fail("fixture must classify all 16 mismatches")

    policy = data.get("mismatchPolicy", {})
    missing = [group for group in POLICY_GROUPS if group not in policy]
    if missing:
        fail(f"missing mismatch policy groups: {', '.join(missing)}")
    classified = [probe for group in POLICY_GROUPS for probe in policy[group]]
    if len(classified) != 16 or len(set(classified)) != 16:
        fail("mismatch policy must contain 16 unique probe IDs")
    if len(policy["ACTIONABLE_PRESENTATION"]) != 12:
        fail("ACTIONABLE_PRESENTATION must contain 12 probes")
    if len(policy["OPT_IN_CPU_AGGREGATE"]) != 1:
        fail("OPT_IN_CPU_AGGREGATE must contain one probe")
    if len(policy["EXPECTED_IMMUTABLE"]) != 3:
        fail("EXPECTED_IMMUTABLE must contain three probes")

    preserve = data.get("preserveOutcomes", {})
    if not preserve:
        fail("preserveOutcomes groups must be non-empty")
    for category, expectation in preserve.items():
        if not isinstance(expectation, dict):
            fail(f"{category} must declare expectedVerdict and probes")
        verdict = expectation.get("expectedVerdict")
        probes = expectation.get("probes")
        if verdict not in counts:
            fail(f"{category} has unknown expectedVerdict: {verdict!r}")
        if not isinstance(probes, list) or not probes:
            fail(f"{category} probes must be a non-empty list")
        if len(probes) != len(set(probes)):
            fail(f"{category} contains duplicate probe IDs")
    return source, counts, policy, preserve


def validate_source(path, source, counts, policy, preserve):
    raw = path.read_bytes()
    actual_hash = hashlib.sha256(raw).hexdigest()
    if actual_hash != source["sha256"]:
        fail(f"source hash {actual_hash} does not match fixture")
    if len(raw) != source["bytes"]:
        fail("source byte count does not match fixture")
    if raw.count(b"\n") != source["newlineCharacters"]:
        fail("source newline count does not match fixture")
    logical_lines = raw.count(b"\n") + (0 if raw.endswith(b"\n") else 1)
    if logical_lines != source["logicalLines"]:
        fail("source logical line count does not match fixture")

    try:
        report = json.loads(raw)
    except json.JSONDecodeError as exc:
        fail(f"source is not valid JSON: {exc}")
    if str(report.get("appVersion")) != source["appVersion"]:
        fail("appVersion does not match fixture")
    results = report.get("results")
    if not isinstance(results, list) or len(results) != source["probeCount"]:
        fail("probe count does not match fixture")

    observed_counts = {}
    by_id = {}
    for result in results:
        verdict = result.get("verdict")
        observed_counts[verdict] = observed_counts.get(verdict, 0) + 1
        probe_id = result.get("spec", {}).get("id")
        if not probe_id or probe_id in by_id:
            fail(f"missing or duplicate probe id: {probe_id!r}")
        by_id[probe_id] = verdict
    if observed_counts != counts:
        fail(f"verdict counts differ: {observed_counts}")

    expected_mismatches = {probe for group in POLICY_GROUPS for probe in policy[group]}
    observed_mismatches = {probe for probe, verdict in by_id.items() if verdict == "MISMATCH"}
    if observed_mismatches != expected_mismatches:
        fail("mismatch IDs differ from policy fixture")
    for category, expectation in preserve.items():
        probes = expectation["probes"]
        expected_verdict = expectation["expectedVerdict"]
        missing = [probe for probe in probes if probe not in by_id]
        if missing:
            fail(f"{category} references absent probes: {', '.join(missing)}")
        wrong = [
            f"{probe}={by_id[probe]}" for probe in probes
            if by_id[probe] != expected_verdict
        ]
        if wrong:
            fail(
                f"{category} expected {expected_verdict}, observed: "
                + ", ".join(wrong)
            )


def main():
    parser = argparse.ArgumentParser(description="Validate the immutable probe expectation fixture")
    parser.add_argument("source", nargs="?", type=pathlib.Path,
                        help="optional immutable gist JSON to verify")
    parser.add_argument("--expectations", type=pathlib.Path,
                        default=DEFAULT_EXPECTATIONS)
    args = parser.parse_args()

    fixture = load_json(args.expectations)
    source, counts, policy, preserve = validate_expectations(fixture)
    if args.source:
        try:
            validate_source(args.source, source, counts, policy, preserve)
        except OSError as exc:
            fail(f"cannot read {args.source}: {exc}")
    suffix = " + source" if args.source else ""
    print(f"OK: probe expectations{suffix} ({source['probeCount']} probes, 16 classified mismatches)")


if __name__ == "__main__":
    main()
