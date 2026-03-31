#!/usr/bin/env bash
set -euo pipefail

KEY_PATH="${HOME}/.ssh/id_ed25519"
BACKUP_PATH="${HOME}/.ssh/id_ed25519.bak.$(date +%Y%m%d%H%M%S)"

if [[ -f "${KEY_PATH}" ]]; then
  cp "${KEY_PATH}" "${BACKUP_PATH}"
  cp "${KEY_PATH}.pub" "${BACKUP_PATH}.pub"
fi

ssh-keygen -t ed25519 -C "coatheal-rotated-$(date +%Y%m%d)" -f "${KEY_PATH}" -N ""

echo

echo "New public key:"
cat "${KEY_PATH}.pub"
echo

echo "Update GitHub/servers with the new key and remove the old one immediately."