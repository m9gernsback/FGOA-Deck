#!/usr/bin/env bash
# FGOA Deck 启动脚本 — 复刻 FGO_Launcher.ps1 运行时改写
# 用法: bash fgo-launch-deck.sh
# 可选环境变量:
#   GL_BACKEND=native|zink   GL 实现（默认 native = radeonsi，2026-09-21 验证：角色材质 237/238 正常渲染；
#                            zink = GL→Vulkan 旧默认，shader 237 的 VS bindless textureSize 触发其编译线程崩溃，仅留作对照）
#   FGOGLCOMPAT=0            禁用 AMD GL 兼容补丁（默认启用；NV bindless→SSBO 翻译层，缺它画面缺失）
#   FGO_ZH_DLL=1             加载中文资源 hook（Wine 下已知可能加载失败，失败时移除即可）
#   WINEDEBUG=err+all        打开 wine 调试输出
#   GLSHIM=0                 关闭 glshim 诊断 shim（2026-09-20 起默认开启，见下）
#   GLSHIM_STUB_IDS=...      定点 stub 着色器 id（默认无 stub——237/238 在 radeonsi 下正常，zink 时代遗产）
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
# Mesa 磁盘着色器缓存命中路径崩溃（libgallium NIR 访问器野指针，死在 glGetProgramiv/编译线程）。
# 2026-09-20 在 zink 实锤；2026-09-21 在 radeonsi 复现（同样预热即崩、止于 program 242 status query）
# ——libgallium 是两后端共享代码，此 bug 与后端无关 → 永久禁用。代价：每次启动现场编译着色器。
export MESA_SHADER_CACHE_DISABLE="${MESA_SHADER_CACHE_DISABLE:-true}"
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
if [ "$GLSHIM" = "1" ] && [ -f "$GAME_C/glshim.so" ]; then
  export LD_PRELOAD="$GAME_C/glshim.so"
  echo "[launch] glshim: 启用（stub=${GLSHIM_STUB_IDS:-无}）"
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
export FGO_EXIT_DIAGNOSTICS=1   # fgohook 退出诊断：hook NtTerminateProcess/ExitProcess，写 C:\FGOA\logs\fgo-exit-trace.log

# ---- 6. 用 bottle 的 soda runner 拉起 ----
RUNNER="$HOME/.var/app/com.usebottles.bottles/data/bottles/runners/soda-11.0-10"
[ -x "$RUNNER/bin/wine" ] || RUNNER=$(dirname "$(ls -d "$HOME/.var/app/com.usebottles.bottles/data/bottles/runners/"*/bin/wine | sort -V | tail -1)")
export WINEPREFIX="$BOTTLE"
export WINEDEBUG="${WINEDEBUG:-err+all}"   # 0.4 节起死因定位移交 glshim v5（exitlog/linklog 自带落盘）；+file/+winsock 已结案，需要时手动 WINEDEBUG=err+all,+file,+winsock 覆盖
cd "$APP"
echo "[launch] runner=$RUNNER deck=$FGO_DECK_CHANNEL"
INJECT_K=(-k 'C:\FGOA\fgoapifix.dll')
[ -n "$FGOGLCOMPAT_K" ] && INJECT_K+=(-k 'C:\FGOA\App\fgoglcompat.dll')
INJECT_K+=(-k 'C:\FGOA\App\fgohook.dll')
# amdipc IPC 报文转储（诊断 4102 用）：dll 存在即自动加载（与 wlanapi shim 的"拷文件即部署"模型一致），IPCDUMP=0 可强制关；结案后删 dll 即可
[ "${IPCDUMP:-auto}" != "0" ] && [ -f "$BOTTLE/drive_c/FGOA/ipcdump.dll" ] && INJECT_K+=(-k 'C:\FGOA\ipcdump.dll')
[ -n "${FGO_ZH_DLL:-}" ] && INJECT_K+=(-k 'C:\FGOA\App\zh\fgozh.dll')
"$RUNNER/bin/wine" inject.exe -d "${INJECT_K[@]}" \
  ago.exe -hdtv720 -w --wasapi-shared 2>&1 | tee "$LOGS/deck-inject-live.log"
