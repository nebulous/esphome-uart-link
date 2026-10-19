#!/usr/bin/env bash
# Validate every fixture with the installed ESPHome.
# Valid fixtures must validate; invalid fixtures must fail and print the
# sentence from the sibling .expect file.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

if ! command -v esphome >/dev/null 2>&1; then
  echo "esphome is not installed. pip install 'esphome==2026.9.0'" >&2
  exit 1
fi

echo "ESPHome $(esphome version)"
fail=0

shopt -s nullglob
for f in tests/valid/*.yaml; do
  if esphome config "$f" >/tmp/validate-err.txt 2>&1; then
    echo "PASS  valid   $(basename "$f")"
  else
    echo "FAIL  valid   $(basename "$f")"
    sed -n '1,10p' /tmp/validate-err.txt
    fail=1
  fi
done

for f in tests/invalid/*.yaml; do
  expect_file="${f%.yaml}.expect"
  if esphome config "$f" >/tmp/validate-err.txt 2>&1; then
    echo "FAIL  invalid $(basename "$f") (validated but must be rejected)"
    fail=1
    continue
  fi
  if grep -qF -f "$expect_file" /tmp/validate-err.txt; then
    echo "PASS  invalid $(basename "$f")"
  else
    echo "FAIL  invalid $(basename "$f") (error text did not match $(basename "$expect_file"))"
    sed -n '1,10p' /tmp/validate-err.txt
    fail=1
  fi
done

exit "$fail"
