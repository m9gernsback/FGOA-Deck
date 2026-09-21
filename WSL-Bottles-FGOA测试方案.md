# WSL2 + Bottles (GE-Proton) 运行 FGOA 测试方案

> 本文档供在 **WSL 环境内的 CodeBuddy** 直接执行。
> 前置环境：WSL2 / Ubuntu 24.04 / systemd 已启用 / NVIDIA GPU（CUDA + D3D12 半虚拟化已确认）。
> Windows 侧游戏目录：`E:\Games\FGOA\FGOA`（WSL 内为 `/mnt/e/Games/FGOA/FGOA`）。

---

## 0. 背景与关键认知（先读）

### 0.1 FGOA 在 Windows 上的启动链

```
FGOLocalPlatform.exe (.NET 平台)
 ├─ Server/ : Python FastAPI 服务 + MariaDB（本地 ALL.Net / billing / AimeDB 模拟）
 └─ App/FGO_Launcher.ps1
     ├─ 复制 App/segatools.ini → DEVICE/runtime/segatools.runtime.ini 并改写大量配置项
     ├─ 将 runtime ini 重新编码为 UTF-16LE 带 BOM（GetPrivateProfileStringW 的要求）
     ├─ 设置 SEGATOOLS_CONFIG_PATH 与大量 FGO_* 环境变量
     └─ inject.exe -d [-k fgoglcompat.dll] -k fgohook.dll [-k zh\fgozh.dll] ago.exe -hdtv720 -w --wasapi-shared
          └─ ago.exe 自行拉起 am/amdaemon.exe
```

### 0.2 决定性风险点：OpenGL，不是 Vulkan/DXVK

ago.exe 11.00 使用 **OpenGL 渲染器**，且依赖 **NVIDIA bindless-buffer GL 扩展**
（见 FGO_Launcher.ps1 中 `preferHighPerformanceGpu` 注释）。

- 因此 **DXVK / Vulkan 与本测试无关**，Bottles 里应关闭 DXVK（它只影响 D3D，反而可能引入问题）。
- WSL2 的 OpenGL 由 **Mesa d3d12 转译层**提供（`/usr/lib/wsl/lib/libd3d12.so`），
  **不提供 NVIDIA 专属扩展**（GL_NV_bindless_texture 等）。
- **预期失败模式**：ago.exe 初始化 GL 时找不到必需扩展 → 崩溃或黑屏。
  本测试的核心就是验证这一点；若失败，结论不是「FGOA 不能跑 Linux」，
  而是「WSL 的 GL 转译层不满足」，真 Linux + 原生 NVIDIA 驱动仍有希望。

### 0.3 结果判读原则

| 结果 | 含义 |
|---|---|
| 进入游戏可玩 | 超出预期，WSL 方案直接成立 |
| 起进程但 GL 扩展缺失崩溃 | 预期结果 → 转真 Linux（双系统/GPU 直通）重测 |
| 注入链本身失败（inject/fgohook 报错） | Wine 对 CreateRemoteThread 注入链兼容性问题，需换注入方式或 winetricks 调优 |

---

## 1. 阶段一：环境侦察（确认 GL 能力）

```bash
sudo apt update && sudo apt install -y mesa-utils flatpak
flatpak remote-add --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo

echo "=== OpenGL renderer ==="
glxinfo -B | grep -iE "renderer|version|vendor"

echo "=== NVIDIA bindless 扩展是否存在（测试生死线） ==="
glxinfo | grep -iE "bindless|GL_NV_" | head -20 || echo "无 NVIDIA 扩展 -> ago.exe 大概率无法初始化渲染"
```

记录输出。若 `glxinfo -B` 的 renderer 是 `D3D12 (NVIDIA ...)` 字样，说明 Mesa 转译层工作正常，但扩展集受 Mesa 限制。

## 2. 阶段二：安装 Bottles + GE-Proton

```bash
flatpak install -y flathub com.usebottles.bottles
```

创建 gaming bottle 并安装 GE-Proton 运行器（bottles-cli 子命令随版本略有差异，先用 `flatpak run --command=bottles-cli com.usebottles.bottles --help` 确认）：

```bash
alias bottles-cli='flatpak run --command=bottles-cli com.usebottles.bottles'
bottles-cli list runners          # 查看可用运行器，挑最新的 ge-proton / soda 等
bottles-cli new --bottle-name FGOA --environment gaming --runner <最新ge-proton名>
```

Bottle 创建后位于：
`~/.var/app/com.usebottles.bottles/data/bottles/bottles/FGOA/`

> 备选：如果 bottles-cli 在 WSL 下不稳定，直接开 GUI（WSLg）：
> `flatpak run com.usebottles.bottles`，图形界面创建 bottle、装 ge-proton。

**Bottle 设置要求**（GUI 或 `bottles-cli edit`）：
- Runner：GE-Proton 最新版
- DXVK：**关闭**（ago.exe 是 OpenGL 游戏，不需要）
- 其余默认

## 3. 阶段三：迁移文件 + 搭建 Linux 原生服务器

### 3.1 复制游戏文件到 ext4

不要用 `/mnt/e`（9P 挂载，慢且权限语义不同）。直接复制进 bottle 的 drive_c，
让 segatools 配置里的 Windows 路径保持简单：

```bash
BOTTLE=~/.var/app/com.usebottles.bottles/data/bottles/bottles/FGOA
mkdir -p "$BOTTLE/drive_c/FGOA"
cp -r "/mnt/e/Games/FGOA/FGOA/App"     "$BOTTLE/drive_c/FGOA/"
cp -r "/mnt/e/Games/FGOA/FGOA/AMFS"    "$BOTTLE/drive_c/FGOA/"
cp -r "/mnt/e/Games/FGOA/FGOA/DEVICE"  "$BOTTLE/drive_c/FGOA/"
cp -r "/mnt/e/Games/FGOA/FGOA/GameData" "$BOTTLE/drive_c/FGOA/" 2>/dev/null || mkdir -p "$BOTTLE/drive_c/FGOA/GameData"
# 对应关系：installRoot = C:\FGOA ；gameRoot = C:\FGOA\App
```

### 3.2 服务器组件跑 Linux 原生

Windows 版 Server 目录里有 `Start-FGOLocalServer.ps1` 和 Windows venv，**不要**在 Wine 里跑它们。
在 Linux 侧重建：

```bash
# 参考 Windows 侧实现：/mnt/e/Games/FGOA/FGOA/Server/
sudo apt install -y mariadb-server python3-venv
cp -r /mnt/e/Games/FGOA/FGOA/Server ~/fgoa-server
cd ~/fgoa-server
python3 -m venv venv-linux
./venv-linux/bin/pip install -r requirements.txt  # 若无此文件，读 Start-FGOLocalServer.ps1 确认依赖后逐个装
```

MariaDB 数据初始化：参考 Windows 侧 Server 目录中的建库脚本/初始数据（检查 `Server/` 下
的 `.sql` 文件或 `Start-FGOLocalServer.ps1` 的初始化逻辑），导入 Linux MariaDB。

服务端口需与 `App/fgo-launcher.json` 的 `serverPorts`（http/billing/aime）一致，
且 `serverHost` 指向本机（WSL 内 Wine 进程与 Linux 服务共享网络命名空间，127.0.0.1 互通）。

> 注意 `App/FGO_LocalNetwork.ps1` 的网络规划逻辑（cabinet IP / subnet / broadcast），
> 复刻启动脚本时 `dns.default`、`netenv.*`、`keychip.subnet` 等 ini 项要与之对应。
> 单机本地测试建议直接把 serverHost 定为 127.0.0.1 并让 netenv 走本地配置。

## 4. 阶段四：复刻启动链（核心）

Windows 侧 `FGO_Launcher.ps1` 对 runtime ini 做的改写必须 1:1 复刻。
以下为 bash 版启动脚本骨架（保存为 `~/fgo-launch.sh`），**带 ⚠ 标记的行需要按实际配置核对**：

```bash
#!/usr/bin/env bash
set -euo pipefail

BOTTLE="$HOME/.var/app/com.usebottles.bottles/data/bottles/bottles/FGOA"
GAME_C="$BOTTLE/drive_c/FGOA"          # = C:\FGOA (installRoot)
APP="$GAME_C/App"                      # = C:\FGOA\App (gameRoot)
DEVICE="$GAME_C/DEVICE"
RUNTIME="$DEVICE/runtime"
LOGS="$GAME_C/logs"
mkdir -p "$RUNTIME" "$LOGS" "$DEVICE/print/players/unassigned"

# ---- 1. 生成 runtime ini（复刻 Set-IniValue 的全部改写） ----
cp "$APP/segatools.ini" "$RUNTIME/segatools.runtime.ini"
python3 - <<'EOF'
import configparser, pathlib
p = pathlib.Path.home() / ".var/app/com.usebottles.bottles/data/bottles/bottles/FGOA/drive_c/FGOA/DEVICE/runtime/segatools.runtime.ini"
c = configparser.ConfigParser(strict=False, interpolation=None)
c.optionxform = str
c.read(p)
def s(sec, k, v):
    if not c.has_section(sec): c.add_section(sec)
    c.set(sec, k, v)
# vfs（Windows 路径，Wine 内视角）
s("vfs","amfs",   r"C:\FGOA\AMFS")
s("vfs","option", r"C:\FGOA\App\option")
s("vfs","appdata",r"C:\FGOA\GameData")
s("aime","aimePath", r"C:\FGOA\DEVICE\aime.txt")
s("printer","mainFwPath",  r"C:\FGOA\DEVICE\printer_main_fw.bin")
s("printer","paramFwPath", r"C:\FGOA\DEVICE\printer_param_fw.bin")
s("printer","dspFwPath",   r"C:\FGOA\DEVICE\printer_dsp_fw.bin")
s("printer","printerOutPath", r"C:\FGOA\DEVICE\print\players\unassigned")  # ⚠ 有 aime 玩家时按 ps1 逻辑定位
s("keychip","billingCa",  r"C:\FGOA\DEVICE\ca.crt")
s("keychip","billingPub", r"C:\FGOA\DEVICE\billing.pub")
s("misc","nextProcessFilePath", r"C:\FGOA\DEVICE\NextProcess.txt")
# 网络 ⚠ 与 fgo-launcher.json / FGO_LocalNetwork.ps1 对齐
s("dns","default","127.0.0.1")
s("dns","startupPort","8443")    # ⚠ = serverPorts.http
s("dns","billingPort","8444")    # ⚠ = serverPorts.billing
s("dns","aimedbPort","22345")    # ⚠ = serverPorts.aime
s("netenv","enable","1")
s("netenv","routerSuffix","1")   # ⚠
s("netenv","addrSuffix","11")    # ⚠
s("netenv","broadcast","192.168.139.255")  # ⚠
s("keychip","subnet","192.168.139.0")      # ⚠
# 图形：先用 720p 窗口化降低变量
s("gfx","windowed","1"); s("gfx","framed","1")
s("gfx","width","1280"); s("gfx","height","720")
s("gfx","logicalWidth","1280"); s("gfx","logicalHeight","720")
s("gfx","preserveAspect","1"); s("gfx","monitor","0"); s("gfx","monitorDevice","")
s("amvideo","resolutionWidth","1280"); s("amvideo","resolutionHeight","720")
s("io4","mode","keyboard")       # ⚠ WSL 下 xinput 手柄未必可用，先 keyboard
s("touch","remap","1"); s("touch","inputWidth","1920"); s("touch","inputHeight","1080"); s("touch","nativeCoordinates","0")
s("system","freeplay","0")
for k,v in {"timezone":"0","daystart":"0","startHour":"0","startMinute":"0","timewarp":"0","writeable":"0"}.items():
    s("clock",k,v)
with open(p, "w", encoding="utf-8") as f: c.write(f)
EOF

# ---- 2. amdaemon 运行时配置（develop_version 覆盖，复刻 ps1 行为） ----
printf '{"credit":{"max_credit":99},"allnet_auth":{"develop_version":"11.00"}}\n' \
  > "$RUNTIME/amdaemon_main.json"   # ⚠ 版本号与 fgo-launcher.json gameVersion 一致

# ---- 3. runtime ini 转 UTF-16LE 带 BOM（GetPrivateProfileStringW 的要求，勿跳过） ----
python3 -c "
from pathlib import Path
p = Path('$RUNTIME/segatools.runtime.ini')
t = p.read_text(encoding='utf-8')
p.write_bytes(b'\xff\xfe' + t.encode('utf-16-le'))
"

# ---- 4. 环境变量（复刻 ps1 注入环境） ----
export SEGATOOLS_CONFIG_PATH='C:\FGOA\DEVICE\runtime\segatools.runtime.ini'
export FGO_TARGET_FPS=60
export FGO_PRINT_METADATA_ONLY=1
export FGO_FULL_SURFACE_FBO=1
export FGO_TEXTURE_QUALITY=0
export FGO_RENDER_SCALE=100
export FGO_SMAA=0
export FGO_SHADOW_RESOLUTION=0
export FGO_ANISOTROPY=16
export FGO_HIDE_TARGET_LINES=0
export FGO_DAMAGE_NUMBER_SCALE=100 FGO_DAMAGE_TEXTURE_SCALE=100
export FGO_DAMAGE_NUMBER_OPACITY=100 FGO_DAMAGE_TEXTURE_OPACITY=100
export FGO_PHOTO_KEY=120
export FGO_PHOTO_KEYS='87,83,65,68,81,69,37,39,38,40,90,67,82'
export FGO_MOTION_BLUR=1 FGO_DEPTH_OF_FIELD=1 FGO_BLOOM=1
export FGO_HIDE_UI=0 FGO_DISABLE_CAMERA_SHAKE=0 FGO_HIDE_CABINET_HUD=0
export FGO_HIDE_UI_KEY=121
export FGO_ZH_ENABLED=1
# FGO_DECK_CHANNEL = 'FGODeck_' + SHA256(大写去尾斜杠的 gameRoot) 大写hex
export FGO_DECK_CHANNEL="FGODeck_$(printf 'C:\FGOA\APP' | sha256sum | cut -d' ' -f1 | tr a-z A-Z)"
# 日文 locale hook（对应 processJapaneseLocale；若不启用可去掉 LRHookx64.dll 的 -k 项）
export LRCodePage=932 LRLCID=1041 LRBIAS=540 LRHookLCID=1

# ---- 5. 通过 bottle 的 wine 直接拉起（等效 bottles-cli run，但更可控） ----
WINE_BIN=$(ls -d ~/.var/app/com.usebottles.bottles/data/bottles/runners/*/bin/wine64 2>/dev/null | sort -V | tail -1)
[ -z "$WINE_BIN" ] && WINE_BIN=$(ls -d ~/.var/app/com.usebottles.bottles/data/bottles/runners/*/bin/wine | sort -V | tail -1)
export WINEPREFIX="$BOTTLE"
export WINEDEBUG=-all   # 需要排查时改为 err+all 或留空
cd "$APP"
"$WINE_BIN" inject.exe -d \
  -k 'C:\FGOA\App\Tools\Locale_Remulator\LRHookx64.dll' \
  -k 'C:\FGOA\App\fgohook.dll' \
  -k 'C:\FGOA\App\zh\fgozh.dll' \
  ago.exe -hdtv720 -w --wasapi-shared 2>&1 | tee "$LOGS/wsl-inject-live.log"
```

> 若存在 `App/fgoglcompat.dll`，把它作为**第一个** `-k`（必须早于 fgohook 加载）。
> 观察 `logs/wsl-inject-live.log`——inject 的 `-d` 调试输出能看到 ago.exe pre_startup
> 的完整日志，这是判断失败点的第一手材料。

### 启动顺序

1. 先起 Linux 原生服务器（MariaDB + Python 服务），确认 `fgo-launcher.json` 里的
   `requiredPorts` 全部监听：`ss -tlnp | grep -E '8443|8444|22345'`（端口以实际配置为准）
2. 再跑 `bash ~/fgo-launch.sh`

## 5. 已知坑（来自 Windows 侧 2026-09-16 排查记录，全部适用于本测试）

1. **漏设 `SEGATOOLS_CONFIG_PATH`** → hook 读 `App\segatools.ini` 里 width=0/height=0
   → fgohook 报 `Resolution mode: native-surface patch failed (hr=80070057)` →
   LoadLibrary 失败 → "DLL failed to load inside target process"。这不是真故障，是配置缺失。
2. runtime ini 必须是 **UTF-16LE 带 BOM**，否则 GetPrivateProfileStringW 读不到改写项。
3. ago.exe 的 PE 时间戳校验：fgohook 会校验 ago.exe 版本，确保复制过程没改动 ago.exe
   （cp 不会改内容，勿做任何 strip/patch）。
4. ago.exe 必须自己拉起 amdaemon，**不要**预启动或代理它（进程状态握手会断）。
5. WSL 无 Windows 音频端点概念；Wine 的 WASAPI 走 PulseAudio/PipeWire，
   若 ago.exe 因音频初始化失败，检查 `pipewire`/`wireplumber` 是否在跑，或尝试去掉 `--wasapi-shared`。

## 6. 结果记录模板

测试完成后把结论写回 Windows 侧 `E:\Games\FGOA\.workbuddy\memory\` 日志：

- [ ] GL renderer 字符串 / 是否有 NV bindless 扩展：
- [ ] inject.exe 是否成功注入（live log 前几行）：
- [ ] ago.exe 走到哪一步：pre_startup 输出 / GL 初始化 / ALL.Net 认证 / 进标题画面
- [ ] 失败点的完整报错（截取 live log 关键段）：
- [ ] 结论：WSL 可行 / 需真 Linux / 注入链需改造

## 7. 后续路线

- **若 GL 扩展缺失崩溃**（最可能）：在真 Linux（双系统 Ubuntu 或 GPU 直通虚拟机）
  + 原生 NVIDIA 驱动 + 同一套 Bottles 流程重测。本方案 3~6 节全部可复用，
  只有图形层从 Mesa-d3d12 换成 NVIDIA 原生 GL。
- **若注入链失败**：尝试 `bottles-cli` 的依赖安装（vcrun2019 等），
  或换 soda/caffe 等纯 Wine 运行器对比；CreateRemoteThread 注入在 Wine 下
  对同为 Wine 进程的目标理论可行，但需要实际验证。
