#!/usr/bin/env python3

# NOTE: this file is entirely LLM written. don't trust it 100%

"""Check that every language file defines the same keys as English.ini.

For each device folder under sonix-packer/assets/*/usr/resource/sonix/language:
  - every key in English.ini must exist in every other language file
  - no language file may have keys that English.ini lacks (stale/typo keys)
  - no file may define the same key twice
  - every placeholder (%s, %d, ...) in a translation must match English
Also checks that every tr("key") used in the C source exists in English.ini.

Exits non-zero if anything is wrong.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LANG_DIRS = sorted(ROOT.glob("sonix-packer/assets/*/usr/resource/sonix/language"))
SRC = ROOT / "sonix-player" / "src"
REFERENCE = "English.ini"
PLACEHOLDER = re.compile(r"%[-+#0]*\d*(?:\.\d+)?(?:hh|h|ll|l|z)?[diouxXcsfeEgGp%]")


def parse(path):
    keys, dupes = {}, []
    for n, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        s = line.strip()
        if not s or s.startswith((";", "#", "[")) or "=" not in s:
            continue
        key, value = (p.strip() for p in s.split("=", 1))
        if key in keys:
            dupes.append((key, n))
        keys[key] = value
    return keys, dupes


def main():
    errors = 0

    def err(msg):
        nonlocal errors
        errors += 1
        print(f"  {msg}")

    if not LANG_DIRS:
        print("no language folders found")
        return 1

    english_keys = set()
    for d in LANG_DIRS:
        device = d.parts[-5]
        ref, ref_dupes = parse(d / REFERENCE)
        english_keys |= ref.keys()
        print(f"[{device}] {len(ref)} keys in {REFERENCE}")
        for k, n in ref_dupes:
            err(f"{REFERENCE}:{n}: duplicate key '{k}'")

        for f in sorted(d.glob("*.ini")):
            if f.name == REFERENCE:
                continue
            keys, dupes = parse(f)
            missing = sorted(ref.keys() - keys.keys())
            extra = sorted(keys.keys() - ref.keys())
            bad_fmt = [k for k in ref.keys() & keys.keys()
                       if sorted(PLACEHOLDER.findall(ref[k])) != sorted(PLACEHOLDER.findall(keys[k]))]
            if not (missing or extra or dupes or bad_fmt):
                continue
            print(f" {f.name}:")
            for k in missing:
                err(f"missing: {k}")
            for k in extra:
                err(f"not in English: {k}")
            for k, n in dupes:
                err(f"line {n}: duplicate key '{k}'")
            for k in sorted(bad_fmt):
                err(f"placeholders differ from English: {k}")

    # Keys used in code that no English file defines.
    used = set()
    for c in list(SRC.rglob("*.c")) + list(SRC.rglob("*.h")):
        used |= set(re.findall(r'\btr\("([A-Za-z0-9_]+)"\)', c.read_text(encoding="utf-8", errors="replace")))
    undefined = sorted(used - english_keys)
    if undefined:
        print(" source:")
        for k in undefined:
            err(f"tr(\"{k}\") has no entry in {REFERENCE}")

    print("OK" if not errors else f"{errors} problem(s)")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
