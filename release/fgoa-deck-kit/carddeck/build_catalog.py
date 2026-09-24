#!/usr/bin/env python3
"""build_catalog.py — 生成 carddeck/catalog.json（一次性，在 WSL 跑）

数据源（均在 FGOA 安装根下）：
  DEVICE/print/FGO11_AllServants/library-manifest.json  卡面清单（FileName/tcId/ServantId/CraftEssenceId/日文 DisplayName）
  App/zh/rom/mst_data/arms_mst_svt.farc                 中文从者名（zstd）
  App/zh/rom/mst_data/arms_mst_craft_essence.farc       中文礼装名（zstd）
  Server/data/fgo-master/svt/arms_mst_svt.bin           日文名回落（properties 文本）
  Server/data/fgo-master/craft_essence/arms_mst_craft_essence.bin

用法: python3 build_catalog.py [FGOA_ROOT]   默认 /mnt/e/Games/FGOA/FGOA
输出: 与脚本同目录的 catalog.json
"""
import io
import json
import re
import struct
import sys
from pathlib import Path

import zstandard

DEFAULT_ROOT = "/mnt/e/Games/FGOA/FGOA"
CARD_DIR_REL = "DEVICE/print/FGO11_AllServants"

# ---------- FARc 解析 ----------
# 头 0x20 字节；条目表从 0x20 起，条目 = name\0 + 4×u32BE(abs_offset, clen, ulen, flag)
# abs_offset 是绝对文件偏移；clen 含子头。子头长度不固定（20 字节或分块表），
# 其后是 1 个或多个拼接的 zstd 帧（magic 28 b5 2f fd）→ 直接定位首个帧再流式解压
ZSTD_MAGIC = b"\x28\xb5\x2f\xfd"

def farc_extract(path: Path, member: str) -> str:
    data = path.read_bytes()
    if data[:4] != b"FARc":
        raise ValueError(f"{path}: 不是 FARc 文件")
    pos = 0x20
    first_off = None
    entries = []
    while first_off is None or pos < first_off:
        end = data.index(b"\0", pos)
        name = data[pos:end].decode("ascii")
        off, clen, ulen, _flag = struct.unpack(">IIII", data[end + 1 : end + 17])
        if first_off is None:
            first_off = off
        entries.append((name, off, clen, ulen))
        pos = end + 17
    for name, off, clen, ulen in entries:
        if name == member:
            blob = data[off : off + clen]
            zpos = blob.find(ZSTD_MAGIC)
            if zpos < 0:
                raise ValueError(f"{path}:{member} 条目中无 zstd 帧")
            raw = zstandard.ZstdDecompressor().stream_reader(io.BytesIO(blob[zpos:])).read()
            if len(raw) != ulen:
                raise ValueError(f"{path}:{member} 解压长度 {len(raw)} != 头声明 {ulen}")
            return raw.decode("utf-8")
    raise KeyError(f"{path} 中找不到 {member}")

# ---------- properties 文本 → {id: name} ----------
def parse_prop_names(text: str, prefix: str, id_field: str) -> dict:
    rows = {}
    pat = re.compile(rf"^{re.escape(prefix)}\.(\d+)\.([^.=]+)=(.*)$")
    for line in text.splitlines():
        m = pat.match(line.strip())
        if m:
            rows.setdefault(int(m.group(1)), {})[m.group(2)] = m.group(3)
    out = {}
    for row in rows.values():
        if id_field in row and "name" in row:
            out[int(row[id_field])] = row["name"]
    return out

def main() -> None:
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(DEFAULT_ROOT)
    out_path = Path(__file__).resolve().parent / "catalog.json"
    card_dir = root / CARD_DIR_REL

    manifest = json.loads((card_dir / "library-manifest.json").read_text(encoding="utf-8"))
    cards = manifest["Cards"]

    # 中文名（zh farc），失败回落日文 bin
    zh_svt = parse_prop_names(
        farc_extract(root / "App/zh/rom/mst_data/arms_mst_svt.farc", "arms_mst_svt.bin"),
        "svt", "svt_id")
    zh_ce = parse_prop_names(
        farc_extract(root / "App/zh/rom/mst_data/arms_mst_craft_essence.farc", "arms_mst_craft_essence.bin"),
        "craft_essence", "id")
    ja_svt = parse_prop_names(
        (root / "Server/data/fgo-master/svt/arms_mst_svt.bin").read_text(encoding="utf-8"),
        "svt", "svt_id")
    ja_ce = parse_prop_names(
        (root / "Server/data/fgo-master/craft_essence/arms_mst_craft_essence.bin").read_text(encoding="utf-8"),
        "craft_essence", "id")
    print(f"[catalog] 名表: 中svt={len(zh_svt)} 中ce={len(zh_ce)} 日svt={len(ja_svt)} 日ce={len(ja_ce)}")

    file_re = re.compile(r"^(\d{5})_(SVT|CE)(\d{5})_(A\d{2})_(NORMAL|HOLO)\.bmp$")
    catalog = []
    skipped = []
    for c in cards:
        fn = c["FileName"]
        m = file_re.match(fn)
        if not m:
            skipped.append(fn)
            continue
        is_svt = m.group(2) == "SVT"
        ref_id = c["ServantId"] if is_svt else c["CraftEssenceId"]
        name_zh = (zh_svt if is_svt else zh_ce).get(ref_id)
        name_ja = (ja_svt if is_svt else ja_ce).get(ref_id)
        catalog.append({
            "file": fn,
            "tcId": c["TradingCardId"],
            "type": "svt" if is_svt else "ce",
            "refId": ref_id,
            "art": m.group(4),
            "finish": "holo" if m.group(5) == "HOLO" else "normal",
            "nameZh": name_zh or "",
            "nameJa": name_ja or c.get("DisplayName", ""),
        })

    # 对账：BMP 目录 vs manifest
    bmps = {p.name for p in card_dir.glob("*.bmp")}
    listed = {c["file"] for c in catalog}
    orphans = sorted(bmps - listed)
    missing = sorted(listed - bmps)
    if orphans:
        print(f"[catalog] 警告: {len(orphans)} 个 BMP 不在 manifest: {orphans[:5]}...")
    if missing:
        print(f"[catalog] 警告: {len(missing)} 个 manifest 条目无 BMP: {missing[:5]}...")
    if skipped:
        print(f"[catalog] 警告: {len(skipped)} 个条目文件名不合法已跳过: {skipped[:5]}...")

    catalog.sort(key=lambda c: (c["type"], c["refId"], c["art"], c["finish"]))
    out_path.write_text(json.dumps(catalog, ensure_ascii=False, indent=None), encoding="utf-8")
    zh_ok = sum(1 for c in catalog if c["nameZh"])
    print(f"[catalog] {out_path}: {len(catalog)} 张卡（中文名命中 {zh_ok}）")

if __name__ == "__main__":
    main()
