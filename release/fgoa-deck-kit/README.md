# 在 Steam Deck 上运行 FGOA（Fate/Grand Order Arcade）

本指南介绍如何在 Steam Deck（SteamOS）上通过 Bottles + 本地服务器运行 FGOA。

> **声明**：本套件只包含部署脚本与兼容补丁，**不包含任何游戏数据**。
> 游戏本体请使用你自己合法持有的文件，仅用于个人学习与备份用途。

---

## 0. 你需要准备的东西

| 物品 | 说明 |
|---|---|
| Steam Deck | SteamOS 最新稳定版，已设置 sudo 密码（桌面模式终端里运行 `passwd` 设置） |
| 游戏本体 | 你自己的 FGOA 目录（内含 `App\ago.exe`、`DEVICE\`、`Server\` 等） |
| 本套件 | `fgoa-deck-kit`（本文件所在目录） |

全程在 **桌面模式** 下操作（按 Steam 键 → 电源 → 切换至桌面）。

---

## 1. 安装 Bottles 并创建 Bottle

1. 打开 **Discover** 商店，搜索并安装 **Bottles**。
2. 打开 Bottles，点左上角 **+** 新建 Bottle：
   - 名称必须叫 **`FGOA`**（脚本按此名称定位）
   - Runner 选择 **`soda-11.0-10`**（如列表没有，先到 Bottles 设置 → Runners 里下载）

创建完成后即可关闭 Bottles——之后游戏由脚本直接启动，不经过 Bottles 界面。

Bottle 的实际路径为：

```
~/.var/app/com.usebottles.bottles/data/bottles/bottles/FGOA/drive_c/
```

（即 Windows 视角的 `C:\`。在 Dolphin 文件管理器里需先按 `Ctrl+H` 显示隐藏文件才能看到 `.var`。）

---

## 2. 拷贝游戏与服务器文件

需要把游戏目录拷贝进 bottle 的 `drive_c`，共两处：

- **游戏本体**：整个 `FGOA` 目录 → 复制为 `drive_c/FGOA/`（即 `C:\FGOA`）
- **本地服务器**：游戏目录内的 **`FGOA/Server`** 子目录（里面自带 ARTEMiS 服务端 `artemis/` 和数据库 `data/`）→ 复制为 `drive_c/FGOA-Server/`
  - 注意：复制后 `drive_c/FGOA/Server` 请勿删除，游戏主数据 `fgo-master` 仍从原位置读取
  - 可选瘦身：`FGOA-Server` 里的 `venv/`、`python/`、`mariadb-*-winx64/` 是 Windows 专用组件，Deck 上用不到，可删掉省空间；但 **`data/` 必须保留**（`data/mariadb/` 是全部玩家进度）

最终结构如下：

```
drive_c/
├── FGOA/                 ← 游戏本体（C:\FGOA）
│   ├── App/
│   │   ├── ago.exe       ← 游戏主程序
│   │   ├── inject.exe、fgohook.dll、segatools.ini …
│   │   └── am/ …
│   ├── DEVICE/
│   │   ├── aime.txt      ← 你的卡号（刷卡登录用）
│   │   ├── ca.crt、billing.pub、打印机固件 …
│   │   └── …
│   └── Server/           ← 保留在原位（fgo-master 主数据从这里读）
│
└── FGOA-Server/          ← 由游戏的 FGOA/Server 复制而来（C:\FGOA-Server）
    ├── artemis/          ← ARTEMiS 服务端源码（含 config/）
    └── data/
        ├── fgo-master/
        └── mariadb/      ← 玩家数据库
```

> **注意**：`FGOA-Server/data/` 里是你的全部游戏进度。
> 删除 Bottle 会连带删除它，重大操作前请先备份这个目录。

---

## 3. 运行部署脚本

把本套件解压到任意位置（例如 `~/Desktop/FGOA`，脚本会自动定位自身目录），
然后打开 Konsole 终端，依次执行：

```bash
cd ~/Desktop/FGOA        # 进入套件目录，按实际位置修改

# ① 一次性初始化（约 5~10 分钟，需联网下载 MariaDB 与 Python 依赖）
bash fgoa-deck-setup.sh
```

`fgoa-deck-setup.sh` 会自动完成：

- 检查游戏/服务器文件是否就位，安装兼容补丁（fgoapifix / fgoglcompat / ipcdump / wlanapi / glshim）
- 创建服务器数据所需的 3 个符号链接（`App`、`DEVICE`、`fgo-master`）
- 解压 Linux 版 MariaDB（优先用套件同目录的 tarball，没有则从镜像站下载约 360MB）
- 将服务器 HTTP 端口从 777 改为 8077（免 root）
- 创建 Python venv 并安装 ARTEMiS 依赖
- 配置虚拟服务器 IP `192.168.100.1`（**这一步会要求输入 sudo 密码**，只需一次，重启自动生效）

```bash
# ② 安装 Mesa 着色器缓存补丁（修复游戏随机闪退，强烈建议）
bash mesa-patch-install.sh

# ③ 生成桌面快捷方式（可选）
bash fgoa-desktop-install.sh
```

---

## 4. 开始游玩

```bash
bash fgoa-play.sh
```

或双击桌面上的 **FGOA Play** 图标。该脚本会：启动本地服务器 → 启动游戏 → 你退出游戏后自动关闭服务器。

> **首次启动请耐心等待**：第一次运行时 AMD 兼容补丁要把游戏的几千个着色器逐个翻译为 Mesa 可编译的形式，窗口可能长时间黑屏/卡顿数分钟，属正常现象。翻译结果会缓存到 `App/shader-cache-r10`（**此目录勿删**），之后每次启动都会明显变快。

- 游戏以 1280×720 窗口运行，Deck 自带手柄即 Xinput 直接可用。
- 刷卡登录：读 `DEVICE\aime.txt` 里的卡号；新卡号会自动建档。
- 卡组编辑：双击桌面 **FGOA Deck Editor**（或 `cd carddeck && python3 server.py`），
  浏览器打开 <http://127.0.0.1:8931> 编辑，保存即写入游戏读取的 `App/deck.json`。

### 添加到 Steam，从 Game Mode 启动

执行过第 3 步③生成 `FGOA-Play.desktop` 后，可以把它加进 Steam 库：

1. 桌面模式下打开 **Steam**，左上角菜单 **游戏 → 添加非 Steam 游戏到我的库中**；
2. 点 **浏览**，文件类型选 **所有文件**，选中套件目录里的 `FGOA-Play.desktop`；
3. 添加后切回 Game Mode，在库的「非 Steam」分类中找到 **FGOA Play** 即可直接启动。

这样无需进桌面模式，Game Mode 里一键开玩（脚本会先拉起本地服务器再进游戏，退出游戏自动关服务器）。

单独控制服务器（不启动游戏）：

```bash
bash fgoa-server-start.sh   # 启动（看到 "Service OK" 即就绪）
bash fgoa-server-stop.sh    # 停止
```

---

## 5. 常见问题

**SteamOS 系统更新后游戏开始闪退？**
系统更新可能换掉了 Mesa 图形库，补丁随之失效——launch 脚本检测到版本不匹配会
**自动停用补丁回退安全模式**（能玩但启动慢），不会硬加载错版库。根治：先把
`/usr/lib/libgallium-<新版本>.so` 拷到能打补丁的机器，用
`python3 mesa-binpatch.py libgallium-<新版本>.so libgallium-<新版本>-patched.so`
重打（脚本按 md5 自动识别版本；md5 不在表内需先核对补丁点），拷回后
`bash mesa-patch-install.sh` 安装（会自动识别系统版本并清理旧版残留）。
套件自带 25.3.0 与 26.1.2 两版补丁，install 按当前系统自动选用。

**改动了服务器数据/符号链接后不生效？**
ARTEMiS 会把数据扫描结果缓存进内存（包括空结果）。改动后务必
`fgoa-server-stop.sh` 再 `fgoa-server-start.sh` 重启服务器。

**想看日志排查问题？**
游戏日志：bottle 内 `drive_c/FGOA/logs/deck-inject-live.log`；
服务器日志：`drive_c/FGOA-Server/logs/`。

**卸载 / 还原：**
- Mesa 补丁：`bash mesa-patch-revert.sh`（纯用户态部署，不碰系统文件）
- 虚拟 IP：`sudo systemctl disable --now fgoa-net.service && sudo rm /etc/systemd/system/fgoa-net.service`
- 其余直接删除套件目录与 Bottle 即可（注意先备份 `FGOA-Server/data/`）。

---

## 鸣谢

- **Vancion** — 提供的 AMD 显卡兼容补丁（fgoglcompat）
- **呵呵的Cloud23333** — 提供的游戏本体

---

## 套件内容一览

| 文件 | 作用 |
|---|---|
| `fgoa-deck-setup.sh` | 一次性初始化（第 3 步①） |
| `fgoa-play.sh` | 一键游玩（起服务器→起游戏→退出后停服务器） |
| `fgoa-server-start.sh` / `fgoa-server-stop.sh` | 单独启停本地服务器 |
| `fgo-launch-deck.sh` | 游戏启动器（被 play 调用，也可单独跑） |
| `mesa-patch-install.sh` / `mesa-patch-revert.sh` | Mesa 闪退补丁的安装与还原 |
| `mesa-binpatch.py` | SteamOS 更新 Mesa 后重新制作补丁 |
| `fgoa-desktop-install.sh` + `*.desktop.template` | 生成桌面快捷方式 |
| `fgoa-watch.sh` / `fgoa-collect-logs.sh` | 状态监视 / 日志打包（排查用） |
| `fgoapifix.dll` | 补齐 Wine 缺失的 user32 API（缺它触摸初始化崩溃） |
| `ipcdump.dll` / `wlanapi.dll` | cngfix 载体：修复 soda runner bcrypt 缺 ECC 派生导致的 4102 死锁（永久必需） |
| `fgoglcompat.dll` | FGO Prism 的 AMD GL 兼容补丁 v0.3.0（NV bindless→SSBO 翻译层，缺它画面缺失） |
| `glshim.so` | GL shim：内嵌 embedded-struct 着色器改写（缺它 12 个着色器编译失败） |
| `libgallium-25.3.0-patched.so` / `libgallium-26.1.2-patched.so` | 修复 Mesa 着色器磁盘缓存命中崩溃的补丁库（install 按系统版本自动选用） |
| `carddeck/` | 卡组编辑器（本地网页，端口 8931） |
