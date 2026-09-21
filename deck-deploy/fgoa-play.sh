#!/usr/bin/env bash
# FGOA 一键游玩：起服务器 -> 起游戏 -> 游戏退出后自动停服务器
# 与其他四个脚本放同一目录即可（自动定位自身目录）
DIR="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"

bash "$DIR/fgoa-server-start.sh" || {
  echo
  read -rp "服务器启动失败，按回车退出..."
  exit 1
}

rm -rf /tmp/glshim   # glshim 现默认开启；stubbed.txt/loaded.txt 是追加模式，每轮清空
bash "$DIR/fgo-launch-deck.sh" || true

# 游戏退出后自动打包本轮日志（也可随时手动跑 fgoa-collect-logs.sh）
bash "$DIR/fgoa-collect-logs.sh"

echo
echo "===== 游戏已退出，正在停止服务器 ====="
bash "$DIR/fgoa-server-stop.sh"
echo
read -rp "按回车关闭窗口..."
