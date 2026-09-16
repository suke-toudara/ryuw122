#!/usr/bin/env python3
"""Validate main/Kconfig.projbuild and the per-role defaults in configs/.

Checks that the Kconfig file parses, that every CONFIG_ key used by
configs/*.defaults and by main/app_main.c actually exists, and that each
defaults file selects the intended role. Requires only `pip install kconfiglib`
(no ESP-IDF toolchain).
"""
import os
import re
import sys

import kconfiglib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
KCONFIG = os.path.join(ROOT, "main", "Kconfig.projbuild")

# Symbols defined by ESP-IDF itself, not by this project's Kconfig.
IDF_SYMBOLS = {"LOG_DEFAULT_LEVEL_INFO", "FREERTOS_HZ", "UART_ISR_IN_IRAM"}

EXPECTED = {
    "configs/anchor.defaults": {
        "RYUW122_ROLE_ANCHOR": "y",
        "RYUW122_LOCAL_ADDRESS": "ANCHOR01",
        "RYUW122_PEER_TAG_ADDRESS": "TAGT0001",
    },
    "configs/tag.defaults": {
        "RYUW122_ROLE_TAG": "y",
        "RYUW122_LOCAL_ADDRESS": "TAGT0001",
    },
}

failures = []


def check(condition, message):
    if not condition:
        failures.append(message)


os.chdir(ROOT)
kconf = kconfiglib.Kconfig(KCONFIG, warn_to_stderr=False)
print(f"ok - {KCONFIG} parses ({len(kconf.unique_defined_syms)} symbols)")

defined = {sym.name for sym in kconf.unique_defined_syms}

# Every CONFIG_ symbol referenced by the application must be defined.
source = open(os.path.join(ROOT, "main", "app_main.c")).read()
for name in sorted(set(re.findall(r"\bCONFIG_(RYUW122_\w+)", source))):
    check(name in defined, f"app_main.c uses CONFIG_{name} which Kconfig does not define")
print(f"ok - app_main.c references only defined symbols")

for path, expected in EXPECTED.items():
    kconf = kconfiglib.Kconfig(KCONFIG, warn_to_stderr=False)
    kconf.load_config(os.path.join(ROOT, path), replace=True)

    for line in open(os.path.join(ROOT, path)):
        match = re.match(r"\s*CONFIG_(\w+)=", line)
        if match and match.group(1) not in defined and match.group(1) not in IDF_SYMBOLS:
            failures.append(f"{path} sets unknown symbol CONFIG_{match.group(1)}")

    for name, want in expected.items():
        got = kconf.syms[name].str_value
        check(got == want, f"{path}: CONFIG_{name} is '{got}', expected '{want}'")

    # The two roles are mutually exclusive, so exactly one must be selected.
    roles = [kconf.syms[n].str_value for n in ("RYUW122_ROLE_ANCHOR", "RYUW122_ROLE_TAG")]
    check(roles.count("y") == 1, f"{path}: exactly one role must be selected, got {roles}")
    print(f"ok - {path}")

if failures:
    for failure in failures:
        print(f"FAIL {failure}", file=sys.stderr)
    sys.exit(1)
print("kconfig check passed")
