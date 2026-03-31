#!/usr/bin/env bash
set -euo pipefail

if ! command -v git-filter-repo >/dev/null 2>&1; then
  echo "git-filter-repo not found. Install it first: https://github.com/newren/git-filter-repo"
  exit 1
fi

git filter-repo --force \
  --path .ssh/id_ed25519 --invert-paths \
  --path .ssh/id_ed25519.pub --invert-paths \
  --path .ssh/known_hosts --invert-paths \
  --path .ssh/known_hosts.old --invert-paths \
  --path .bash_history --invert-paths \
  --path .bashrc --invert-paths \
  --path .bash_logout --invert-paths \
  --path .profile --invert-paths \
  --path .gitconfig --invert-paths \
  --path .sudo_as_admin_successful --invert-paths \
  --path .config/labwc/environment --invert-paths

echo "Sensitive history purged locally. Verify and force-push when ready."