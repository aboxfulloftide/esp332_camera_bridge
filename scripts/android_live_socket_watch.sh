#!/system/bin/sh

while true; do
  date +%s.%3N
  netstat -anp 2>/dev/null | grep -E '192\.168\.8\.1|:554 |:8080 |^udp|^udp6'
  echo ---
  sleep 0.5
done
