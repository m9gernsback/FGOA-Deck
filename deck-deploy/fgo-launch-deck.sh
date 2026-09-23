#!/usr/bin/env bash
# FGOA Deck 启动脚本 — 复刻 FGO_Launcher.ps1 运行时改写
# 用法: bash fgo-launch-deck.sh
# 可选环境变量:
#   GL_BACKEND=native|zink   GL 实现（默认 native = radeonsi，2026-09-21 验证：角色材质 237/238 正常渲染；
#                            zink = GL→Vulkan 旧默认，shader 237 的 VS bindless textureSize 触发其编译线程崩溃，仅留作对照）
#   FGOGLCOMPAT=0            禁用 AMD GL 兼容补丁（默认启用；NV bindless→SSBO 翻译层，缺它画面缺失）
#   FGO_ZH_DLL=1             加载中文资源 hook（Wine 下已知可能加载失败，失败时移除即可）
#   WINEDEBUG=err+all        打开 wine 调试输出（默认 -all 全静音；实测 wine 日志仅占总日志量 0.2%）
#   GLSHIM_QUIET=0           glshim 恢复全量 dump（默认 1=安静：跳过 shader 源码/infolog/dlsym dump，
#                            保留 linklog/exitlog/linkfail/rewrite 异常现场记录）
#   IPCDUMP_QUIET=0          ipcdump 恢复全量报文转储（默认 1=安静：只留 cngfix 日志）
#   GAMEMODERUN=0            关闭 gamemoderun 包装（默认启用：gamemoderun 存在即自动套，
#                            切 CPU governor 到 performance 并提优先级；SteamOS 自带 gamemode）
#   GLSHIM=0                 关闭 glshim 诊断 shim（2026-09-20 起默认开启，见下）
#   GLSHIM_STUB_IDS=...      定点 stub 着色器 id（默认无 stub——237/238 在 radeonsi 下正常，zink 时代遗产）
#   MESA_PATCH=0             关闭 libgallium 哨兵补丁重定向（默认检测到 ~/Desktop/FGOA/mesa-patch/ 即启用，
#                            启用时启动自检 md5 并在 ago.exe 加载 GL 后把 maps 里的真实路径写进日志）
set -euo pipefail

BOTTLE="$HOME/.var/app/com.usebottles.bottles/data/bottles/bottles/FGOA"
GAME_C="$BOTTLE/drive_c/FGOA"          # = C:\FGOA (installRoot)
APP="$GAME_C/App"                      # = C:\FGOA\App (gameRoot)
DEVICE="$GAME_C/DEVICE"
RUNTIME="$DEVICE/runtime"
LOGS="$GAME_C/logs"
SRV="$BOTTLE/drive_c/FGOA-Server"
HTTP_PORT=8077   # 与 fgoa-deck-setup.sh 一致
GL_BACKEND="${GL_BACKEND:-native}"

# ---- GL 后端 ----
if [ "$GL_BACKEND" = "zink" ]; then
  export MESA_LOADER_DRIVER_OVERRIDE=zink
  echo "[launch] GL 后端: Zink (OpenGL→Vulkan→RADV)"
else
  unset MESA_LOADER_DRIVER_OVERRIDE || true
  echo "[launch] GL 后端: 原生 radeonsi"
fi
# ago.exe 无条件调用 NV 专属 GL 入口。fgoglcompat.dll（见下方 inject 链）在 Windows WGL 层
# 自建 resolver 提供这些入口并翻译着色器，正常时 Mesa 根本看不到 NV 调用；override 仅作兜底保留。
export MESA_EXTENSION_OVERRIDE="+GL_NV_bindless_texture +GL_NV_shader_buffer_load +GL_NV_vertex_buffer_unified_memory +GL_NV_vertex_attrib_integer_64bit +GL_NV_bindless_multi_draw_indirect"
# Mesa 磁盘着色器缓存命中恢复路径崩溃（_mesa_program_get_resource_name 解引用 -1 哨兵，
# 见 0.39 根因修正）已由 mesa-patch 根治。默认值随补丁状态自适应：补丁生效 → 默认开缓存
# （false，消除每局着色器重编译卡顿）；补丁缺失/被关 → 默认禁缓存（true，安全兜底）。
# 显式 MESA_SHADER_CACHE_DISABLE=true/false 可覆盖默认。
# libgallium 哨兵崩溃二进制补丁（0.39，用户态重定向，不动系统文件）：
# 游戏跑宿主机 Mesa（直跑 runner 不经 flatpak 沙箱），~/Desktop/FGOA/mesa-patch/
# 存在即通过 LD_LIBRARY_PATH + LIBGL_DRIVERS_PATH 重定向到补丁版 libgallium，
# 从而可以安全地 MESA_SHADER_CACHE_DISABLE=false（消除每局着色器重编译卡顿）。
# MESA_PATCH=0 强制关闭；mesa-patch-install.sh 部署 / mesa-patch-revert.sh 还原
MESA_PATCH_ON=0
MESA_PATCH_DIR="$HOME/Desktop/FGOA/mesa-patch"
MESA_PATCH_MD5=5989ee30468a11a476ee68bfa48d90c6
MESA_BASE_MD5=c1a3e616b4697cea9ee69a1c120dec9a
if [ "${MESA_PATCH:-1}" != "0" ] && [ -f "$MESA_PATCH_DIR/libgallium-25.3.0.so" ]; then
  export LD_LIBRARY_PATH="$MESA_PATCH_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  export LIBGL_DRIVERS_PATH="$MESA_PATCH_DIR/dri:/usr/lib/dri"
  MESA_PATCH_ON=1
  # 启动自检：补丁文件未被改坏 + 系统 Mesa 仍是补丁基准版本（SteamOS 更新后需重打）
  _pmd5=$(md5sum "$MESA_PATCH_DIR/libgallium-25.3.0.so" | cut -d' ' -f1)
  _smd5=$(md5sum /usr/lib/libgallium-25.3.0.so 2>/dev/null | cut -d' ' -f1 || echo missing)
  [ "$_pmd5" = "$MESA_PATCH_MD5" ] && _pok=OK || _pok="MISMATCH($_pmd5)"
  [ "$_smd5" = "$MESA_BASE_MD5" ] && _sok=OK || _sok="MISMATCH($_smd5)"
  echo "[launch] mesa-patch: 启用（补丁 md5 $_pok / 系统基准 $_sok）"
  [ "$_pok" = OK ] && [ "$_sok" = OK ] || echo "[launch] 警告: mesa-patch 校验异常，建议重跑 mesa-patch-install.sh 或 MESA_PATCH=0"
else
  echo "[launch] mesa-patch: 未启用（$MESA_PATCH_DIR 不存在或 MESA_PATCH=0）"
fi
if [ "$MESA_PATCH_ON" = "1" ]; then
  export MESA_SHADER_CACHE_DISABLE="${MESA_SHADER_CACHE_DISABLE:-false}"
else
  export MESA_SHADER_CACHE_DISABLE="${MESA_SHADER_CACHE_DISABLE:-true}"
fi
echo "[launch] Mesa 着色器缓存: $([ "$MESA_SHADER_CACHE_DISABLE" = "false" ] && echo 启用 || echo 禁用)"
# 缓存开关与补丁状态交叉检查：开缓存但没补丁 = 已知必崩组合，明确警告
if [ "${MESA_SHADER_CACHE_DISABLE}" = "false" ] && [ "$MESA_PATCH_ON" != "1" ]; then
  echo "[launch] 警告: 缓存已开启但 mesa-patch 未生效——命中 0.39 哨兵崩溃的风险极高！"
fi
# 运行时验证：ago.exe 起来后从 /proc/<pid>/maps 读真实加载路径写进日志（tee 的输出文件）
if [ "$MESA_PATCH_ON" = "1" ]; then
  (
    for _ in $(seq 1 150); do
      _pid=$(pgrep -x ago.exe | head -1)
      if [ -n "$_pid" ] && [ -r "/proc/$_pid/maps" ]; then
        _line=$(grep -m1 'libgallium' "/proc/$_pid/maps" 2>/dev/null) || true
        if [ -n "$_line" ]; then
          _path=$(echo "$_line" | awk '{print $NF}')
          if [[ "$_path" == *mesa-patch* ]]; then
            echo "[mesa-patch] 运行时验证通过: ago.exe(pid $_pid) 加载补丁版 $_path" | tee -a "$LOGS/deck-inject-live.log"
          else
            echo "[mesa-patch] 运行时验证失败: ago.exe(pid $_pid) 加载的是 $_path（补丁未生效！）" | tee -a "$LOGS/deck-inject-live.log"
          fi
          exit 0
        fi
      fi
      sleep 2
    done
    echo "[mesa-patch] 运行时验证超时: 300s 内未见 ago.exe 加载 libgallium" | tee -a "$LOGS/deck-inject-live.log"
  ) &
fi
# fgoglcompat.dll 检查：默认启用，FGOGLCOMPAT=0 关闭
FGOGLCOMPAT_K=""
if [ "${FGOGLCOMPAT:-1}" = "1" ]; then
  if [ -f "$APP/fgoglcompat.dll" ]; then
    FGOGLCOMPAT_K=1
    echo "[launch] fgoglcompat: 启用（NV bindless→SSBO 翻译层）"
  else
    echo "[launch] 警告: FGOGLCOMPAT=1 但 App/fgoglcompat.dll 不存在，请重跑 fgoa-deck-setup.sh"
  fi
else
  echo "[launch] fgoglcompat: 已禁用（FGOGLCOMPAT=0）"
fi
# glshim 诊断 shim（2026-09-20 起默认开启）：dump 着色器源码/编译日志到 /tmp/glshim/，
# 并内嵌 embedded-struct 提升改写（v8.1，离了它 12 个 shader 编译失败）+ 定点 stub。
# GLSHIM=0 关闭；GLSHIM_STUB_IDS 默认空（237/238 stub 是 zink 时代遗产，radeonsi 不需要）
GLSHIM="${GLSHIM:-1}"
export GLSHIM_STUB_IDS="${GLSHIM_STUB_IDS-}"
export GLSHIM_QUIET="${GLSHIM_QUIET:-1}"   # v8.2 安静模式：跳过全量 dump，保留异常现场记录
if [ "$GLSHIM" = "1" ] && [ -f "$GAME_C/glshim.so" ]; then
  export LD_PRELOAD="$GAME_C/glshim.so"
  echo "[launch] glshim: 启用（stub=${GLSHIM_STUB_IDS:-无} quiet=$GLSHIM_QUIET）"
else
  echo "[launch] glshim: 已禁用"
fi

# ---- 打印机输出账户：用 aime.txt 匹配玩家库（复刻 ps1 逻辑） ----
read -r PRINT_ACCOUNT PLAYER_NAME < <(python3 - <<EOF
import json, pathlib
code = pathlib.Path("$DEVICE/aime.txt").read_text(encoding='ascii', errors='ignore').strip()
try:
    players = json.load(open("$SRV/state/fgo-players.json", encoding='utf-8'))
    for k, v in players.items():
        if k.startswith("aime:") and str(v.get("auth_access_code", "")) == code:
            print(k.replace(":", "-"), v.get("master_name") or "player")
            break
    else:
        print("aime-1 player")
except Exception:
    print("aime-1 player")
EOF
)
echo "[launch] 打印账户: $PRINT_ACCOUNT ($PLAYER_NAME)"
mkdir -p "$RUNTIME" "$LOGS" "$DEVICE/print/players/$PRINT_ACCOUNT"

# ---- 1. 生成 runtime ini ----
cp "$APP/segatools.ini" "$RUNTIME/segatools.runtime.ini"
PRINT_ACCOUNT="$PRINT_ACCOUNT" HTTP_PORT="$HTTP_PORT" python3 - <<'EOF'
import configparser, os, pathlib
home = pathlib.Path.home()
p = home / ".var/app/com.usebottles.bottles/data/bottles/bottles/FGOA/drive_c/FGOA/DEVICE/runtime/segatools.runtime.ini"
c = configparser.ConfigParser(strict=False, interpolation=None, allow_no_value=True)
c.optionxform = str
c.read(p)
def s(sec, k, v):
    if not c.has_section(sec): c.add_section(sec)
    c.set(sec, k, v)
s("vfs","amfs",   r"C:\FGOA\AMFS")
s("vfs","option", r"C:\FGOA\App\option")
s("vfs","appdata",r"C:\FGOA\GameData")
s("aime","aimePath", r"C:\FGOA\DEVICE\aime.txt")
s("printer","mainFwPath",  r"C:\FGOA\DEVICE\printer_main_fw.bin")
s("printer","paramFwPath", r"C:\FGOA\DEVICE\printer_param_fw.bin")
s("printer","dspFwPath",   r"C:\FGOA\DEVICE\printer_dsp_fw.bin")
s("printer","printerOutPath", "C:\\FGOA\\DEVICE\\print\\players\\" + os.environ["PRINT_ACCOUNT"])
s("keychip","billingCa",  r"C:\FGOA\DEVICE\ca.crt")
s("keychip","billingPub", r"C:\FGOA\DEVICE\billing.pub")
s("misc","nextProcessFilePath", r"C:\FGOA\DEVICE\NextProcess.txt")
# 网络：server 192.168.100.1（绑在 lo），端口与 core.yaml 对齐
s("dns","default","192.168.100.1")
s("dns","startupPort",os.environ["HTTP_PORT"])
s("dns","billingPort","9999")
s("dns","aimedbPort","7777")
s("netenv","enable","1")
s("netenv","routerSuffix","1")
s("netenv","addrSuffix","11")
s("netenv","broadcast","127.0.0.1")
s("keychip","subnet","192.168.100.0")
# 图形：1280x720 窗口（Deck 屏幕 1280x800 可容纳）
s("gfx","windowed","1"); s("gfx","framed","1")
s("gfx","width","1280"); s("gfx","height","720")
s("gfx","logicalWidth","1280"); s("gfx","logicalHeight","720")
s("gfx","preserveAspect","1"); s("gfx","monitor","0"); s("gfx","monitorDevice","")
s("amvideo","resolutionWidth","1280"); s("amvideo","resolutionHeight","720")
s("io4","mode","xinput")       # Deck 自带手柄；无效则改 keyboard
s("touch","remap","1"); s("touch","inputWidth","1920"); s("touch","inputHeight","1080"); s("touch","nativeCoordinates","0")
s("system","freeplay","0")
for k,v in {"timezone":"0","daystart":"0","startHour":"0","startMinute":"0","timewarp":"0","writeable":"0"}.items():
    s("clock",k,v)
with open(p, "w", encoding="utf-8") as f: c.write(f)
EOF

# ---- 2. amdaemon develop_version 覆盖 ----
printf '{"credit":{"max_credit":99},"allnet_auth":{"develop_version":"11.00"}}\r\n' \
  > "$RUNTIME/amdaemon_main.json"

# ---- 3. player.json ----
printf '{"account": "%s", "name": "%s"}' "$PRINT_ACCOUNT" "$PLAYER_NAME" \
  > "$DEVICE/print/players/$PRINT_ACCOUNT/player.json"

# ---- 4. runtime ini 转 UTF-16LE 带 BOM ----
python3 -c "
from pathlib import Path
p = Path('$RUNTIME/segatools.runtime.ini')
t = p.read_text(encoding='utf-8')
p.write_bytes(b'\xff\xfe' + t.encode('utf-16-le'))
"

# ---- 5. 环境变量（值取自 fgo-launcher.json） ----
export SEGATOOLS_CONFIG_PATH='C:\FGOA\DEVICE\runtime\segatools.runtime.ini'
export FGO_LOCAL_NETWORK=1
export FGO_INSTALL_ROOT='C:\FGOA'
export FGO_LOCAL_HTTP_PORT=$HTTP_PORT FGO_LOCAL_BILLING_PORT=9999 FGO_LOCAL_AIME_PORT=7777
export FGO_TARGET_FPS=60
export FGO_PRINT_METADATA_ONLY=1
export FGO_FULL_SURFACE_FBO=1
export FGO_TEXTURE_QUALITY=0
export FGO_RENDER_SCALE=100
export FGO_SMAA="${FGO_SMAA:-1}"   # 排查期可用 FGO_SMAA=0 关闭（当前怀疑 SMAA 惰性编译在 Zink 上失败导致退出）
export FGO_SHADOW_RESOLUTION=1024
export FGO_ANISOTROPY=4
export FGO_HIDE_TARGET_LINES=1
export FGO_DAMAGE_NUMBER_SCALE=37.3 FGO_DAMAGE_TEXTURE_SCALE=27.8
export FGO_DAMAGE_NUMBER_OPACITY=36.8 FGO_DAMAGE_TEXTURE_OPACITY=18.6
export FGO_PHOTO_KEY=120
export FGO_PHOTO_KEYS='87,83,65,68,81,69,37,39,38,40,90,67,82'
export FGO_MOTION_BLUR=0 FGO_DEPTH_OF_FIELD=1 FGO_BLOOM=1
export FGO_HIDE_UI=0 FGO_DISABLE_CAMERA_SHAKE=1 FGO_HIDE_CABINET_HUD=1
export FGO_HIDE_UI_KEY=121
export FGO_ZH_ENABLED=1
export FGO_DECK_CHANNEL="FGODeck_$(printf 'C:\FGOA\APP' | sha256sum | cut -d' ' -f1 | tr a-z A-Z)"
export FGO_EXIT_DIAGNOSTICS="${FGO_EXIT_DIAGNOSTICS:-0}"   # fgohook 退出诊断（4102 已结案，默认关；排查时 =1，写 C:\FGOA\logs\fgo-exit-trace.log）

# ---- 6. 用 bottle 的 soda runner 拉起 ----
RUNNER="$HOME/.var/app/com.usebottles.bottles/data/bottles/runners/soda-11.0-10"
[ -x "$RUNNER/bin/wine" ] || RUNNER=$(dirname "$(ls -d "$HOME/.var/app/com.usebottles.bottles/data/bottles/runners/"*/bin/wine | sort -V | tail -1)")
export WINEPREFIX="$BOTTLE"
export WINEDEBUG="${WINEDEBUG:--all}"   # 默认全静音（0.30 收尾待办）；排查时 WINEDEBUG=err+all 或 err+all,+file,+winsock 覆盖
cd "$APP"
echo "[launch] runner=$RUNNER deck=$FGO_DECK_CHANNEL"
INJECT_K=(-k 'C:\FGOA\fgoapifix.dll')
[ -n "$FGOGLCOMPAT_K" ] && INJECT_K+=(-k 'C:\FGOA\App\fgoglcompat.dll')
INJECT_K+=(-k 'C:\FGOA\App\fgohook.dll')
# amdipc IPC 报文转储（诊断 4102 用）：dll 存在即自动加载（与 wlanapi shim 的"拷文件即部署"模型一致），IPCDUMP=0 可强制关
# 注意：ipcdump.dll 是 cngfix 载体=永久必需品勿删；v8 起 IPCDUMP_QUIET=1（默认）只留 cngfix 日志，=0 恢复全量转储
export IPCDUMP_QUIET="${IPCDUMP_QUIET:-1}"
[ "${IPCDUMP:-auto}" != "0" ] && [ -f "$BOTTLE/drive_c/FGOA/ipcdump.dll" ] && INJECT_K+=(-k 'C:\FGOA\ipcdump.dll')
[ -n "${FGO_ZH_DLL:-}" ] && INJECT_K+=(-k 'C:\FGOA\App\zh\fgozh.dll')
# Feral GameMode（Bottles 的"野兽模式"同款）：gamemoderun 存在即自动套，GAMEMODERUN=0 关闭
GAMEMODE_WRAP=()
if [ "${GAMEMODERUN:-1}" != "0" ] && command -v gamemoderun >/dev/null 2>&1; then
  GAMEMODE_WRAP=(gamemoderun)
  echo "[launch] gamemode: 启用（gamemoderun）"
else
  echo "[launch] gamemode: 未启用"
fi
"${GAMEMODE_WRAP[@]}" "$RUNNER/bin/wine" inject.exe -d "${INJECT_K[@]}" \
  ago.exe -hdtv720 -w --wasapi-shared 2>&1 | tee "$LOGS/deck-inject-live.log"
