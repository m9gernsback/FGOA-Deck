#!/usr/bin/env bash
# FGOA Deck 服务器启动：MariaDB + ARTEMiS（ALL.Net/billing/AimeDB）
# 用法: bash fgoa-server-start.sh
set -euo pipefail

SRV="$HOME/.var/app/com.usebottles.bottles/data/bottles/bottles/FGOA/drive_c/FGOA-Server"
HTTP_PORT=8077   # 与 fgoa-deck-setup.sh 一致

# ARTEMiS 的 log_dir(../../logs) 解析到 drive_c/logs，需提前建好
mkdir -p "$SRV/logs" "$(dirname "$SRV")/logs"

wait_port() {  # host port timeout_s
  for _ in $(seq 1 $(($3 * 4))); do
    (exec 3<>"/dev/tcp/$1/$2") 2>/dev/null && return 0
    sleep 0.25
  done
  return 1
}

echo "==> 检查虚拟服务器 IP"
if ! ip addr show lo | grep -q "192.168.100.1"; then
  echo "192.168.100.1 不在 lo 上，尝试启动 fgoa-net.service（可能要 sudo 密码）"
  sudo systemctl start fgoa-net.service
fi

echo "==> 启动 MariaDB (8888)"
if ! (exec 3<>/dev/tcp/127.0.0.1/8888) 2>/dev/null; then
  cd "$SRV"
  nohup "$SRV/mariadb-linux/bin/mariadbd" --defaults-file="$SRV/mariadb-deck.ini" \
    >> "$SRV/logs/mariadb-stdout.log" 2>&1 &
  echo $! > "$SRV/state/mariadb.pid"
fi
wait_port 127.0.0.1 8888 20 || { echo "MariaDB 启动失败，见 $SRV/logs/mariadb.log"; exit 1; }

echo "==> 启动 ARTEMiS (${HTTP_PORT}/9999/7777)"
# 修复一（2026-09-20）：ARTEMiS index.py 的 ssl_version=3（ssl.PROTOCOL_TLSv1）在
# 新版 python（3.13）创建的是客户端用途 context，不能当服务器用 → 握手 internal error → RST
# （billing.log 零记录的根因）。改成 PROTOCOL_TLS_SERVER。幂等修补：
# 修复三（2026-09-20 晚）：游戏 billing 客户端只讲 TLS1.0（Windows 侧 PROTOCOL_TLSv1 只收
# TLS1.0 却一直正常 = 实证），而 python>=3.10 的 PROTOCOL_TLS_SERVER 默认 minimum_version=TLSv1.2，
# 且 OpenSSL 3 默认安全级 1 也直接禁用 TLS1.0/1.1（实测：清掉 min 后仍 UNSUPPORTED_PROTOCOL，
# 必须 @SECLEVEL=0）。OPENSSL_CONF 对 python 进程不生效（MinProtocol=TLSv1.3 反证实验：cnf 被无视），
# 所以包装 uvicorn.config.create_ssl_context 直接改 context：降 minimum_version + SECLEVEL=0。
# 注意：不能包装 ssl.SSLContext——ssl.py 的 minimum_version/verify_mode setter 用
# super(SSLContext, SSLContext) 硬引用模块全局名，替换全局名会让 uvicorn 后续
# verify_mode 赋值 TypeError（billing 崩 → FIRST_COMPLETED → 全服关闭 → 8077 未就绪）。
# uvicorn 在 wrapper 返回后还会 set_ciphers(billing 原串)，只换密码列表不动安全级，
# TLS1.0 套件 ECDHE-RSA-AES256-SHA 保留（本地 TLS1.0/1.1/1.2/1.3 四版本握手实测全通）。
python3 - "$SRV/artemis/index.py" <<'EOF'
import re
import sys
from pathlib import Path
p = Path(sys.argv[1])
t = p.read_text(encoding="utf-8")
changed = False
# 撤掉修复三的坏版本（ssl.SSLContext 包装），如果有
t2 = re.sub(r"_fgo_tls_orig_ctx = _ssl\.SSLContext\n.*?^_ssl\.SSLContext = _fgo_tls_ctx\n", "", t, flags=re.S | re.M)
if t2 != t:
    t = t2
    changed = True
if "ssl_version=3," in t:
    t = t.replace("import uvicorn", "import uvicorn\nimport ssl as _ssl", 1)
    t = t.replace("ssl_version=3,", "ssl_version=_ssl.PROTOCOL_TLS_SERVER,")
    changed = True
if "_fgo_tls_patch" not in t:
    t = t.replace(
        "import ssl as _ssl",
        "import ssl as _ssl\n"
        "import uvicorn.config as _uvcfg\n"
        "if hasattr(_uvcfg, 'create_ssl_context'):\n"
        "    _fgo_tls_orig_create = _uvcfg.create_ssl_context\n"
        "    def _fgo_tls_create_ssl_context(*a, **k):\n"
        "        ctx = _fgo_tls_orig_create(*a, **k)\n"
        "        ctx.minimum_version = _ssl.TLSVersion.MINIMUM_SUPPORTED\n"
        "        ctx.maximum_version = _ssl.TLSVersion.MAXIMUM_SUPPORTED\n"
        "        ctx.set_ciphers('ALL:@SECLEVEL=0')  # OpenSSL3 安全级>=1 直接禁 TLS1.0/1.1，必须降级\n"
        "        return ctx\n"
        "    _uvcfg.create_ssl_context = _fgo_tls_create_ssl_context  # _fgo_tls_patch",
        1,
    )
    changed = True
if changed:
    p.write_text(t, encoding="utf-8")
    print("[server] 已修补 billing TLS context（PROTOCOL_TLS_SERVER + TLS1.0 放行）")
EOF
# 日志降噪（2026-09-21）：ARTEMiS 出厂一片 loglevel: debug，单会话 fgo.log+aimedb.log 约 1MB
# 且追加式累积。启动前幂等压到 info；SERVER_LOGLEVEL=debug 可整体还原排查。
# 注意：ARTEMiS 数据扫描缓存进进程内存（文档 0.33），改配置必须重启服务器进程才生效。
python3 - "$SRV/artemis/config" "${SERVER_LOGLEVEL:-info}" <<'EOF'
import re, sys
from pathlib import Path
cfg, lvl = Path(sys.argv[1]), sys.argv[2]
for name in ("core.yaml", "fgo.yaml"):
    p = cfg / name
    if not p.exists():
        continue
    t = p.read_text(encoding="utf-8")
    t2 = re.sub(r'(loglevel:\s*)"?[a-z]+"?', lambda m: m.group(1) + lvl, t)
    if t2 != t:
        p.write_text(t2, encoding="utf-8")
        print(f"[server] {name}: loglevel 统一设为 {lvl}（已在运行的服务器需重启生效）")
EOF
# 修复二：openssl-legacy.cnf（MinProtocol=TLSv1 + SECLEVEL=0）。注：实测此 cnf 对 python
# 进程不生效（TLS1.3-only 反证），TLS1.0 放行靠上面修复三的 wrapper 直接改 context；
# cnf 保留仅供 openssl CLI 探针等外部工具使用（顶层 openssl_conf 指令必须有，否则整个文件被静默忽略）。
cat > "$SRV/openssl-legacy.cnf" <<'EOF'
openssl_conf = openssl_init

[openssl_init]
ssl_conf = ssl_sect

[ssl_sect]
system_default = system_default_sect

[system_default_sect]
MinProtocol = TLSv1
CipherString = ALL:@SECLEVEL=0
EOF
if ! (exec 3<>/dev/tcp/127.0.0.1/$HTTP_PORT) 2>/dev/null; then
  cd "$SRV/artemis"
  OPENSSL_CONF="$SRV/openssl-legacy.cnf" \
  nohup "$SRV/venv-linux/bin/python" index.py --config config \
    >> "$SRV/logs/artemis-stdout.log" 2>> "$SRV/logs/artemis-stderr.log" &
  echo $! > "$SRV/state/artemis.pid"
fi
for p in $HTTP_PORT 9999 7777; do
  wait_port 127.0.0.1 $p 30 || { echo "ARTEMiS 端口 $p 未就绪，见 $SRV/logs/artemis-stderr.log"; exit 1; }
done

HEALTH=$(curl -sk --max-time 3 "http://127.0.0.1:${HTTP_PORT}/" || true)
[ "$HEALTH" = "Service OK" ] || { echo "健康检查失败: $HEALTH"; exit 1; }
echo "===== 服务器就绪 (Service OK) ====="
