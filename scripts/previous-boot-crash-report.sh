#!/usr/bin/env bash
set -euo pipefail

BOOT_OFFSET="${1:--1}"

if ! journalctl --list-boots >/dev/null 2>&1; then
  echo "journalctl is unavailable on this system." >&2
  exit 1
fi

if ! journalctl -b "$BOOT_OFFSET" -n 1 >/dev/null 2>&1; then
  echo "No journal entries found for boot offset $BOOT_OFFSET."
  echo "This usually means the system has not completed another reboot since persistent logging was enabled."
  exit 0
fi

echo "== Boots =="
journalctl --list-boots
echo

echo "== Previous Boot: warnings and above =="
journalctl -b "$BOOT_OFFSET" -p warning..alert --no-pager || true
echo

echo "== Previous Boot: kernel log =="
journalctl -k -b "$BOOT_OFFSET" --no-pager || true
echo

echo "== Previous Boot: crash keywords =="
journalctl -b "$BOOT_OFFSET" --no-pager | \
  grep -iE 'oom|out of memory|panic|segfault|general protection|call trace|traceback|BUG:|I/O error|ext4-fs error|under-voltage|voltage|throttl|thermal|watchdog|hang|hung task|soft lockup|hard lockup|reset|reboot|fatal' || true
