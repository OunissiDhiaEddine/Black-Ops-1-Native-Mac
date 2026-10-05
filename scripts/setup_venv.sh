#!/usr/bin/env bash
# Creates ./.venv and installs tools/requirements.txt. Project-local; touches nothing system-wide.
set -euo pipefail
cd "$(dirname "$0")/.."
[ -d .venv ] || python3 -m venv .venv
.venv/bin/python -m pip install -q --upgrade pip
.venv/bin/python -m pip install -q -r tools/requirements.txt
.venv/bin/python -m pip freeze
