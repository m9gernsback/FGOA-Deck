# Steam Deck + Bottles (SODA) 运行 FGOA 部署方案

> 本文档面向 **Steam Deck 桌面模式手动部署**（Deck 上无 CodeBuddy，全程只需复制文件 + 跑 4 个脚本）。
> 前置认知来自 2026-09-17 WSL 实测（见 `WSL-Bottles-FGOA测试方案.md` 与 `.workbuddy/memory/2026-09-17.md`）：
> **注入链（inject + fgohook）在 Wine 11 / soda-11.0-10 下已验证可行；WSL 失败的唯一原因是 GL 转译层缺 bindless 扩展，Deck 是真 Linux，此阻塞不存在。**

---

## 0. 关键认知（先读）

1. ago.exe 是 **OpenGL 游戏**，需要 `GL_ARB_bindless_texture`。
   - Bottles 里的 **DXVK 开关与本游戏无关**（DXVK 只管 D3D9-11），保持关闭。
   - Deck 上 GL 有两条路径，由启动脚本的环境变量 `GL_BACKEND` 选择：
     - `zink`（默认）：OpenGL→Vulkan→RADV 转译。Zink 自 Mesa 21 起稳定支持 bindless，**保底可行**。
     - `native`：原生 radeonsi。性能最好，当前 Mesa 已支持 bindless，但需实测确认 SteamOS 自带版本。
   - 先用默认 zink 跑通，再试 `GL_BACKEND=native bash ~/fgo-launch-deck.sh` 对比性能。
2. 服务器组件（ARTEMiS + MariaDB）跑 **Linux 原生**，不在 Wine 里。
3. 端口调整：HTTP 777 → **8077**（777<1024 需要 root，8077 免折腾）；billing 9999 / aime 7777 / db 8888 不变。
4. 游戏连服务器走虚拟 IP `192.168.100.1`（绑在 lo 上，由一次性 systemd 服务完成）。

## 1. 用户手动步骤（图形界面 / 文件复制）

### 1.1 安装 Bottles
桌面模式 → Discover 商店 → 搜索安装 **Bottles**（flathub, com.usebottles.bottles）。

### 1.2 创建 bottle
打开 Bottles：
- 新建 Bottle：名称 **FGOA**，环境 **Gaming**
- 设置里 Runner 选 **soda-11.0-10**（若列表没有，先在 Bottles 设置 → Runners 里下载 SODA 11.0-10）
- **DXVK 保持关闭**（默认 gaming 模板会开，记得关）

### 1.3 复制文件
用文件管理器或 scp/rsync 从 PC 复制（26G 游戏 + 354M 服务器，建议有线或 SD 卡）：

| PC 侧（E:\Games\FGOA\） | Deck 侧 |
|---|---|
| `FGOA\App`、`FGOA\AMFS`、`FGOA\DEVICE`、`FGOA\GameData` | `~/.var/app/com.usebottles.bottles/data/bottles/bottles/FGOA/drive_c/FGOA/` 下 |
| `FGOA\Server` 整个目录 | `~/.var/app/com.usebottles.bottles/data/bottles/bottles/FGOA/drive_c/FGOA-Server/`（即 `FGOA-Server/artemis`、`FGOA-Server/data` …） |
| `deck-deploy\*.sh` 五个脚本 + `*.desktop` 三个启动器 | `~/Desktop/FGOA/`（桌面新建 FGOA 文件夹，全部放一起） |

> Server 目录里的 `venv/`、`python/`、`mariadb-10.11.16-winx64/` 是 Windows 专用，可不复制（复制了也无害）。
> 服务器放在 bottle 的 drive_c 里只是为了集中管理——它跑的是 Linux 原生进程，与 Wine 沙箱无关。
> **注意：在 Bottles 界面删除/重建 FGOA bottle 会连带删除服务器和玩家数据库**，操作前先备份 `FGOA-Server/data/`。

## 2. 一次性初始化（约 5 分钟）

Konsole 里执行：

```bash
bash ~/Desktop/FGOA/fgoa-deck-setup.sh
```

脚本自动完成：
- 准备 MariaDB 10.11.19 Linux 版到 `FGOA-Server/mariadb-linux/`（与 Windows 版同属 10.11 系列，数据库文件直接复用玩家数据，patch 版本完全兼容）。
  **优先使用本地 tarball**：若脚本同目录（或 `FGOA-Server/`、`/tmp`）下存在 `mariadb-10.11.19-linux-systemd-x86_64.tar.gz` 则直接解压，否则才从清华镜像下载（约 341MB）。`deck-deploy/` 里已附带该 tarball，随脚本一起拷到 Deck 即可全程离线。
- 写 MariaDB 配置（端口 8888）
- 把 ARTEMiS 的 HTTP 端口 777 改为 8077
- 建 Python venv、装依赖（自动跳过 pylibmc、自动补 msgpack、sqlalchemy 失败时自动放宽版本）
- **需要 sudo 的一步**：安装 `fgoa-net.service`（开机自动把 192.168.100.1 绑到 lo）。
  若未设过 deck 用户密码，先跑 `passwd` 设一个。

## 3. 每次游玩

方式一（命令行）：

```bash
bash ~/Desktop/FGOA/fgoa-server-start.sh    # 起 MariaDB + ARTEMiS，看到 "Service OK" 即可
bash ~/Desktop/FGOA/fgo-launch-deck.sh      # 起游戏
```

结束后：

```bash
bash ~/Desktop/FGOA/fgoa-server-stop.sh
```

方式二（桌面图标，推荐）：双击 `~/Desktop/FGOA/` 里的 `.desktop` 即可：

- **FGOA Play**：一键全流程——起服务器 → 起游戏 → 游戏退出后自动停服务器
- **FGOA Start Server** / **FGOA Stop Server**：手动分步控制（调试用）

> `.desktop` 里写死脚本路径为 `/home/deck/Desktop/FGOA/`；若换了目录，用文本编辑器改 `.desktop` 里的 Exec 路径。
> 首次双击若提示「不受信任」，右键 → 属性 → 权限 → 勾选「可执行」即可。

## 4. 可选开关

```bash
GL_BACKEND=native bash ~/fgo-launch-deck.sh    # 用原生 radeonsi GL（更快，先确认可行）
FGO_ZH_DLL=1 bash ~/fgo-launch-deck.sh         # 加载中文资源 hook（Wine 下已知可能加载失败）
WINEDEBUG=err+all,+wgl,+opengl bash ~/fgo-launch-deck.sh   # GL 调试日志
```

## 5. 已知问题（WSL 实测继承）

1. **fgozh.dll（中文资源）**：Wine 下 DllMain 在 REDIRECT_INDEX(files=1451) 后返回 FALSE 导致注入失败。Windows 正常，疑为 Wine 特定行为。默认不加载（`FGO_ZH_DLL=1` 才启用），不影响游戏本体。
2. runtime ini 必须是 UTF-16LE 带 BOM——脚本已自动处理，勿手动编辑后保存为其他编码。
3. ago.exe 会自己拉起 amdaemon，**不要**提前手动启动 amdaemon。
4. 音频走 `--wasapi-shared` → Wine → PipeWire，SteamOS 原生支持；若无声，去掉启动脚本末尾的 `--wasapi-shared` 试试。
5. 手柄：`io4.mode=xinput`（Deck 自带手柄）；若按键无响应，把 `~/fgo-launch-deck.sh` 里 `s("io4","mode","xinput")` 改成 `"keyboard"`（WASD/方向键操作）。

## 6. 排错指引

| 现象 | 看什么 |
|---|---|
| 服务器起不来 | `FGOA-Server/logs/artemis-stderr.log`（在 bottle 的 drive_c 下）；若报 `drive_c/logs/db.log` 不存在，执行 `mkdir -p .../FGOA/drive_c/logs`（新版 start 脚本已自动处理） |
| 注入失败 / DLL failed to load | bottle 内 `drive_c/FGOA/logs/deck-inject-live.log` 前几行 |
| 游戏崩溃 | 同一日志末尾；崩溃 dump 在 `drive_c/FGOA/logs/ago-crash-*.dmp` |
| GL 扩展缺失（`bindless ... not supported`） | 用默认的 `GL_BACKEND=zink`；若已是 zink 仍报缺，说明 bottle 运行时 Mesa 太旧，在 Bottles 里更新 |
| 连不上服务器 | `ip addr show lo \| grep 192.168.100.1`；`ss -tln \| grep -E '8077|9999|7777'` |

## 7. 若 GL 两条路都失败（兜底）

在 Konsole 验证扩展支持（SteamOS 自带 glxinfo；无则略过直接看启动日志）：

```bash
glxinfo | grep -c bindless                                    # 原生
MESA_LOADER_DRIVER_OVERRIDE=zink glxinfo | grep -c bindless   # zink
```

把 `deck-inject-live.log` 和上述输出带回 PC 侧分析。WSL 已证明注入链可靠，Deck 上剩余风险只在 GL 层与 fgozh。
