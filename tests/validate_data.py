#!/usr/bin/env python3
"""Validate SandboxID identity data files: personas.tsv, devices.tsv.

Pure stdlib, no NDK/compiler needed — runnable on any host with python3 and
suitable as a CI gate alongside the shell-lint step. It mirrors the coherence
rules enforced on-device by selftest.sh (SDK<->release, security_patch format,
SoC/platform) and the column contracts documented in each TSV header.

Hard failures (wrong column count, SDK/release skew, malformed patch, a
non-Google persona with no devices.tsv provenance) set the exit code non-zero.
Softer oddities are reported as warnings and do not fail the run.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SDK_RELEASE = {"30": "11", "31": "12", "32": "12",
               "33": "13", "34": "14", "35": "15", "36": "16"}
TENSOR_PLATFORMS = {"gs101", "gs201", "zuma", "zumapro", "laguna"}
PATCH_RE = re.compile(r"^\d{4}-\d{2}-\d{2}$")
YM_RE = re.compile(r"^\d{4}-\d{2}$")

errors = []
warnings = []


def err(path, ln, msg):
    errors.append("%s:%d: ERROR: %s" % (os.path.basename(path), ln, msg))


def warn(path, ln, msg):
    warnings.append("%s:%d: warn: %s" % (os.path.basename(path), ln, msg))


def rows(path):
    """Yield (lineno, [fields]) for each non-comment, non-blank line."""
    out = []
    with open(path, encoding="utf-8") as fh:
        for i, line in enumerate(fh, 1):
            s = line.rstrip("\n")
            if not s.strip() or s.lstrip().startswith("#"):
                continue
            out.append((i, s.split("\t")))
    return out


def check_sdk_release(path, ln, sdk, release):
    if not sdk.isdigit():
        err(path, ln, "non-numeric sdk %r" % sdk)
        return
    exp = SDK_RELEASE.get(sdk)
    if exp is None:
        warn(path, ln, "sdk %s outside mapped range 30-36" % sdk)
    elif release.split(".")[0] != exp:
        err(path, ln, "sdk %s expects release %s, got %r" % (sdk, exp, release))


def validate_personas(path):
    n = 0
    models = []
    for ln, c in rows(path):
        n += 1
        if len(c) not in (10, 15, 16):
            err(path, ln, "expected 10/15/16 fields, got %d (%r...)" % (len(c), c[:2]))
            continue
        model, device, product, board, platform, sdk, release, bid, incr, patch = c[:10]
        for name, val in (("model", model), ("device", device), ("product", product),
                          ("board", board), ("platform", platform), ("id", bid),
                          ("incremental", incr)):
            if not val:
                err(path, ln, "empty required field %s" % name)
        check_sdk_release(path, ln, sdk, release)
        if not PATCH_RE.match(patch):
            err(path, ln, "security_patch not YYYY-MM-DD: %r" % patch)
        if len(c) == 10:
            if platform not in TENSOR_PLATFORMS:
                err(path, ln, "10-col (Google) row but platform %r not in %s"
                    % (platform, sorted(TENSOR_PLATFORMS)))
            models.append((ln, model, True))
        else:
            brand, soc_model = c[10], c[14]
            if not brand:
                err(path, ln, "non-Google row missing brand (col 11)")
            if not soc_model:
                err(path, ln, "non-Google row missing soc_model (col 15)")
            models.append((ln, model, brand.lower() == "google"))
    return n, models


def validate_devices(path):
    n = 0
    device_models = set()
    for ln, c in rows(path):
        n += 1
        if len(c) != 15:
            err(path, ln, "expected 15 fields, got %d (%r...)" % (len(c), c[:2]))
            continue
        (brand, _manuf, _market, model, device, product, board, _socman,
         socmod, sdk, release, bid, incr, patch, reldate) = c
        device_models.add(model)
        for name, val in (("brand", brand), ("model", model), ("device", device),
                          ("product", product), ("board", board),
                          ("soc_model", socmod), ("id", bid), ("incremental", incr)):
            if not val:
                err(path, ln, "empty required field %s" % name)
        check_sdk_release(path, ln, sdk, release)
        if not PATCH_RE.match(patch):
            err(path, ln, "security_patch not YYYY-MM-DD: %r" % patch)
        if not YM_RE.match(reldate):
            err(path, ln, "release_date not YYYY-MM: %r" % reldate)
    return n, device_models


def find_tsv(name):
    """Resolve a data file in both layouts: data/<name> in the repo, or <name>
    at the root of a build.sh-flattened module package."""
    for cand in (os.path.join(ROOT, "data", name), os.path.join(ROOT, name)):
        if os.path.exists(cand):
            return cand
    return os.path.join(ROOT, "data", name)


def main():
    p_personas = find_tsv("personas.tsv")
    p_devices = find_tsv("devices.tsv")

    np_, models = validate_personas(p_personas)
    nd, device_models = validate_devices(p_devices)

    for ln, model, is_google in models:
        if not is_google and model not in device_models:
            err(p_personas, ln,
                "non-Google persona model %r not found in devices.tsv (provenance)" % model)

    for w in warnings:
        print(w)
    for e in errors:
        print(e)
    print("validate_data: %d rows (%d personas, %d devices), "
          "%d errors, %d warnings"
          % (np_ + nd, np_, nd, len(errors), len(warnings)))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
