#!/usr/bin/env bash
# build-rpm.sh - stage an .rpm from dist/ (created by make-dist.sh).
#
# Usage: bash packaging/build-rpm.sh <version>
#
# Requires rpmbuild (CI: sudo apt-get install -y rpm). rpmbuild runs on the
# ubuntu runner and produces an x86_64 binary rpm for Fedora/RHEL-class distros.

set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
version="${1:?usage: build-rpm.sh <version>}"

if ! command -v rpmbuild >/dev/null 2>&1; then
    echo "rpmbuild not found - install rpm tooling (apt-get install rpm)" >&2
    exit 1
fi

rm -rf "$root/rpmbuild"
mkdir -p "$root/rpmbuild"/{BUILD,RPMS,SOURCES,SPECS,BUILDROOT}

rpmbuild -bb \
    --define "_topdir $root/rpmbuild" \
    --define "_repo $root" \
    --define "appver $version" \
    "$root/packaging/tinynext.spec"

rpm="$root/rpmbuild/RPMS/x86_64/tinynext-$version-1.x86_64.rpm"
if [ ! -f "$rpm" ]; then
    echo "rpm not produced (looked at $rpm)" >&2
    exit 1
fi

out="$root/tinynext-v$version-linux-x86_64.rpm"
mv "$rpm" "$out"
echo "produced: $out"
