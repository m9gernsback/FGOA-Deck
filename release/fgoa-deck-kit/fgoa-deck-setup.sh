#!/usr/bin/env bash
# FGOA Deck 一次性初始化：MariaDB tarball + Python venv + 端口调整 + 虚拟服务器 IP
# 用法: bash fgoa-deck-setup.sh
set -euo pipefail

BOTTLE="$HOME/.var/app/com.usebottles.bottles/data/bottles/bottles/FGOA"
SRV="$BOTTLE/drive_c/FGOA-Server"
MARIA_VER="10.11.19"
MARIA_URL="https://mirrors.tuna.tsinghua.edu.cn/mariadb/mariadb-${MARIA_VER}/bintar-linux-systemd-x86_64/mariadb-${MARIA_VER}-linux-systemd-x86_64.tar.gz"
HTTP_PORT=8077   # 原 777 <1024 需 root，改为非特权端口

echo "==> [1/6] 检查前置文件"
[ -f "$BOTTLE/drive_c/FGOA/App/ago.exe" ] || { echo "缺少 bottle 内游戏文件：$BOTTLE/drive_c/FGOA/App/ago.exe"; exit 1; }
[ -d "$SRV/artemis" ] || { echo "缺少服务器目录：$SRV（应包含 artemis/ data/ 等）"; exit 1; }
ls -d "$HOME/.var/app/com.usebottles.bottles/data/bottles/runners/soda-11.0-10" >/dev/null 2>&1 \
  || echo "警告: 未找到 soda-11.0-10 runner，请在 Bottles 里确认 runner 版本"
# fgoapifix.dll（Wine API 补丁）：脚本同目录有则自动装入 bottle
SCRIPT_DIR="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
if [ -f "$SCRIPT_DIR/fgoapifix.dll" ]; then
  cp "$SCRIPT_DIR/fgoapifix.dll" "$BOTTLE/drive_c/FGOA/fgoapifix.dll"
  echo "fgoapifix.dll 已安装到 C:\FGOA\\"
else
  echo "警告: 未找到 fgoapifix.dll（Wine 下必需，否则游戏在触摸初始化时崩溃）"
fi
# glshim.so（GL 诊断 shim，GLSHIM=1 时使用）：有则装入 bottle
[ -f "$SCRIPT_DIR/glshim.so" ] && cp "$SCRIPT_DIR/glshim.so" "$BOTTLE/drive_c/FGOA/glshim.so" && echo "glshim.so 已安装到 C:\FGOA\\"
# fgoglcompat.dll（AMD GL 兼容补丁，NV bindless→SSBO 翻译层）：装到 App\（须与 ago.exe 同目录）
if [ -f "$SCRIPT_DIR/fgoglcompat.dll" ]; then
  if [ "$(sha256sum "$SCRIPT_DIR/fgoglcompat.dll" | cut -d' ' -f1)" = "6a7d3b6086cdf7f0b9e4bd3c3a05d514ecb245f42a70e4b5151847ebb96b16d6" ]; then
    cp "$SCRIPT_DIR/fgoglcompat.dll" "$BOTTLE/drive_c/FGOA/App/fgoglcompat.dll"
    echo "fgoglcompat.dll v0.3.0 已安装到 C:\FGOA\App\\"
  else
    echo "警告: fgoglcompat.dll 哈希不匹配 v0.3.0，未安装（请核对版本）"
  fi
else
  echo "警告: 未找到 fgoglcompat.dll（AMD 兼容补丁，缺失时 NV 指针着色器无法翻译）"
fi
# ipcdump.dll（amdipc 报文转储 + cngfix：修复 soda bcrypt 缺 ECC secret 派生=4102 根因）：永久必需
if [ -f "$SCRIPT_DIR/ipcdump.dll" ]; then
  cp "$SCRIPT_DIR/ipcdump.dll" "$BOTTLE/drive_c/FGOA/ipcdump.dll"
  echo "ipcdump.dll 已安装到 C:\FGOA\\"
else
  echo "警告: 未找到 ipcdump.dll（cngfix 载体，缺失则 amdipc ECK1 密钥协商死锁=4102）"
fi
# wlanapi.dll（amdaemon 侧 cngfix 载体，App\am\ 应用目录 shadow）：永久必需
if [ -f "$SCRIPT_DIR/wlanapi.dll" ]; then
  cp "$SCRIPT_DIR/wlanapi.dll" "$BOTTLE/drive_c/FGOA/App/am/wlanapi.dll"
  echo "wlanapi.dll 已安装到 C:\FGOA\App\am\\"
else
  echo "警告: 未找到 wlanapi.dll（amdaemon 侧 cngfix 载体，缺失=4102）"
fi
# 数据符号链接（4102 收尾 0.31）：ARTEMiS 的 app_root 写死为包上溯 4 层+App，
# artemis 在 drive_c\FGOA-Server\ → 服务器找 drive_c\App 和 drive_c\Server\data\fgo-master，
# 数据实际在 drive_c\FGOA\ 下 → 符号链接补齐。永久组成部分，勿删。
ln -sfn FGOA/App "$BOTTLE/drive_c/App"
# drive_c\DEVICE：服务端 _load_card_catalog 用 normpath(join(app_root, CardsPath))
# 词法折叠 drive_c/App/../DEVICE → drive_c/DEVICE，不经过 App 链接 → 必须单独补，
# 否则 call_up 从者卡全部判 invalid（0.35 根因）。
ln -sfn FGOA/DEVICE "$BOTTLE/drive_c/DEVICE"
mkdir -p "$BOTTLE/drive_c/Server/data"
ln -sfn ../../FGOA/Server/data/fgo-master "$BOTTLE/drive_c/Server/data/fgo-master"
echo "符号链接已就绪: drive_c\App、drive_c\DEVICE、drive_c\Server\data\fgo-master"
mkdir -p "$SRV/state" "$SRV/logs"

echo "==> [2/6] 准备 MariaDB ${MARIA_VER} Linux 版"
if [ ! -x "$SRV/mariadb-linux/bin/mariadbd" ]; then
  SCRIPT_DIR="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
  TARBALL=""
  for c in "$SCRIPT_DIR/mariadb-${MARIA_VER}-linux-systemd-x86_64.tar.gz" \
           "$SRV/mariadb-${MARIA_VER}-linux-systemd-x86_64.tar.gz" \
           "/tmp/mariadb-${MARIA_VER}-linux-systemd-x86_64.tar.gz"; do
    [ -f "$c" ] && TARBALL="$c" && break
  done
  if [ -z "$TARBALL" ]; then
    echo "未找到本地 tarball，从网络下载（约 362MB）..."
    TARBALL="/tmp/mariadb-deck.tar.gz"
    curl -L "$MARIA_URL" -o "$TARBALL"
  else
    echo "使用本地 tarball: $TARBALL"
  fi
  mkdir -p "$SRV/mariadb-linux"
  tar -xzf "$TARBALL" -C "$SRV/mariadb-linux" --strip-components=1
fi
"$SRV/mariadb-linux/bin/mariadbd" --version

echo "==> [3/6] 写入 MariaDB 配置"
cat > "$SRV/mariadb-deck.ini" <<EOF
[mariadbd]
basedir=$SRV/mariadb-linux
datadir=$SRV/data/mariadb
port=8888
bind-address=127.0.0.1
socket=$SRV/state/mariadb.sock
skip-name-resolve
character-set-server=utf8mb4
collation-server=utf8mb4_unicode_ci
max_connections=50
max_allowed_packet=64M
performance_schema=OFF
skip-log-bin
innodb_flush_log_at_trx_commit=2
sync_binlog=0
log-error=$SRV/logs/mariadb.log
pid-file=$SRV/state/mariadb-engine.pid

[client]
host=127.0.0.1
port=8888
socket=$SRV/state/mariadb.sock
default-character-set=utf8mb4
EOF
rm -f "$SRV"/data/mariadb/*.err 2>/dev/null || true

echo "==> [4/6] 调整 ARTEMiS 端口 777 -> ${HTTP_PORT}（避开特权端口）"
python3 - "$SRV/artemis/config/core.yaml" "$HTTP_PORT" <<'EOF'
import re, sys
p, port = sys.argv[1], sys.argv[2]
t = open(p, encoding='utf-8').read()
t2 = re.sub(r'(?m)^  port: 777$', f'  port: {port}', t)
open(p, 'w', encoding='utf-8').write(t2)
print(f"core.yaml: {t2.count(chr(32)*2 + 'port: ' + port)} 处端口已设为 {port}")
EOF

echo "==> [5/6] 创建 Python venv 并安装依赖（pylibmc 跳过，config 里 memcached 本就关闭）"
cd "$SRV"
PIP_INDEX="https://pypi.tuna.tsinghua.edu.cn/simple"
curl -sI --max-time 5 "$PIP_INDEX" >/dev/null 2>&1 || PIP_INDEX="https://pypi.org/simple"
echo "pip 源: $PIP_INDEX"
python3 -m venv venv-linux
./venv-linux/bin/pip install --index-url "$PIP_INDEX" --upgrade pip
grep -v -iE "pylibmc|^aiomysql|^pymysql" artemis/requirements.txt > /tmp/fgoa-req.txt
# 锁定 WSL 实测兼容的组合（aiomysql 0.2.x 与 PyMySQL 1.1+ 不兼容）
echo "aiomysql==0.3.2" >> /tmp/fgoa-req.txt
echo "PyMySQL==1.2.0" >> /tmp/fgoa-req.txt
if ! ./venv-linux/bin/pip install --index-url "$PIP_INDEX" -r /tmp/fgoa-req.txt msgpack; then
  echo "首次安装失败，尝试放宽 sqlalchemy 版本（SteamOS Python 较新）..."
  sed -i 's/^sqlalchemy==.*/sqlalchemy==1.4.54/' /tmp/fgoa-req.txt
  ./venv-linux/bin/pip install --index-url "$PIP_INDEX" -r /tmp/fgoa-req.txt msgpack
fi
./venv-linux/bin/python -c "import sqlalchemy, aiomysql, msgpack, starlette; from aiomysql.connection import Connection; print('依赖 OK')"

echo "==> [6/6] 配置虚拟服务器 IP 192.168.100.1（需要 sudo，只需一次）"
if ip addr show lo | grep -q "192.168.100.1"; then
  echo "192.168.100.1 已在 lo 上"
else
  sudo tee /etc/systemd/system/fgoa-net.service >/dev/null <<'EOF'
[Unit]
Description=FGOA local server IP (192.168.100.1) on loopback
After=network-pre.target

[Service]
Type=oneshot
ExecStart=/usr/sbin/ip addr add 192.168.100.1/32 dev lo
ExecStop=/usr/sbin/ip addr del 192.168.100.1/32 dev lo
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
EOF
  sudo systemctl daemon-reload
  sudo systemctl enable --now fgoa-net.service
fi
ip addr show lo | grep 192.168.100.1

echo
echo "===== 初始化完成 ====="
echo "脚本目录:    $SCRIPT_DIR（可整体移动/改名，脚本均自定位）"
echo "一键游玩:    bash $SCRIPT_DIR/fgoa-play.sh"
echo "桌面快捷方式: bash $SCRIPT_DIR/fgoa-desktop-install.sh"
