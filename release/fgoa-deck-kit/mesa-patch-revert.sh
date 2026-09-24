#!/usr/bin/env bash
# mesa-patch-revert.sh — 还原 libgallium 补丁（用户态部署，只删自己的目录）
set -euo pipefail
DIR="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
TARGET="${MESA_PATCH_DIR:-$DIR/mesa-patch}"
if [ -d "$TARGET" ]; then
  rm -rf "$TARGET"
  echo "[已删除] $TARGET — 下次启动自动回到系统原版 Mesa"
else
  echo "未安装过补丁（$TARGET 不存在）"
fi
echo "提示: 补丁移除后 launch 脚本会自动回到默认禁缓存（安全兜底），无需手动改 MESA_SHADER_CACHE_DISABLE"
