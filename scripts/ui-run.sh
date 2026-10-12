#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Run a command through scripts/lib/cli-ui.sh (progress spinner + compact diagnostics).
# Usage: scripts/ui-run.sh [--dir DIR] cmd [args...]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export UI_REPO_ROOT="$ROOT"
# shellcheck source=lib/cli-ui.sh
source "$ROOT/scripts/lib/cli-ui.sh"

if [[ "${1:-}" == "--dir" ]]; then
    shift
    [[ $# -ge 1 ]] || { ui_err "ui-run --dir needs a directory"; exit 2; }
    cd "$1"
    shift
fi

[[ $# -ge 1 ]] || { ui_err "ui-run needs a command"; exit 2; }
ui_run "$@"
