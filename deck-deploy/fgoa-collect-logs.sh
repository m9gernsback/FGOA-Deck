#!/usr/bin/env bash
# FGOA 日志收集：打包本轮运行日志到 $DIR/logs/<时间戳>/（游戏运行中也可执行）
DIR="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
BOTTLE="$HOME/.var/app/com.usebottles.bottles/data/bottles/bottles/FGOA/drive_c"
OUT="$DIR/logs/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT"
[ -d /tmp/glshim ] && cp -r /tmp/glshim "$OUT/glshim"
for f in "$BOTTLE/FGOA/App/captures/compat.log" \
         "$BOTTLE/FGOA/logs/deck-inject-live.log" \
         "$BOTTLE/FGOA/logs/fgo-exit-trace.log" \
         "$BOTTLE/FGOA/GameData/SDEJ/amdaemon.exe.log"; do
  [ -f "$f" ] && cp "$f" "$OUT/$(basename "$f")"
done
[ -d "$BOTTLE/logs" ] && cp -r "$BOTTLE/logs" "$OUT/server-logs"
# ARTEMiS/MariaDB 进程 stdout/stderr（服务器进程崩溃 traceback 在这里）
SRV="$BOTTLE/FGOA-Server"
[ -d "$SRV/logs" ] && mkdir -p "$OUT/server-proc-logs" && cp "$SRV/logs"/*.log "$OUT/server-proc-logs/" 2>/dev/null
echo "日志已收集到: $OUT"
