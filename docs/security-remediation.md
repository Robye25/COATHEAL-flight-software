# Security Remediation

This repository previously contained personal shell files and an SSH private key. Use the following process before pushing updated history.

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