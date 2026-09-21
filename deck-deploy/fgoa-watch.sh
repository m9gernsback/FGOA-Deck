#!/usr/bin/env bash
# FGOA 挂死/闪退观测脚本 — 盯内存曲线 + ago.exe 线程状态
# 用法：先在另一个终端启动本脚本（它会等 ago.exe 出现），再启动游戏
#   bash ~/Desktop/FGOA/fgoa-watch.sh
# 输出：/tmp/memwatch.log（游戏进程消失后自动收尾，末尾 50 行是重点）

LOG=/tmp/memwatch.log
: > "$LOG"

echo "[watch] 等待 ago.exe 启动..." | tee -a "$LOG"
# 注意不能用 pgrep -f：会把命令行里带 ago.exe 的 wine/start.exe 包装进程也匹配进来
until pid=$(pgrep -x ago.exe | head -1); [ -n "$pid" ]; do
  sleep 1
done
echo "[watch] ago.exe pid=$pid，开始观测（$(date '+%F %T')）" | tee -a "$LOG"

while kill -0 "$pid" 2>/dev/null; do
  {
    echo "===== $(date '+%T') ====="
    free -m | head -2
    top -b -H -n1 -p "$pid" 2>/dev/null | tail -n +7 | head -12
  } >> "$LOG"
  sleep 2
done

echo "[watch] ago.exe 已消失（$(date '+%F %T')），日志在 $LOG" | tee -a "$LOG"
echo "[watch] 闪退后请执行: sudo dmesg | grep -iE 'oom|killed process|out of memory'" | tee -a "$LOG"
