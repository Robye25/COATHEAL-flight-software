# Security Remediation

This repository previously contained personal shell files and an SSH private
key (`.ssh/id_ed25519` and friends, committed in the first commit and
deleted in the second).

**Status 2026-10-10:** the key had been rotated, but the history still
carried it. The history was rewritten with `git-filter-repo` (the script in
step 2) on the `feature/preflight-cleanup` branch: every commit SHA changed
and no reachable object holds the files any more. What remains to do:

1. Force-push every branch (`git push --force --all origin` and
   `git push --force --tags origin`) and have every clone re-cloned; a
   pull into an old clone would merge the old history back in.
2. GitHub keeps the old commits reachable by SHA (pull-request refs, forks,
   caches) until support purges them: ask via
   https://support.github.com with the old SHAs (`9a95c81`, `9698f70`).
3. Keep treating the old key as compromised; the rotation stands.

The original process:

## 1) Rotate the compromised SSH key

Run on the original host where the key was used:

```bash
./scripts/rotate_ssh_key.sh
```

Then remove old keys from all services (GitHub, servers, CI secrets).

## 2) Purge sensitive files from Git history

Preferred (`git-filter-repo`):

```bash
./scripts/purge_sensitive_history.sh
```

Fallback (if `git-filter-repo` unavailable): use BFG or `git filter-branch` manually.

## 3) Force-push sanitized history

```bash
git push --force-with-lease origin main
```

## 4) Invalidate cached credentials

- Remove cached deploy keys/tokens tied to old material.
- Regenerate CI deploy credentials if they used compromised private keys.