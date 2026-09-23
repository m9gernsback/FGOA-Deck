#!/usr/bin/env bash
# fgoa-desktop-install.sh — 在脚本同目录生成 .desktop 快捷方式
# 模板（*.desktop.template）里的 @FGOA_DIR@ 替换为本脚本目录的绝对路径。
# 目录移动/改名后重跑一次本脚本即可（模板不动，可反复生成）。
set -euo pipefail

DIR="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"

for src in "$DIR"/*.desktop.template; do
  [ -f "$src" ] || { echo "未找到模板文件（$DIR/*.desktop.template）"; exit 1; }
  out="${src%.template}"
  sed "s|@FGOA_DIR@|$DIR|g" "$src" > "$out"
  chmod +x "$out"
  echo "[生成] $out"
done
echo "[完成] 快捷方式已指向 $DIR"
