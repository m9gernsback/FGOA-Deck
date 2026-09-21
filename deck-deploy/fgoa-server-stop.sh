#!/usr/bin/env bash
# FGOA Deck 服务器停止
SRV="$HOME/.var/app/com.usebottles.bottles/data/bottles/bottles/FGOA/drive_c/FGOA-Server"

if [ -f "$SRV/state/artemis.pid" ]; then
  kill "$(cat "$SRV/state/artemis.pid")" 2>/dev/null && echo "ARTEMiS 已停止"
  rm -f "$SRV/state/artemis.pid"
fi
sleep 1
if [ -f "$SRV/state/mariadb.pid" ]; then
  kill "$(cat "$SRV/state/mariadb.pid")" 2>/dev/null && echo "MariaDB 已停止（等待落盘...）"
  rm -f "$SRV/state/mariadb.pid"
  sleep 3
fi
ss -tln | grep -E ':(8077|9999|7777|8888) ' || echo "端口已全部释放"
