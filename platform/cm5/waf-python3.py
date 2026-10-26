#!/usr/bin/env python3
"""Run the bundled Waf 2.0.14 on Python 3.11 and newer."""

import builtins
import runpy
import sys
import types
from pathlib import Path


if sys.version_info >= (3, 11):
    builtin_open = builtins.open

    def waf_compatible_open(file, mode="r", *args, **kwargs):
        return builtin_open(file, mode.replace("U", ""), *args, **kwargs)

    builtins.open = waf_compatible_open

if sys.version_info >= (3, 12):
    imp_compat = types.ModuleType("imp")
    imp_compat.new_module = types.ModuleType
    sys.modules["imp"] = imp_compat

repository = Path(__file__).resolve().parents[2]
waf = repository / "waf"
sys.argv[0] = str(waf)
runpy.run_path(str(waf), run_name="__main__")
