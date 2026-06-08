#!/usr/bin/env bash
set -euo pipefail

repo_root="$(git rev-parse --show-toplevel)"
cd "$repo_root"

chmod +x .githooks/post-commit .githooks/pre-push scripts/perf/local_perf.sh
git config core.hooksPath .githooks

echo "MattSQL git hooks installed."
echo "core.hooksPath=$(git config --get core.hooksPath)"
echo
echo "post-commit records local performance metrics."
echo "pre-push records metrics and fails when the baseline gate regresses."
echo "Set MATTSQL_SKIP_PERF_HOOKS=1 to skip both hooks for one command."
