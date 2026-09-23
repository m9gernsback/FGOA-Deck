#!/usr/bin/env bash
# mesa-patch-install.sh — Deck 用户态部署 libgallium 哨兵崩溃补丁
#
# 原理（2026-09-23 修正）：fgo-launch-deck.sh 直接跑 runner 的 wine 二进制，
# 不经 flatpak 沙箱 → 游戏加载的是【宿主机 SteamOS 系统 Mesa】
# /usr/lib/libgallium-25.3.0.so（core 文件映射路径实锤）。SteamOS /usr 只读，
# 所以不碰系统文件，改用环境变量重定向：
#   LD_LIBRARY_PATH    → 覆盖 DT_NEEDED(libgallium-25.3.0.so) 解析
#   LIBGL_DRIVERS_PATH → 覆盖 Mesa loader 的 dri 驱动搜索（radeonsi_dri.so 等）
# 只影响从 fgo-launch-deck.sh 启动的进程，对其他应用/游戏零影响；
# 还原 = 删 ~/Desktop/FGOA/mesa-patch/ 或 MESA_PATCH=0。
#
# 用法: bash mesa-patch-install.sh [patched.so 路径]
set -euo pipefail

ORIG_MD5=c1a3e616b4697cea9ee69a1c120dec9a     # 制作补丁用的基准原版
PATCHED_MD5=5989ee30468a11a476ee68bfa48d90c6 # mesa-binpatch.py 产物
DIR="$(cd "$(dirname "$0")" && pwd)"
PATCHED="${1:-$DIR/libgallium-25.3.0-patched.so}"
TARGET="$HOME/Desktop/FGOA/mesa-patch"
SYS=/usr/lib/libgallium-25.3.0.so

[ -f "$PATCHED" ] || { echo "找不到补丁文件: $PATCHED"; exit 1; }
actual=$(md5sum "$PATCHED" | cut -d' ' -f1)
[ "$actual" = "$PATCHED_MD5" ] || { echo "补丁文件 md5 不符: $actual（期望 $PATCHED_MD5）"; exit 1; }

# 宿主机 Mesa 版本检查（SteamOS 更新会换掉它，补丁必须重做）
if [ -f "$SYS" ]; then
  sysmd5=$(md5sum "$SYS" | cut -d' ' -f1)
  if [ "$sysmd5" = "$ORIG_MD5" ]; then
    echo "[检查] 系统 $SYS 与补丁基准一致（25.3.0 原版）✓"
  else
    echo "[警告] 系统 Mesa 已变化（md5=$sysmd5），补丁基准是 25.3.0 原版。"
    echo "       若 SteamOS 刚更新过 Mesa，请把新 $SYS 拷回 WSL 重新打补丁后再继续。"
    exit 1
  fi
else
  echo "[警告] $SYS 不存在（系统 Mesa 版本变了？），中止"
  exit 1
fi

mkdir -p "$TARGET/dri"
cp "$PATCHED" "$TARGET/libgallium-25.3.0.so"
ln -sf ../libgallium-25.3.0.so "$TARGET/dri/radeonsi_dri.so"
ln -sf ../libgallium-25.3.0.so "$TARGET/dri/zink_dri.so"
chmod +x "$TARGET/libgallium-25.3.0.so"
echo "[完成] 补丁安装到 $TARGET"
echo "       （fgo-launch-deck.sh 检测到该目录即自动启用；MESA_PATCH=0 可临时关闭）"

cat <<'EOF'

== 复测步骤 ==
1. 确认 launch 脚本已更新（deck-deploy/fgo-launch-deck.sh 同步到 ~/Desktop/FGOA/）
2. 冷缓存: rm -rf ~/.cache/mesa_shader_cache* ~/.var/app/com.usebottles.bottles/cache/mesa_shader_cache*
3. MESA_SHADER_CACHE_DISABLE=false bash ~/Desktop/FGOA/fgoa-play.sh
4. 游戏中另开终端验证补丁真的加载了:
   grep -m2 -E 'libgallium|radeonsi_dri' /proc/$(pgrep -x ago.exe | head -1)/maps
   → 应显示 /home/deck/Desktop/FGOA/mesa-patch/ 下的路径
5. 不崩且第二局启动明显变快 = 根治成功；仍崩则抓 core（coredumpctl）回传
EOF
