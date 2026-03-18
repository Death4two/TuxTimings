#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

KEEP_UNTRACKED=0
if [[ "${1:-}" == "--keep-untracked" ]]; then
  KEEP_UNTRACKED=1
  shift
fi

if ! command -v git >/dev/null 2>&1; then
  echo "git is not installed."
  exit 1
fi

if ! git -C "$ROOT_DIR" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  echo "This is not a git clone. Download a fresh release/source archive instead."
  exit 1
fi

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
  cat <<'EOF'
Usage:
  ./update.sh                 Force-update to latest origin/main (default; overwrites + deletes)
  ./update.sh --keep-untracked  Keep untracked files/dirs (build artifacts, .deb, etc)

Notes:
  - Default behavior OVERWRITES local edits to tracked files AND deletes untracked files.
  - Use --keep-untracked if you want to preserve local build artifacts.
EOF
  exit 0
fi

echo "==> Updating TuxTimings (main)..."
git -C "$ROOT_DIR" fetch origin main
git -C "$ROOT_DIR" checkout -q main

git -C "$ROOT_DIR" reset --hard -q origin/main

if [[ "$KEEP_UNTRACKED" -eq 0 ]]; then
  git -C "$ROOT_DIR" clean -fd -q
fi

echo "==> Up to date."

