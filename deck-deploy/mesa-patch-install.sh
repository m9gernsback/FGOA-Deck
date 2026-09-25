#!/usr/bin/env bash
# mesa-patch-install.sh — Deck 用户态部署 libgallium 哨兵崩溃补丁（多版本）
#
# 原理（2026-09-23 修正）：fgo-launch-deck.sh 直接跑 runner 的 wine 二进制，
# 不经 flatpak 沙箱 → 游戏加载的是【宿主机 SteamOS 系统 Mesa】
# /usr/lib/libgallium-<版本>.so（core 文件映射路径实锤）。SteamOS /usr 只读，
# 所以不碰系统文件，改用环境变量重定向：
#   LD_LIBRARY_PATH    → 覆盖 DT_NEEDED(libgallium-<版本>.so) 解析
#   LIBGL_DRIVERS_PATH → 覆盖 Mesa loader 的 dri 驱动搜索（radeonsi_dri.so 等）
# 只影响从 fgo-launch-deck.sh 启动的进程，对其他应用/游戏零影响；
# 还原 = 删 <脚本目录>/mesa-patch/ 或 MESA_PATCH=0。
#
# 多版本（2026-09-25）：自动识别系统 Mesa 版本，要求补丁文件与之匹配。
# SteamOS 更新换掉系统 Mesa 后：把新 /usr/lib/libgallium-*.so 拷回 WSL 跑
# mesa-binpatch.py 重打，再回本脚本安装。
#
# 用法: bash mesa-patch-install.sh [patched.so 路径]（默认自动匹配脚本目录下的补丁产物）
set -euo pipefail

# 版本表：系统原版 md5 → 版本名 / 对应补丁产物 md5（与 mesa-binpatch.py VERSIONS 同步）
declare -A BASE_MD5=(
  [c1a3e616b4697cea9ee69a1c120dec9a]=25.3.0
  [3236bf4fe1b1e92117fbd2a882032ead]=26.1.2
)
declare -A PATCHED_MD5=(
  [25.3.0]=5989ee30468a11a476ee68bfa48d90c6
  [26.1.2]=868ae27b557c8a2eb3331c4cf990ad7e
)
DIR="$(cd "$(dirname "$0")" && pwd)"
# 补丁目录默认跟随脚本自身位置（整个 FGOA 目录放哪都行），可用 MESA_PATCH_DIR 覆盖
TARGET="${MESA_PATCH_DIR:-$DIR/mesa-patch}"

# 1) 识别系统 Mesa 版本
SYS=$(ls /usr/lib/libgallium-*.so 2>/dev/null | head -1) || true
[ -n "${SYS:-}" ] && [ -f "$SYS" ] || { echo "找不到 /usr/lib/libgallium-*.so，系统 Mesa 布局变了？"; exit 1; }
SONAME=$(basename "$SYS")
sysmd5=$(md5sum "$SYS" | cut -d' ' -f1)
VER=${BASE_MD5[$sysmd5]:-}
if [ -z "$VER" ]; then
  echo "[失败] 系统 $SONAME md5=$sysmd5 不在已知基准表内——是新版本 Mesa。"
  echo "       请把 $SYS 拷回 WSL，用 mesa-binpatch.py 对新文件重打并更新版本表后再来。"
  exit 1
fi
echo "[检查] 系统 Mesa: $SONAME（$VER 原版）✓"

# 2) 定位并校验补丁文件
PATCHED="${1:-$DIR/libgallium-$VER-patched.so}"
[ -f "$PATCHED" ] || { echo "找不到补丁文件: $PATCHED"
  echo "       先用 mesa-binpatch.py 制作: python3 mesa-binpatch.py <原版$SONAME> libgallium-$VER-patched.so"; exit 1; }
actual=$(md5sum "$PATCHED" | cut -d' ' -f1)
[ "$actual" = "${PATCHED_MD5[$VER]}" ] || { echo "补丁文件 md5 不符: $actual（$VER 期望 ${PATCHED_MD5[$VER]}）"; exit 1; }
echo "[检查] 补丁文件: $(basename "$PATCHED")（$VER 补丁版）✓"

# 3) 安装：清掉旧版本残留，装入本版，dri 符号链接指向本版
mkdir -p "$TARGET/dri"
rm -f "$TARGET"/libgallium-*.so "$TARGET/dri"/*.so
cp "$PATCHED" "$TARGET/$SONAME"
ln -sf "../$SONAME" "$TARGET/dri/radeonsi_dri.so"
ln -sf "../$SONAME" "$TARGET/dri/zink_dri.so"
chmod +x "$TARGET/$SONAME"
echo "[完成] 补丁安装到 $TARGET/$SONAME"
echo "       （fgo-launch-deck.sh 检测到该目录即自动启用；MESA_PATCH=0 可临时关闭）"

cat <<'EOF'

== 复测步骤 ==
1. 确认 launch 脚本已更新（deck-deploy/fgo-launch-deck.sh 同步到 Deck 上的 FGOA 脚本目录）
2. 冷缓存: rm -rf ~/.cache/mesa_shader_cache* ~/.var/app/com.usebottles.bottles/cache/mesa_shader_cache*
3. MESA_SHADER_CACHE_DISABLE=false bash <脚本目录>/fgoa-play.sh
4. 游戏中另开终端验证补丁真的加载了:
   grep -m2 -E 'libgallium|radeonsi_dri' /proc/$(pgrep -x ago.exe | head -1)/maps
   → 应显示 <脚本目录>/mesa-patch/ 下的路径
5. 不崩且第二局启动明显变快 = 根治成功；仍崩则抓 core（coredumpctl）回传
EOF
