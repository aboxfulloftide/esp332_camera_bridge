#!/usr/bin/env bash
set -euo pipefail

adb shell <<'EOF'
su -c '
rm -f /data/local/tmp/selftest4.pcap
tcpdump -i any -s 0 -U -w /data/local/tmp/selftest4.pcap >/dev/null 2>&1 &
TPID=$!
echo "tcpdump_pid:$TPID"
sleep 1
ping -c 3 192.168.8.1 >/dev/null 2>&1 || true
sleep 1
kill -INT "$TPID"
wait "$TPID"
ls -lh /data/local/tmp/selftest4.pcap
'
EOF
