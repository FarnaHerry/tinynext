#!/usr/bin/env bash
# make-linux-pkg.sh - Linux release packaging: portable tar.gz (via make-dist.sh)
# plus .deb and .rpm installers built from the staged dist/.
#
# Usage: bash packaging/make-linux-pkg.sh
#
# Requires the build to have happened (`mcpp build --release`). CI installs the
# packaging tool with: sudo apt-get install -y rpm
#
# Outputs (repo root):
#   tinynext-v<ver>-linux-x86_64.tar.gz     (portable, from make-dist.sh)
#   tinynext-v<ver>-linux-x86_64.deb
#   tinynext-v<ver>-linux-x86_64.rpm

set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
version="$(grep -m1 '^version' "$root/mcpp.toml" | sed -E 's/.*"([^"]+)".*/\1/')"

echo "== 1/3 make-dist.sh (tar.gz + dist/ + run.sh) =="
bash "$root/make-dist.sh" linux x86_64

echo "== 2/3 build-deb.sh =="
bash "$root/packaging/build-deb.sh" "$version"

echo "== 3/3 build-rpm.sh =="
bash "$root/packaging/build-rpm.sh" "$version"

echo "linux packaging done"
