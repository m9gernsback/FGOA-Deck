# FGOA Steam Deck 移植 — 调试进度记录

> 最后更新：2026-09-23（**✅ Mesa 缓存崩溃已根治，Deck 复测通过，缓存默认随补丁开启**；新会话先读第 0 节 + 0.39 + 第 6 节）
> 前序文档：`WSL-Bottles-FGOA测试方案.md`、`SteamDeck-Bottles-FGOA部署方案.md`

## 0.39 第四十轮（2026-09-23 午后~晚，缓存崩溃根因二次修正 + libgallium 二进制补丁 + ✅ Deck 复测通过结案）

**对 0.37 根因的重要修正**：crash PC `libgallium+0x2f91d0`（`movdqu (%rax),%xmm0` + 24B 拷贝 + `cmpq $0,(%rsi); setne`）不是 serialize.cpp 写路径的 `->builtin` 解引用，而是 **`_mesa_program_get_resource_name`（shader_query.cpp:538）的 GL_UNIFORM 分支 `*out = RESOURCE_UNI(res)->name`**（gl_resource_name 恰 24 字节，逐字节吻合）。完整链：缓存命中恢复（`read_program_resource_data` 把 UniformRemapTable 里的 `INACTIVE_UNIFORM_EXPLICIT_LOCATION` 哨兵合法装回 `res->Data`，serialize.cpp:1006）→ `st_link_shader` 无条件调 `_mesa_create_program_resource_hash`（st_glsl_to_nir.cpp:811，**不在 ENABLE_SHADER_CACHE 门内**）→ 遍历资源取名字时解引用 -1。触发前提 = FGOA shader 大量 `layout(location=N)` 造成"活跃/未激活 uniform 显式 location 相撞"，remap 表该 location 条目为哨兵。佐证：fresh link 的资源列表只含活跃 uniform（gl_nir_linker.c:1125 传的是真 UniformStorage 指针），哨兵只能来自 deserialize → **与"MESA_SHADER_CACHE_DISABLE=true 永不崩"完全互洽**。环境变量分流路线已排除：`MESA_GLSL_CACHE_DISABLE` 在 25.3.0 已 deprecated 且等价于全禁（disk_cache_os.c:1022-1029），无独立开关。

**二进制补丁（路线 B，已完成并验证）**：`deck-deploy/mesa-binpatch.py` 改写 `_mesa_program_get_resource_name` 的 GL_UNIFORM 共享分支（vaddr 0x2f91cc，跳转表 idx 0/1/4/5/7-12/19 共用）——`mov 0x8(%rdi),%rax` 后插 `cmp $-1,%rax; je →return false`（复用函数内现成的 `xor eax,eax;ret` stub @0x2f9170），原 24B 拷贝重排进腾出的 36B 连续空间（含尾部 9B nop 填充），语义=两个调用方（哈希构建 2208/按名查找 715）本就以 `if(!get_resource_name(...))` 跳过无名资源。**全 .text 控制流扫描确认无跳入补丁区内部**；dlopen 直调 harness（`~/mesa-build/binpatch_test.c`）对照：原版哨兵输入 SEGV / 补丁版 rc=0 不崩、合法 uniform 拷贝逐字节正确、default 分支不变。产物：`deck-deploy/libgallium-25.3.0-patched.so`（md5 `5989ee30468a11a476ee68bfa48d90c6`，原版 `c1a3e616b4697cea9ee69a1c120dec9a`）。

**Deck 部署（待做）**：**重要修正——游戏加载的是宿主机 SteamOS 系统 Mesa（/usr/lib/libgallium-25.3.0.so），不是 flatpak runtime 的 Mesa**：fgo-launch-deck.sh 直跑 runner 的 wine 二进制、不经 `flatpak run` 沙箱（core 映射路径只有 /usr/lib/* 实锤；0.37/0.38 关于 flatpak GL 扩展升级的讨论建立在错误前提上，flatpak update 与本 bug 无关）。因此部署改为**用户态重定向，零系统改动**（SteamOS /usr 只读也不用碰）：`mesa-patch-install.sh` 校验系统 Mesa md5==基准后把补丁版装到 `~/Desktop/FGOA/mesa-patch/`（含 dri/radeonsi_dri.so、zink_dri.so 符号链接）；fgo-launch-deck.sh 检测到该目录即自动 `LD_LIBRARY_PATH` + `LIBGL_DRIVERS_PATH` 重定向（MESA_PATCH=0 可关）；还原 = `mesa-patch-revert.sh`（只删自己的目录）。**补丁生效验证已内置**：启动时自检补丁文件+系统基准双 md5 并打印；ago.exe 加载 GL 后后台 watcher 读 `/proc/<pid>/maps` 把 libgallium 真实加载路径写入 deck-inject-live.log（"运行时验证通过/失败"），开缓存但补丁未生效时启动即告警。对其他应用/游戏零影响（只影响本脚本拉起的进程）。**✅ Deck 复测通过（2026-09-23 晚）：补丁生效（watcher 验证 maps 加载路径），开缓存不再崩，第二次启动明显变快。缓存默认值已改为随补丁自适应：补丁在 → 默认 false（开缓存），补丁缺失/被关 → 默认 true（安全兜底），显式环境变量可覆盖。GameMode（Steam 添加非Steam 游戏）首启仍慢=该环境缓存冷启动（key 与桌面不同），第二次起同样快，属预期。**复测：`rm -rf ~/.cache/mesa_shader_cache*` → `MESA_SHADER_CACHE_DISABLE=false bash ~/Desktop/FGOA/fgoa-play.sh` 冷+暖两局，第二局应明显变快；游戏中 `grep libgallium /proc/$(pgrep -x ago.exe|head -1)/maps` 应见 mesa-patch 路径。仍崩则抓 core 回传。**SteamOS 系统更新换掉 Mesa 后补丁失效需重打**（install 脚本 md5 检查会拦住）。

**源码补丁（路线 A 素材，已备好）**：`deck-deploy/get-resource-name-sentinel-fix.patch`（主修复）+ `serialize-sentinel-hardening.patch`（写侧加固：新增 `uniform_inactive_explicit_location` enum 让哨兵在缓存里往返，防 glGetProgramBinary 打击恢复后的 program）。issue 草案 `mesa-issue-glsl-serialize-sentinel.md` 已按修正后根因重写。全量自编 Mesa（任务#3）尚未做——WSL 无 sudo 且 DNS 挂（仅 DoH+curl --resolve 可用），构建依赖（meson/ninja/LLVM）装不了，需要用户授权 sudo 或先修 DNS。

**遗留风险（低）**：`_mesa_program_resource_name`/`_mesa_program_resource_array_size` 对哨兵同样无防御，但只能经 GL program interface 查询 API 触达，FGOA 不走该路径（glGetUniformLocation 走 remap 表有哨兵检查）。**注意 harness 踩坑**：dlopen 返回值是 link_map* 不是基址，取基址要 `dlinfo(RTLD_DI_LINKMAP)->l_addr`。

## 0.38 会话小结（2026-09-22~23，WSL 侧离线分析专场，全部产出与状态）

**本轮没有改任何 Deck 运行配置**（MESA_SHADER_CACHE_DISABLE=true、radeonsi、glshim embedded-struct 改写、fgoglcompat、cngfix 双 dll、三符号链接照旧）。纯离线取证 + 研究。

### 已结案的新结论
1. **zink 时代 0xa76034 崩溃根因**（0.36）：zink `lower_bindless_instr`（zink_compiler.c:4371）对无 coord 的 txs（textureSize+bindless handle）读 `tex->src[-1]`。上游 main 未修。触发源 shader 237。
2. **开缓存崩 242 的根因**（0.37）：`serialize.cpp: write_program_resource_data` 对 GL_UNIFORM 资源无哨兵检查，撞上 `INACTIVE_UNIFORM_EXPLICIT_LOCATION`（=(void*)-1）。冷/暖缓存都触发（写路径冷也跑）。上游 main 未修。
3. **llvmpipe 25.2.8 回放实验**：142 shader + 100 link 冷/暖/混合全过 → 通用缓存路径无 bug，问题在链接期元数据序列化（driver 无关，glsl 共享层）。
4. **卡顿归因**：两层缓存——fgoglcompat 翻译缓存（shader-cache-r10，持久、复用、勿删）正常；Mesa 编译缓存被禁用是卡顿主因（每次启动 + 每会话新特效首次出现都重编译）。恢复 Mesa 缓存的前提是修 bug#2。

### 产出文件
- `mesa-issue-zink-lower-bindless.md` — 上游 issue 草案①（zink src[-1]）
- `mesa-issue-glsl-serialize-sentinel.md` — 上游 issue 草案②（serialize 哨兵解引用）
- `deck-deploy/shader-replay.c` — EGL surfaceless shader 回放器（用法：`EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 MESA_EXTENSION_OVERRIDE="+GL_ARB_bindless_texture" MESA_SHADER_CACHE_DIR=/tmp/xx ./replay <glshim dump目录>`；bindless 需 override，llvmpipe 专用）
- `deck-deploy/corescan.py` / `corefind.py` / `forensics.py` — core 取证三件套（线程/ucontext 扫描、SIGSEGV 定位、对象+栈取证）
- core 分析惯例：Hist 里已存 `libgallium-25.3.0.so`（Deck runtime 原版，偏移对照基准）

### Deck 侧现状（待办）
- **Mesa 仍 25.3.0**（flatpak 未更新；Deck 同时装有 GL 扩展 24.08 和 25.08 分支，Bottles 用哪个需 `flatpak info com.usebottles.bottles | grep Runtime` 确认）。flathub tip：24.08→26.1.8、25.08→26.2.2。
- **flatpak 升级复测价值降级**：两个 bug 上游 main 均未修，26.x 大概率照崩。仍可做（低成本），但预期管理：崩了抓 core 对照是否同一位置即可。
- 可选根治路径：打 serialize.cpp 补丁自编 Mesa 25.3.0 → 自制 flatpak GL 扩展注入 bottle → 恢复 Mesa 缓存 → 消除卡顿。
- 上游反馈渠道（0.30 遗留）现在有完整素材：Mesa ×2（上述两个 issue）、fgoglcompat 作者（embedded struct 改写需求）、wine/soda（bcrypt ECC）、ARTEMiS（ssl_version=3）。

### AMD_Other 补丁研究（留档）
`AMD_Other/a卡补丁v4.1` = GitHub 开源项目 **fluphus/fgo-arcade-amd-shim**（2026-09-16 london-fog-fix 快照）。与 fgoglcompat 不同作者不同路线（opengl32.dll 劫持 + 假地址 0x4647 模拟 bindless + 整篇替换 NV shader），带全源码，绑 Windows AMD 驱动 + 1080p，**不适合 Deck 直接换用**。价值：①伦敦地图白闪修复思路（compute dispatch 前从当前 UBO 刷新灯光 SSBO 指针绑定）可移植到 glshim ②tests/fixtures 有真实 shader 夹具 ③60FPS pacing 与 amdcfg 8192 批次上限与 fgoglcompat 独立收敛，交叉印证。仓库在活跃迭代（09-22 已出 pacing 修复新版）。

## 0. 一句话现状

**✅ 已结案（2026-09-21 午后）：Deck 成功进入标题画面。** 4102 根因 = soda-11.0-10 的 bcrypt 编译时缺 ECC secret 派生（`Compiled without ECC secret support`），amdipc ECK1 密钥协商在 `BCryptDeriveKey(HASH)` 必败 → Messenger 卡 state 2 → 推送被门死 → 对称死锁。修复 = ipcdump v7 内置 cngfix（钩 SecretAgreement/DeriveKey 合成派生密钥），Deck 部署 v7 双 dll 后一局进标题。**注意：cngfix 是永久必需品——Deck 上的 `drive_c/FGOA/ipcdump.dll` 和 `drive_c/FGOA/App/am/wlanapi.dll` 不能删**（删了就回到 4102）。

## 0.37 第三十九轮（2026-09-23，冷缓存崩溃 core 取证：bug#2 根因实锤 = serialize.cpp 程序元数据序列化对 INACTIVE_UNIFORM_EXPLICIT_LOCATION 哨兵（Data=-1）无检查解引用）

素材：deck-logs/20260923-102800/（真·冷缓存，mesa_shader_cache 清空后第一局即崩，崩后缓存目录零写入；core.ago.8263 已解压分析）。**Deck Mesa 仍为 25.3.0（flatpak 未更新；GL.txt 显示 Deck 同时装有 24.08/25.08 两套 runtime 分支）。**

- **死亡点不变**：linklog 止于 program 242 status query（主线程等待）；真实崩溃在 **Mesa util_queue 工作线程**（栈底 trampoline = job 调度器，与 zink 时代同款签名）。
- **崩溃指令**：libgallium+0x2f91d0 `movdqu (%rax),%xmm0`，RAX=CR2=0xffffffffffffffff —— 解引用 **-1 指针**。
- **调用链**（自底向上）：util_queue 线程 → job → link 层（st_link_shader/st_glsl_to_nir.cpp 区域，含 "linking with uncompiled/unspecialized shader"/"GLSL shader program %d info log:" 字符串实锤）→ 0x2fb7d0（遍历 program 资源，gl_program_resource = {GLenum16 Type; void *Data; u8 StageRef} 24 字节）→ helper 读 res->Data 崩。
- **崩溃对象**：ProgramResourceList 首条目 Type=0x92e1（GL_UNIFORM）、**Data=(void*)-1**；后续兄弟条目 Data 均为有效指针。
- **-1 的身份实锤**：`shader_types.h:51` —— `#define INACTIVE_UNIFORM_EXPLICIT_LOCATION ((struct gl_uniform_storage *) -1)`，Mesa 对"显式 location 但未激活 uniform"的合法哨兵。
- **代码路径**：缓存启用 → 链接成功后 `shader_cache_write_program_metadata`（st_glsl_to_nir.cpp:831，`#ifdef ENABLE_SHADER_CACHE` 门内）→ `serialize_glsl_program` → `write_program_resource_list` → `write_program_resource_data` 的 GL_UNIFORM 分支直接 `((gl_uniform_storage *)res->Data)->builtin/->name.string`，**不查哨兵**（serialize.cpp:900-908）。remap 表写路径（serialize.cpp:588/614）有哨兵检查，资源列表写路径没有。
- **上游 main（2026-09-23）仍未修**；且 deserialize 读路径（serialize.cpp:1006 `res->Data = util_range_remap(...)->ptr`）会把 remap 表里的哨兵合法地装回 res->Data → 暖缓存轮次也有同款风险（0.14/0.34 的 242 死因大概率就是它，与本次冷启动同一处）。
- **冷启动也崩的成因**：首轮即触发说明写路径本身就会在某种 uniform 形态下产生/遇到哨兵条目（显式 location + inactive 的 uniform，FGOA shader 大量 layout(location=N) 声明）。
- **结论**：MESA_SHADER_CACHE_DISABLE=true 仍是正确 workaround（元数据写路径整体跳过）；**flatpak 升级到 26.2.2 大概率也修不了**（main 未修），复测价值降级但不排除周边改动改变触发条件。
- **修复方案**：上游补丁 = write_program_resource_data 对 GL_UNIFORM 分支加 `res->Data == INACTIVE_UNIFORM_EXPLICIT_LOCATION` 检查（写 remap_type_inactive_explicit_location 或直接跳过，参照 588/614 的既有处理）。本地如需缓存加速，可用该补丁自编 Mesa 做 flatpak GL 扩展。

## 0.36 第三十八轮（2026-09-22，WSL 侧离线取证：zink 0xa76034 崩溃根因实锤 = lower_bindless_instr 对无 coord 的 txs 越界读 src[-1]；暖缓存 242 崩溃是另一个 bug，待 Deck 取 core）

**本段全部在 Windows/WSL 侧离线完成**，素材 = Hist/coredump/core.ago.14391 + Hist/libgallium-25.3.0.so + 20260921-142225 轮 glshim dump。

- **崩溃函数实锤 = zink `lower_bindless_instr`**（mesa-25.3.0 `src/gallium/drivers/zink/zink_compiler.c:4371`）。证据链：
  - 崩溃对象按 `+0x18` 类型字节 3/4 分派 = nir_instr_type tex(3)/intrinsic(4)（nir_instr 布局：node 0x10 + block 指针 → type 恰在 +0x18）
  - intrinsic 走 14 项跳转表（0x33-0x40），8 活 6 空 → 字母序 nir_intrinsic_op 的 bindless_image_* 族（OP_SWAP 8 个）✓
  - 崩溃对象（ucontext RBX）内存：+0x18=3(tex)、sampler_dim=1(2D)、dest_type=0x22(int32)、**op@+0x28=8=nir_texop_txs（textureSize）**、src 数组 3 项 src_type={0x10 texture_handle, 0x11 sampler_handle, 0x5 lod}、**无 coord**
  - 崩溃指令 `mov 0x18(%rax),%r9` 中 rax = src数组 + **0x27ffffffd8**（编译器把 `-1*0x28` 折叠成的常数，core 里指令流有 `movabs $0x27ffffffd8,%rcx` 实证）
- **根因代码**（zink_compiler.c:4404-4406，Warhammer 40k 补丁引入）：`unsigned c = nir_tex_instr_src_index(tex, nir_tex_src_coord); nir_src_num_components(tex->src[c].src);` —— **txs（textureSize）没有 coord src，src_index 返回 -1，转 unsigned 后 tex->src[-1] 野指针读 → SIGSEGV**。**上游 main（2026-09-22）仍未修**（同代码在 main:4640）。
- **触发源 = shader 237**（`shader_237_q0.glsl:94`：`textureSize(sampler2D(fgo_handle_bits(...)), 0)` —— bindless 句柄构造采样器 + textureSize，VS 阶段，全场唯一）。与 0.13 stub 实验完全互洽：stub 掉 237/238 即越过 239。
- **为何外国线程无声死亡**：zink 在工作线程（util_queue cache_get_thread 的 optimized_compile_job）里跑 shader 编译 → lower_bindless 崩 → wine 对外国线程 NULL TEB 双重 fault（0.11）。
- **重要分案**：此 bug 是 zink 专属、冷缓存也触发（第 11-13 轮死 239）。**0.14/0.34 的"暖缓存死 program 242"是另一个 bug**（radeonsi 无 lower_bindless 照样死）——仍待定位，需要在 Deck 上暖缓存复现时抓 core 做同款取证。
- **对当前 radeonsi 方案无影响**（zink 已退役），但值得报上游 Mesa：最小复现 = GLSL 里对 bindless sampler 调 textureSize。
- **新工具**（已入 deck-deploy/）：`shader-replay.c`（EGL surfaceless 回放 glshim dump，冷/暖缓存对照）、`corescan.py`（coredump 线程/ucontext 扫描）、`forensics.py`（崩溃现场对象+栈取证）。llvmpipe 25.2.8 上回放 142 shader + 100 link 全过（冷、暖、混合三种），证明缓存命中通用路径无 bug，bug#2 在驱动专属缓存路径。
- **Deck 侧下一步**：①`flatpak update`（GL 扩展 tip 已是 Mesa 26.2.2）→ `MESA_SHADER_CACHE_DISABLE=false` 复测 ②仍崩则抓 core（coredumpctl）+ 回传，用同款取证定位 bug#2 ③zink lower_bindless 上游报告素材已齐。

## 0.35 第三十七轮（2026-09-21 晚，✅ 已结案：刷卡能读礼装但从者卡不显示——call_up 硬依赖 catalog，反斜杠 CardsPath 在 Linux 必败）

- **现象**：刷卡成功音、validator accepted=5，礼装（CE）正常显示，3 张从者卡不显示
- **根因链**：carddeck 每次保存把 deck.json `CardsPath` 写成反斜杠 `..\DEVICE\...` → Linux 服务端 `_load_card_catalog()` 的 `normpath(join(app_root, CardsPath))` 不识别反斜杠 → catalog 恒空 → `call_up` 中从者卡必须命中 `installed_servant_cards`（catalog），空表 → tc_id 全部进 `invalid_tid_list` → 客户端当不可读卡不显示；CE 卡走 trc 主表（TrcTypeId≠1）不查 catalog，所以幸存
- **纠正 0.33 误判**：该"遗留"并非"仅影响打印卡资格"，它直接打断 call_up 从者显示
- **修复（方案 2，不动服务端代码）**：① deck.json `CardsPath` 改正斜杠 `../DEVICE/print/FGO11_AllServants`（`SelectedCards` 保持反斜杠——客户端 DLL 按 `\` 解析）；② Deck 补符号链接 `drive_c/DEVICE -> FGOA/DEVICE`（normpath 词法折叠 `App/..` 不经过 App 链接，必须单独补，已固化进 fgoa-deck-setup.sh）；③ carddeck/server.py 拆 `CARDS_PATH_POSIX`（写 CardsPath）/ `CARDS_PATH_WIN`（写 SelectedCards）防回退
- **注意**：`_load_card_catalog` 无缓存、每请求重读，改 deck.json 后无需重启 ARTEMiS 即生效

## 0.34 第三十六轮（2026-09-21 午后，✅ 已结案：radeonsi 开 Mesa 磁盘缓存同样闪退——缓存 bug 与后端无关，永久禁用）

- **实验**：`MESA_SHADER_CACHE_DISABLE=false`（缓存开启）+ radeonsi 跑两局 → 第二局（温缓存）闪退
- **证据**：linklog 止于 `program 242 status query enter`（死在 Mesa 内部 glGetProgramiv/编译线程），与 0.21 zink 时代的死亡点完全一致；exit-trace 两局均异常终止
- **修正旧归因**：0.21 记为"zink 特有 bug"是写窄了——崩溃点在 **libgallium 的 NIR 磁盘缓存命中路径，zink 与 radeonsi 共享该代码**，两个后端都中。上游反馈素材的归因描述应相应修正
- **固化**：`MESA_SHADER_CACHE_DISABLE` 默认值改回 `true`（已改成可用环境变量覆盖的形式，便于将来复测）；启动慢是永久代价

## 0.33 第三十五轮（2026-09-21 午后，✅ 已结案：抽卡/战斗 Error = 服务器缓存是建链接前的空数据，重启即修复）

- **现象**（与 0.32 同一局，13:10 进游戏）：抽卡报错、进战斗报错
- **日志**：`summon` → cmd_result=2（"Card 6 is not an eligible summon card"）；`get_singularity_quest_progress(9001)` ×3 → cmd_result=2（quest 列表空，index.py:21308 故意拒绝）；**failures.jsonl 无新增**
- **根因（同一个）**：ARTEMiS 服务器进程自 11:37 起未重启，11:38 game_init（符号链接建立之前）把所有数据扫描结果**缓存为空**：quest 索引、`_trading_card_master_cache`、open settings。13:10 的 game_init 零索引日志 = 缓存命中没重扫。start 能过只因玛修阈值是懒加载、11:38 崩溃时未写缓存
- **修复**：重启 ARTEMiS（fgoa-server-stop/start）让缓存重建。Windows 侧已验证数据齐全（9001 有 21 quest；trc 表含 Card 6）。**经验：改数据/链接后必须重启服务器**。**验证通过（同日）：重启后抽卡正常、战斗可进入**
- **遗留（不阻塞）**：deck.json 的 `CardsPath` 是反斜杠 Windows 路径（`..\DEVICE\print\FGO11_AllServants`），Linux path.join 不解析 → 打印卡目录加载失败 WARNING。仅影响打印卡资格；根治 = 改 Deck 的 deck.json 为正斜杠相对路径，或服务器两处 catalog loader 加反斜杠归一化

## 0.32 第三十四轮（2026-09-21 午后，✅ 已结案：角色全黑 = 237/238 stub 遗产，换 radeonsi 原生后端根治）

- **现象**：进大厅后角色渲染纯黑无轮廓。非网络/服务器问题，GL 层
- **根因**：`GLSHIM_STUB_IDS=237,238` 仍是默认——237/238 是角色材质着色器，stub 换成直通桩 → 角色黑。stub 是 zink 时代为绕过"shader 237 VS bindless textureSize 触发 zink 编译线程崩溃"（0.21 实锤）的代价
- **修复**：`GL_BACKEND=native`（radeonsi 原生，自带 ARB bindless；NV 扩展字符串 override 是 Mesa 通用层不依赖 zink）+ `GLSHIM_STUB_IDS=""` 去 stub → **角色正常渲染，一局验证通过**
- **固化**：`fgo-launch-deck.sh` 默认值改为 `GL_BACKEND=native`、`GLSHIM_STUB_IDS=""`；glshim 保留（embedded-struct 改写仍是必需品）；`MESA_SHADER_CACHE_DISABLE=true` 暂保留（zink 特有问题，radeonsi 下可试 re-enable 加速启动，未验证）
- **部署动作**：deck-deploy/fgo-launch-deck.sh 需同步覆盖 Deck 的 `~/Desktop/FGOA/`
- **zink 整条路线退役**（仅留对照）；上游反馈待办里的 Mesa/zink 崩溃素材仍有效（对上游是真实 bug），但本机不再依赖

## 0.31 第三十三轮（2026-09-21 午后，✅ 已结案：刷卡登录修复——Deck 数据目录层级差一层，符号链接解决）

- **现象**：标题界面刷卡 → 日语通用通信错误框。日志链验证：aimedb `lookup_ex` 正常（卡 …8549 → user_id 1，发 token）→ `pre_start` 正常应答 → **`start` 只有一行 card-catalog WARNING，无 `Offline response for start`** → 客户端 4s 后 `pd_unlock`（= start 失败后的解锁动作，与 `titles/fgo/index.py:21057` 注释吻合）→ 弹错误框。再刷一次同样死在 start
- **关键证据 = `failures.jsonl`**（capture 目录，默认 `logs/fgo_capture`；处理器异常被捕获得只有 `error_type`/`error`，无 traceback）：两次 start 均 `StopIteration`，空消息
- **根因**：`_mash_story_progress`（`index.py:2031`）裸 `next()` 在 `Server/data/fgo-master/svt/arms_mst_svt.bin` 里找玛修行；`_load_property_rows` 对缺文件静默返回 `[]` → StopIteration。调用链：start → `_build_player_servant_inventory`(13217) → `_merge_flavor_unlocks_into_inventory`(13303) → `_merge_mash_story_into_inventory`(1946) → `_mash_story_progress`(2066)。**Deck 未部署 `drive_c/Server/data/fgo-master/`**
- **背景**：Deck 侧服务器数据大面积缺失（game_init 警告：fgo-master quest 文件、`App/rom/{sprite,multi,single,aet}`、`App/deck.json` 全缺，open settings 索引 0/0/0）；别处有容错只降级，start 链上没有
- **修复**：Windows 侧 `FGOA/Server/data/fgo-master/`（128MB）+ `FGOA/App/deck.json` 拷入 Deck bottle `drive_c/` 对应路径；`App/rom/`（24GB）暂不拷（rom 缺失服务器只 WARNING 不崩）。若还有下一处裸 next()，failures.jsonl 会直接指认
- **✅ 已修复并验证（2026-09-21 午后）**：不是没数据，是**目录层级差一层**。`app_root` 写死为「artemis 包上溯 4 层 + `App`」（`index.py:252`）；Deck 上 artemis 在 `drive_c/FGOA-server/` → 解析出 `drive_c/App` + `drive_c/Server/data/fgo-master`，而数据实际在 `drive_c/FGOA/App` + `drive_c/FGOA/Server/...`。Windows 侧 artemis 在 `FGOA/Server/` 里所以恰好对齐。**修复 = 符号链接**：`drive_c/App -> FGOA/App`、`drive_c/Server/data/fgo-master -> ../../FGOA/Server/data/fgo-master`（rom 24GB 顺带可见，WARNING 全消；注意大小写，`server`≠`Server`）。**建链后刷卡正常进入游戏，结案**。这两个链接与 cngfix 双 dll 一样属于 Deck 部署的**永久组成部分**，重建 bottle 时需恢复

## 0.30 第三十二轮（2026-09-21 午后，终验通过：标题画面达成）

- Deck 覆盖 v7 双 dll 后正常启动，**成功进入标题界面**，4102 消失。cngfix 一局生效
- **部署状态变更（重要）**：ipcdump.dll / wlanapi.dll 从"诊断工具，结案后删"变为**运行必需品**（cngfix 是 amdipc 密钥协商的唯一修复）。`IPCDUMP_CNGFIX=0` 只是调试开关，日常勿用
- 收尾待办（不阻塞游玩）：
  1. launch 脚本 `WINEDEBUG` 默认值改回 `-all`（0.18 起是 `err+all`）
  2. 性能验证：长时间游玩/刷卡/结算全流程跑一遍（billing/aime 已通，理论无碍）
  3. 上游反馈（素材全齐）：①wine/tkg：soda 构建缺 ECC secret 派生（附 cngsmoke.c 复现）②Mesa：zink 磁盘缓存路径崩溃 + wine 外国线程 NULL TEB 双重 fault ③fgoglcompat 作者：embedded struct 改写需求 ④ARTEMiS：ssl_version=3 在新 python 下失效
  4. 遗留未排查：zh/fgozh.dll 在 Wine 下 DllMain 返回 FALSE（`FGO_ZH_DLL=1` 加载，默认不带）
  5. Windows 侧已全部恢复原样（amdaemon.exe 原版已回、zlanapi.dll 已删、对照日志存档 deck-logs/Hist/）

## 0.29 第三十一轮（2026-09-21 中午，根因结案：wine bcrypt 缺 ECC secret 派生 → amdipc 密钥协商死 → Messenger 卡 state 2）

**Windows amdaemon 对照 transcript 到手**（zlanapi 补丁 exe + v6 shim，存档 `deck-logs/Hist/ipc-dump_PC_v6_amdaemon_control.log`）：
- **两次 h=NULL WFSO 在 Windows 上一字不差地存在**（89143312 ×2）——烟雾弹实锤，从嫌疑名单永久划掉
- **推送由握手线程自己发起**（tid 33952 = Deck 的 tid 424 等价物）：握手应答 +31ms 后纯用户态组包 protect(16B{id=2,len=44})→WriteFileEx 64B → id=3 118960B……无任何跨线程触发、无任何新句柄——推送条件在进程内部
- Windows 顺序：握手→+31ms 推→属性采集/子系统初始化与推送交织；Deck 顺序：握手→属性采集→备份 shm→沉寂。推送不等子系统就绪

**静态逆向（amdaemon.exe 本地，RTTI+demangled 签名金矿）**，完整状态机：
- `Messenger::Impl::putMention` (0x1400706a0)：**`cmp dword [this+0x20], 3; jne return 0`** —— state≠3 时推送被静默丢弃
- setState (0x14006f990)：state==2 时自动调 `writeKey`（发握手应答）；迁移回调 `[this+0xb0]->vtable[0x10](new,old)`
- pump (0x14006fa70)：state 1→2（触发 writeKey）；**state 2 + `[this+0x28]==2`（key 写完）+ `0x140075df0(ctx)` 为真 → setState(3)**
- `0x140075880`（KEY 消息体处理）：导入客户端 3 个 ECK1 公钥（`fromKeyPairBinary` "ECCPUBLICBLOB"）→ 对每个做 `makeSecretAgreementKey`（**ECDH_P256 + BCryptSecretAgreement + BCryptDeriveKey("HASH", KDF_HASH_ALGORITHM="SHA256")**）→ 全成功才置 `[ctx+0x240]=1`（= cryptoReady）
- 结论：**推送门槛 = state 3 = ECK1 密钥协商全部成功**

**根确实验（`deck-deploy/cngsmoke.c`，本地 soda-11.0-10）**：
- ECDH_P256 密钥对生成/ECK1 导出/导入/SecretAgreement 全 OK；**`BCryptDeriveKey("HASH")` 必返 0xC0000002**，`+bcrypt` trace 实锤：`warn:bcrypt:key_asymmetric_derive_key Compiled without ECC secret support.`（soda 构建时 gnutls 缺 `gnutls_privkey_derive_secret`，unix 侧 compat 桩；上游 wine-11.0 是 LOAD_FUNCPTR_OPT 运行时加载，无此编译守卫——疑似 soda/tkg 构建环境 gnutls 太旧）
- 缺陷范围仅此而已：AES-CBC 回环、SHA256 向量、公钥导出（私钥句柄/导入句柄均可）全 OK
- ago.exe 与 amdaemon.exe 静态导入同一套 bcrypt ECDH API（同一 amdipc 库），两侧同样死

**修复 = ipcdump v7 内置 cngfix（`deck-deploy/cngfix.h`）**：IAT 钩 `BCryptSecretAgreement`（成功后导出双方 72B ECK1 公钥，字典序拼接存档）+ `BCryptDeriveKey`（先走真实现，失败且句柄在册则合成 `SHA256(sort(pubA,pubB))`）+ `BCryptDestroySecret`（注销）。先走真实现 = wine 哪天修好自动回归原生。**自钩自测（cngsmoke -DCNGFIX_SELFTEST）在 soda 下双端派生 32B 完全一致**（7e0e4857… 匹配），AES 后续路径不受影响。`IPCDUMP_CNGFIX=0` 可关。
- 构建不变：ago 侧 `-DIPCDUMP_STANDALONE`，amdaemon 侧 `wlanapi.c + ipcdump.c` 不带宏
- **Deck 部署 = 覆盖两个 dll 后正常启动**：`ipcdump.dll`(v7,241398B)→`drive_c/FGOA/`；`wlanapi.dll`(v7,242821B)→`drive_c/FGOA/App/am/`
- 预期：ipc-dump.log 出现 `cngfix: secret ... recorded` + `cngfix: DeriveKey(HASH) synthesized`，随后 amdaemon pipeWriteEx 推送序列（id=2/3/…）、ago 回写 #02、fgo.log 出 `game_init`——4102 应消失
- 注意：合成密钥与真 Windows 不一致（语义=本地 IPC 混淆层，两端同 wine 同补丁自洽即可；不影响 ARTEMiS 服务器）
- Windows 侧已恢复：App\am\amdaemon.exe 回原版（md5 4a306e9b…）、zlanapi.dll 已删

**遗留可选**：换 runner 实验（上游 wine-11.0 运行时加载 gnutls_privkey_derive_secret，理论不受影响；但换 runner 有 GL/注入回归风险，cngfix 已是决定解）。向 wine/tkg 上报该构建缺陷（附 cngsmoke 复现）。

## 0.28 第三十轮（2026-09-21 上午，Deck v6 回传分析：NULL 来源不在钩子覆盖面 + 对照组思路转向）

**v6 采集（2026-09-21 10:14 局，ago=260 27/31 钩、amdaemon=420 29/31 钩）判读结果**：
- **两次 h=NULL WFSO 定位**：tid 424 在 ts 209804/209814（握手完成后 +126/+136ms），嵌在 `SystemSegaSystemPropertywirelessNetworkmain_nic` 属性段内。模式相同：`CreateEventW(mr=1)` → wireless 属性互斥操作 → **WFSO(NULL, INFINITE) 立即返回 0xffffffff（非阻塞）** → 连建 5 个无名事件 → WFSO(前一轮句柄值) → 新 worker tid（476/480）在新建事件上醒来。判读 = 按子系统起 worker 池的初始化循环，NULL 是那个**从未被创建的句柄**（头号嫌疑：shim 返回空无线接口列表 → wlan 监视线程未启动 → 其句柄保持 NULL）
- **v6 新钩全部空军**：amdaemon 全程**零 CreateThread / 零 _beginthreadex / 零 OpenProcess** 调用（钩子本身成功：amdaemon.exe 静态导入 _beginthreadex+OpenProcess 确认在表内；worker 线程 476/480/484/488 实际存在 = 走的是未覆盖路径，DLL 自有 IAT 或 RtlCreateUserThread）。ago 侧的 _beginthreadex 记录全是 +30s 后的着色器线程池，与案情无关
- **关键降级**：0.27 的"Deck 独有异常"表述有误——Windows 对照只插桩了 ago（amdaemon 因 KnownDLLs 从未被观测），**Windows 的 amdaemon 可能同样有这两次 NULL 等待**（无线段噪音）。NULL 等待从"关键线索"降级为"未证实"
- amdaemon 完整时间线与 v5 一致：建 4 管 → ago +28ms 连上 → 握手 209677-678 → BS0 轮询 → 属性采集 209767-209818 → 210099 建 daemon_backup + 写空 272B 块 → 沉寂（tid424 alertable 轮询、476/480 WFMO(3)、每分钟 DBMUTEX 内务线程群 848-876 与 aime play log 失败节奏吻合、tid488 NFC 刷屏 9.5 万行）。ago 侧不变（挂读 SC0_SC）。推送始终未发生
- amdaemon.exe.log 零初始化报错（只有 Windows 时代就有的 aime play log 失败）

**RTTI 情报**（amdaemon.exe 本地静态）：`amdaemon::ipc::Messenger::Impl` 状态机，`std::function<void(State,State)>` 迁移 lambda 表；源码 `libs/amdipc/src/Messenger.cpp / NamedPipe.cpp / Backup.cpp / message/{Reader,Writer,Crypto}.cpp`

**决定性对照工具 `deck-deploy/win-amdaemon-kit/`（绕过 KnownDLLs）**：
- `amdaemon.exe`：导入表 DLL 名 `wlanapi.dll`→`zlanapi.dll`（0x806d4a 单字节 w→z，无 Authenticode 签名，md5 d572dd0ad796abddc9862516b1bd41e2）。zlanapi 不在 KnownDLLs → 真 Windows 下应用目录 shadow 生效
- `zlanapi.dll`：v6 shim 原样改名（8 个导出与 amdaemon 导入一一对应已验证）
- **Windows 操作**：备份 `App\am\amdaemon.exe` → 放入补丁版（文件名保持 amdaemon.exe）+ zlanapi.dll → 正常跑一局到标题退出 → 回传 **`App\am\ipc-dump.log`**（C:\FGOA 不存在时落到主模块同目录）→ 恢复原版 exe、删 zlanapi.dll
- 风险：仅 ago.exe 有 fgohook 时间戳校验，amdaemon.exe 无已知完整性检查； shim 无线桩返回空列表对 IPC 观测无影响
- 判读目标：①健康局推送由哪个 tid 发起、第一条 16+48B 之前最近的调用序列（推送触发条件现形）②Windows amdaemon 有没有同样的两次 NULL WFSO（证实/证伪烟雾弹）③握手后状态采集→推送的完整调用链

**若 Windows transcript 仍不够**：静态逆 Messenger.cpp 状态机（RTTI 符号齐全），找推送触发的 State 迁移条件，再决定下一版钩子（候选：NtCreateThreadEx/RtlCreateUserThread/CreateSemaphoreA——amdaemon 导入但未钩的可等待句柄 API）

## 0.27 第二十九轮（2026-09-20 深夜第七轮，Windows 对照组 ago transcript 到手：案情收敛为单问句）

**Windows 健康局 v5 ago 侧（pid 18004，完整跑到标题退出）回答全部拓扑问题**：
- **BS0/BC0 是烟雾弹**：Windows 健康局里两对管道也只建不用（ago 的 BC0 accept 从未完成，amdaemon 也没连）——所有流量都走 SC0 对。Deck 上 BS0/BC0 pending 属正常，从嫌疑名单划掉
- **谁先写 = amdaemon 先写**：握手完成 +32ms amdaemon 主动推 `16+48B`，+63ms 推 `16+118944B`（118KB 状态包），ago 收到后才回写首个 6320B；随后 amdaemon 持续推 6896/42096/20528/112688B 等块，ago 周期性回写 6320/640/192B——推送是 amdaemon 自驱动的，ago 只挂读
- ago +657ms 打开 daemon_backup shm（err=0 已存在）——Deck 上 ago 从未走到这步，因为它在等第一条推送
- Windows ago 全程零 `h=NULL` 等待；**Deck 独有的异常 = amdaemon 握手后 +116ms 两次 `WaitForSingleObject(h=NULL, INFINITE) -> 0xffffffff`**（v3/v4/v5 每轮都有）——嫌疑 = 某个线程/进程/事件句柄在 wine 下创建失败为 NULL，amdaemon 的推送状态机因此没启动
- Windows amdaemon 侧没插桩成功：**wlanapi.dll 在真 Windows 的 KnownDLLs 列表里，应用目录 shadow 被绕过**（wine 无此条目所以 Deck 上好使）。PC amdaemon transcript 暂不必须

**案情单问句**：Deck 的 amdaemon 为什么不在握手后 +32ms 推送？——追杀 NULL 句柄来源。

**ipcdump v6（deck-deploy 已更新，冒烟过）**：加钩 CreateThread（start addr/返回句柄/tid/err）、_beginthreadex（msvcrt/ucrtbase/api-ms-win-crt-runtime 三名尝试）、OpenProcess（pid/acc/返回值/err）；CreateEventW 改记无名事件（每进程防洪 100 条）。构建命令同 0.25。

**下一轮（Deck，同前）**：两个 v6 dll 分别覆盖 `drive_c/FGOA/ipcdump.dll` + `drive_c/FGOA/App/am/wlanapi.dll`，正常启动一局，回传 `logs/ipc-dump.log`；顺手回传 `GameData/SDEJ/amdaemon.exe.log`（amdaemon 自己的日志，看它的初始化有没有报错）。判读：①h=NULL 的 WFSO 前面最近一个 CreateThread/OpenProcess/CreateEvent 是谁、返回什么 ②若全是成功的 → NULL 来自更深处（再考虑钩 NtCreateThreadEx 或直接逆向 Messenger.cpp 的推送触发条件）

**Windows 侧已于 0.27 后恢复原状**（App\ipcdump.dll、App\am\wlanapi.dll、App\ipc-dump.log 已删；对照 transcript 存档 `deck-logs/Hist/ipc-dump_PC_v5_control.log`）

## 0.26 第二十八轮（2026-09-20 深夜第六轮，双进程 transcript 齐了：对称死锁实锤）

**Deck v5 双进程（ago=256 / amdaemon=420）完整协议拓扑与死锁现场**：
- 拓扑：amdaemon 服务 SC0+BS0 两对管道；**ago 服务 BC0 对**（ago 也 CreateNamedPipeW，此前"ago 建 BC0"的 +file 印象证实）
- ago 全程：`WaitNamedPipeW(SC0, timeout=0)` 轮询等 amdaemon 建管（err=2 直到建成）→ CreateFileW 连 SC0 对（CS=写、SC=读）→ pipeWriteEx 256B 注册 + 读回应答 → **挂 `pipeReadEx(SC0_amdmsg_SC, req=16)` 等 amdaemon 推送** → IPC 线程沉默 165s → 4102 拆台
- amdaemon 全程：建 4 管 + 4 accept pending → ago 连上 SC0 → 握手应答 → **挂 `pipeReadEx(SC0_amdmsg_CS, req=16)` 等 ago 的 #02** → 168.5s 后 err=109
- **对称死锁：双方都在 SC0 对上挂读等对方先写**。Windows 健康局（v3 ago transcript）显示 amdaemon 在握手应答后 +31ms 内会主动推 2 条 16B——Deck 上这个推送从未发生，它是打破对称的钥匙
- 交叉连接双双缺失：ago 从不碰 BS0（accept 永远 pending），amdaemon 从不碰 BC0（ago 的 accept 也永远 pending）——推送/交叉连接的状态机在 Deck 上整体没启动
- 排除项：daemon_table shm 内容传播正常（ago 标 client=01 → amdaemon 看到并标 server=01 → 稳态 01/01，与 PC 相同）；**插曲**：amdaemon 本轮 OpenMutex ago 建的 `amdipc_shmmtx_daemon_table_150805` 得 err=2（ago 建完就 CloseHandle——句柄值复用实锤——wineserver 把对象删了），amdaemon 自建了一个同名但不同的互斥锁；但因 shm 内容照常互通，暂列非致命疑点（PC 对照可验证该锁该不该共享）
- 未解疑点：amdaemon 握手后紧跟两次 `WaitForSingleObject(h=NULL, INFINITE) -> 0xffffffff`（每轮都有）——某个资源句柄在 Deck 上是 NULL（不在钩子覆盖面的 API，如 OpenProcess/CreateThread/CreateEvent 无名失败），Windows 上该位置是什么待对照
- 静态分析：amdipc 库（Jenkins 源码路径字符串）同时静态链入两个 exe；管道名由 `\\.\pipe\` + 前缀 + `_amdmsg_CS/SC` 拼接，SC/BS/BC 前缀非字面字符串（动态生成），strings 挖不出更多

**下一轮 = Windows 对照组双插桩（最后一根决定性稻草）**：
1. `deck-deploy/ipcdump.dll`（**STANDALONE v5，236661 字节**）→ Windows `FGOA\App\ipcdump.dll`（覆盖）
2. `deck-deploy/wlanapi.dll`（v5，237572 字节）→ Windows `FGOA\App\am\wlanapi.dll`（新文件，应用目录 shadow；stub 无线枚举返回空列表，不影响 IPC 观测）
3. 正常跑一局到标题画面退出，回传 `App\ipc-dump.log`（ago）+ amdaemon 部分（同文件，pid 区分）
4. 判读目标：①握手后谁先写、写在哪条管道 ②BC0/BS0 交叉连接谁先发起、何时 ③那两个 NULL 句柄位置在 Windows 上是什么对象 ④16B 推送由什么事件触发——拿到这局，Deck 的差异点会直接现形

## 0.25 第二十七轮（2026-09-20 深夜第五轮，ago 未插桩的真因：dll 构建 flavor 错了）

- v4/v5 两轮 ago 都没插桩，不是 IPCDUMP 开关问题：**inject.exe `-k` = CreateRemoteThread+LoadLibraryW，只触发 DllMain**（fgoapifix.dll 有 DllMain 所以一直好使）；而 v4/v5 的 `ipcdump.dll` 是不带 `-DIPCDUMP_STANDALONE` 编的，**二进制里没有 DllMain**（导出表只有 ipcdump_attach 是幌子——DllMain 走 PE AddressOfEntryPoint，不需要导出）→ LoadLibrary 静默无事发生。v3 轮 ago 能 attach 是因为当时部署的是 STANDALONE 版
- **修复**：ago 侧部署的 ipcdump.dll 改用 `-DIPCDUMP_STANDALONE` 编译（带 DllMain）；wlanapi.dll 不变（wlanapi.c 自带 DllMain，其链接的 ipcdump.c 必须非 STANDALONE，否则 DllMain 重复）。验证：nm 见 `T DllMain` + pipesmoke LoadLibrary 冒烟 attach 成功
- **两个 dll 的正确构建命令（勿再搞错）**：
  - `x86_64-w64-mingw32-gcc -shared -O2 -DIPCDUMP_STANDALONE -o ipcdump.dll ipcdump.c`（ago 侧，inject -k 用）
  - `x86_64-w64-mingw32-gcc -shared -O2 -o wlanapi.dll wlanapi.c ipcdump.c`（amdaemon 侧，App/am/ 目录 shadow）
- fgo-launch-deck.sh 的自动加载改动（dll 存在即 -k）保留无害

## 0.24 第二十六轮（2026-09-20 深夜第四轮，v5 首轮：责任侧翻转到 ago——#02 从未发出）

**v5 的 ReadFileEx/WriteFileEx+蹦床钩子完整抓到 amdaemon 管道数据流**（28/32 钩）：
- 握手传输逐字节可见：`pipeWriteEx SC0_amdmsg_SC len=256`（应答）+ `pipeReadEx SC0_amdmsg_CS req=16→done=16→req=240→done=240`（请求），APC 泵 `WaitForSingleObjectEx(alert=1) -> 0xc0` 正常触发
- **决定性一帧**：握手后 amdaemon 立刻 `pipeReadEx SC0_amdmsg_CS req=16` 挂起等 ago 的下一条 16B 消息——**等了 168.5 秒，直到 4102 拆台返回 err=109（ERROR_BROKEN_PIPE）**。amdaemon 完全健康，是**ago 从未发出 #02**（PC 健康局 ago 在收到应答后 +62ms 就写 #02）
- 佐证：v3 PC 日志里 ago 在写 #02 之前的两条 unprotect-16 应重新解读为 amdipc 内部缓冲区解密（CryptProtectMemory 是通用内存混淆），**线上首条 post-handshake 消息就是 ago 的 #02 写**
- BS0 对 accept 依旧全程 pending（与 v4 轮一致）；daemon_backup 建好+写空块，ago 从不读
- **ago 侧连续两轮没插桩**：IPCDUMP=1 开关疑似没带上（.desktop 启动没有该环境变量）或 dll 未就位。**已改 fgo-launch-deck.sh：ipcdump.dll 存在即自动加载（`$BOTTLE/drive_c/FGOA/ipcdump.dll` 存在就 -k，IPCDUMP=0 可强制关，结案后删 dll 即可）**——与 wlanapi shim 的"拷文件即部署"模型统一，不再依赖手动开关

**下一轮（唯一要做的事）**：确认 Deck 上 `drive_c/FGOA/ipcdump.dll` 是 v5（237979 字节）+ `drive_c/FGOA/App/am/wlanapi.dll` 是 v5，然后**正常启动即可**（桌面图标也行，自动加载）。判读目标：①ago 收到握手应答后到（没）写 #02 之间干了什么（WaitNamedPipeW/CreateFileW on BS0？无名事件 INFINITE 卡死？）②ago 的 pipeReadEx 挂在哪 ③若 ago 连 BS0 的 CreateFileW 报错 → wine 管道客户端连接路径实锤

## 0.23 第二十五轮（2026-09-20 深夜第三轮，v4 首轮：amdaemon 侧管道全图景 + 两个采集缺口修复）

**Deck v4 日志（8.2MB，shmdump 门控生效）只有一个 pid=420 = amdaemon**（wlanapi shim 静态导入自动加载，无需开关；ago 侧要 IPCDUMP=1 才有——本轮 ago 未插桩，attach 行缺失）。amdaemon 侧完整序列：

1. `AM Daemon` 互斥 → daemon_table shm → `daemon_live_201223_server0`（server 侧签名，ago 是 client0）
2. **创建全部 4 条管道**（服务端）：`SC0_amdmsg_CS`/`BS0_amdmsg_CS` = INBOUND+OVERLAPPED+FIRST_INSTANCE（server 读），`SC0_amdmsg_SC`/`BS0_amdmsg_SC` = OUTBOUND+OVERLAPPED+FIRST_INSTANCE（server 写），全 BYTE 模式（pipe=0）；4 个 ConnectNamedPipe 全部 err=997 pending
3. +7ms 无名事件(0x15c) INFINITE 等到 → GetOverlappedResult 确认 **SC0 对连接成功**（ago 在 25ms 内连上）→ 握手 16+240 往返（与 v3 一致）→ +436ms 建 daemon_backup shm(4186392B) + 写空 272B 块
4. **BS0 对的 ConnectNamedPipe 永远 pending**：整个会话期间 amdaemon 从未 GetOverlappedResult BS0——ago 要么没连 BS0，要么连了但 amdaemon 的状态机没去收
5. 泵线程群：tid 476/480/484/472 各等无名事件对后进入 `WaitForMultipleObjects(3, INFINITE)` 长停（+22s 才醒一次，无事发生）；tid 508 每 5-10s 一次心跳等待
6. 全程零 pipeRead/pipeWrite（v4 盲区实锤）；+176s 4102 拆台：DisconnectNamedPipe SC0_CS → 重新 ConnectNamedPipe 等下一局

**objdump 导入表实锤**：amdaemon.exe 与 ago.exe 都静态导入 **ReadFileEx/WriteFileEx**（+ WaitForSingleObjectEx/SleepEx alertable 泵点，amdaemon 另有 DeviceIoControl）——amdipc 的管道数据走 overlapped Ex + 完成例程，这就是 v4 看不到任何数据流的原因。ago.exe 还导入 CreateNamedPipeW/ConnectNamedPipe/DisconnectNamedPipe/WaitNamedPipeW（客户端角色待 v5 transcript 确认）

**ipcdump v5（deck-deploy 已更新，冒烟全过）**：
- ReadFileEx/WriteFileEx 钩（仅管道句柄）+ **完成例程蹦床**：替换 app 的 completion routine 为 trampoline，记 err/done + dump 64B 后再调回原例程（OVERLAPPED 指针建映射表，64 槽，独立 CS 防死锁）
- WaitForSingleObjectEx 记录无名句柄的 alertable 等待与 0xC0(WAIT_IO_COMPLETION) 返回（防洪 5 条/句柄）；SleepEx alertable 同理——0xC0 是"APC 泵活着"的直接证据
- DeviceIoControl（仅管道句柄）
- 冒烟：overlapped 管道对回环，pipeReadEx/pipeWriteEx 排队 + pipeReadDone/pipeWriteDone 蹦床回调（数据 hex 正确）+ SleepEx 0xC0 全过。**踩坑：非 OVERLAPPED 句柄上 wine 的 ReadFileEx 退化为同步等数据挂住（第一版冒烟因此卡死）——冒烟管道必须带 FILE_FLAG_OVERLAPPED（amdipc 实际就是这么建的）**

**下一轮（Deck，两个文件都要更新+双进程都要在）**：
1. `ipcdump.dll`(v5) → bottle `drive_c/FGOA/`；`wlanapi.dll`(v5) → `drive_c/FGOA/App/am/`
2. **`IPCDUMP=1 bash ~/Desktop/FGOA/fgoa-play.sh`（本轮 ago 没插桩 = 这个开关没生效或 dll 没就位，跑前确认 `drive_c/FGOA/ipcdump.dll` 是新版）**；回传 `logs/ipc-dump.log`
3. 判读目标：①ago 的 CreateFileW/WaitNamedPipeW 有没有碰 BS0 对、err 是什么 ②握手后 ago/amdaemon 的 pipeReadEx 排队在哪个管道上挂着 ③16B 消息在 PC 上走哪条管道（可选：PC 也换 v5 双进程重跑，wlanapi.dll 放 `App\am\`）④泵线程的 0xC0 节奏——Deck 上握手后还有没有 APC 触发

## 0.22 第二十四轮（2026-09-20 深夜第二轮，Deck 双进程 v3 + PC v3 transcript 对照分析 → 断点锁定在管道层）

**采集**：Deck `ipc-dump.log`（v3，ago=pid 260 + amdaemon=pid 420 经 wlanapi shim 注入成功）+ PC `ipc-dump_PC.log`（ago 两局：pid 72384=v2 10/11 钩、pid 75840=v3 12/13 钩）。

**Deck 时间线（v3 局，ts 27472xxx 起）**：
1. ago 建 daemon_table shm/互斥（它先启动，正常）→ daemon_live 互斥获取成功
2. **握手完整成功**：ago protect 16+240 写出注册 → amdaemon unprotect 读到（密文逐字节一致）→ amdaemon protect 16+240 写回应答 → ago unprotect 读到（一致）。CRYPT32 钩双向交叉验证，管道 256B 往返无丢失
3. amdaemon +439ms：**OpenFileMappingW `amdipc_shm_daemon_backup_140825` err=2 → 自己 Create + MapView size=4186392（与 PC 同尺寸）→ 读首个 272B 槽（全零）→ 写回 272B 块 `1b70a991`+全零（=空状态块）**——daemon_backup 在 Deck 上其实建了！此前"从未出现"是因为 ago 侧从不去 OpenFileMapping
4. 此后 amdaemon 只剩内部 SystemProperty/NFC/AimeDB 互斥轮询（正常 idle 循环），**再无任何 amdipc 报文**；ago 读完握手应答后**零 IPC 动作**，+169s 碰一次 daemon_table 互斥（4102 拆台）收场

**PC 健康局对照（ago pid 75840）**：握手应答 +31ms 起 ago 连读 2 条 16B 消息（`3371742c…`/`c2bf251d…`）→ 回写 16B `#02 {id,0,0x189c,crc}` → 持续数百条 16B 收发（id 递增到 0x1ff+）+ 周期性 131088B 状态块 protect/unprotect；+6.5s ago `OpenFileMappingW daemon_backup` err=0 成功，读出 272/48/144/12304/2064/131088/32784/3088/96/112B 等块。**backup shm 布局** = 8B 头（`e20cf915` cookie + 长度）+ 顺序消息块（PC 首个 272B 块是满数据；Deck 是空块——amdaemon 的状态是从 pipe 消息里喂出来的，没喂就是空）

**结论（4102 断点最终定位）**：断流点在**握手应答之后、16B 消息泵启动之前**，且是 **amdaemon 侧从未发出第一条 16B 推送**（ago 在 PC 上是先收后答）。ago 的泵线程大概率阻塞在管道 ReadFile/WaitForSingleObject（无名事件）上——v3 对管道 I/O 和无名句柄等待全盲，观测已到极限。嫌疑名单：①amdaemon 连接 ago 的 BC0 管道失败/从未尝试（0.21 +file trace 已见 BC0 无连接）②WaitNamedPipe/ConnectNamedPipe/overlapped 管道读在 wine 下的行为差异 ③amdipc 用 GetProcAddress 动态解析管道 API（则 IAT 钩仍盲，v4 结果会暴露这一点）

**v3 工具副作用（已修）**：amdaemon 的 SystemProperty 轮询触发 73 万行 shmdump（680MB 日志）；双进程 `OPEN_ALWAYS+SetFilePointer(END)` 追加写非原子，出现交错损坏（丢过 amdaemon 的 attach 行）

**ipcdump v4（deck-deploy 已更新，本地冒烟全过）**：
- 新增管道钩子：CreateFileW/A（仅 `\pipe\` 路径）、CreateNamedPipeW、ConnectNamedPipe、DisconnectNamedPipe、WaitNamedPipeW、TransactNamedPipe、PeekNamedPipe、SetNamedPipeHandleState、ReadFile/WriteFile（仅管道句柄，同步读写 dump 前 64B hex+ASCII）、GetOverlappedResult、FlushFileBuffers、CloseHandle（注销跟踪）；另加 WaitForMultipleObjects
- WaitForSingleObject 新增**无名句柄 INFINITE 等待**记录（每句柄防洪 5 条）→ 直接抓"泵线程卡在哪个事件上"
- 修复：日志改 FILE_APPEND_DATA 原子追加；shmdump 仅在 daemon/amdipc 互斥锁上触发
- 冒烟：soda runner 下管道回环/WaitNamedPipe err=2/无名事件/门控全部符合预期
- **注意**：IAT 钩只覆盖 exe 静态导入——若 v4 日志仍零管道行，说明 amdipc 动态解析 API，下一版需钩 GetProcAddress 或上 inline hook

**下一轮**（工具全部就绪，只需跑）：
1. Deck：`ipcdump.dll`(v4) → bottle `drive_c/FGOA/`（覆盖）；`wlanapi.dll`(v4) → `drive_c/FGOA/App/am/`（覆盖）；`IPCDUMP=1 bash ~/Desktop/FGOA/fgoa-play.sh`；回传 `drive_c/FGOA/logs/ipc-dump.log`
2. 可选 Windows 对照加强：把 `wlanapi.dll`(v4) 也放 PC 的 `App\am\` → 拿到健康局 amdaemon 侧管道 transcript（注意：stub 的无线枚举返回空列表，与真机 wlanapi 行为有差异，但对 IPC 观测无影响；测完删）
3. 判读目标：①Deck amdaemon 有没有 CreateFileW/WaitNamedPipeW 碰 BC0 管道、err 是什么 ②ago 的泵线程是否卡在无名事件 INFINITE ③16B 消息在 Windows 上走哪条管道（名字/方向/同步还是 overlapped）

## 0.21 第二十三轮（2026-09-20 深夜，billing 全通、4102 真凶收窄到 amdaemon↔ago IPC 断流）

- 修复三验证通过：8077 就绪、TLS1.0 探针 `Cipher is ECDHE-RSA-AES256-SHA`；但 **billing 其实在修复一后（16:09 轮）就已通**（billing.log 有 checkin + result=0 响应）——客户端讲 TLS1.2，此前的"客户端只会 TLS1.0"推断证伪，修复三作为加固保留无害
- **4102 与 billing 无关**。Windows 对照时间线（9/15 20:49）：auth → **game_init(+0.4s)** → attend → ping → … → DownloadOrder(+2s) → billing(+10s)。Deck：auth ✓ → DownloadOrder ✓ → billing ✓ → **game_init 永远不发**（fgo.log 零记录）
- **分工实锤**（0.18 的 +winsock trace 复核）：auth/DownloadOrder/billing 全是 amdaemon（tid 01a4）干的；`game_init` 字符串只存在于 ago.exe；**ago.exe 全程零 socket I/O**（55 万行 trace 里仅 14 行 winsock：建了个 socket、取了 ConnectEx 指针、直接 close，从未 connect）
- **IPC 层现状**（同一 +file trace）：ago↔amdaemon 用命名管道 `SC0/BS0/BC0_amdmsg_{CS,SC}` + 共享内存 `amdipc_shm_*` + 互斥 `amdipc_shmmtx_*`（amdipc 库，Jenkins build 字符串为证）。握手成功：ago 写 256B 注册消息 → amdaemon 完整读 16+240B → amdaemon 回写 256B。然后**对话中断**：amdaemon 从不连接 ago 建的 BC0 管道、ago 从不碰 BS0、auth 成功后 amdaemon 也未再写管道（auth 结果没送达游戏）→ 游戏拿不到 allnet 会话信息 → cmm 不启动 → 无 game_init → 4102
- 原语排除法：同一 runner 下命名管道/共享内存/命名事件跨进程全部正常（shmprobe 双进程实测）——**不是 wine 原语问题，是协议流在首条消息后中断**（消息内容或状态机差异）
- **新工具 `deck-deploy/ipcdump.dll`**：IAT 钩 ago.exe 的 CRYPT32!CryptProtectMemory/UnprotectMemory（amdipc 报文写前 protect、读后 unprotect；wine 下这两个是 no-op stub），转储到 `C:\FGOA\logs\ipc-dump.log`。launch 脚本加 `IPCDUMP=1` 开关（已提交）
- **ipcdump 第一轮结果（2026-09-20 晚）**：抓到完整握手——ago 发出 `16B头{1,1,228,0} + 240B体`（体=12B 子头{72,72,72}+3 个 "ECK1 " 加密块），amdaemon 应答同构。**但报文是 amdipc 内层加密（message/Crypto.cpp，OpenSSL），抓到的是密文，内容不可读**。一轮往返后再无消息
- **ipcdump v2**（deck-deploy 已更新）：加钩 KERNEL32 命名对象——CreateFileMappingW/OpenFileMappingW/CreateEventW/OpenEventW/CreateMutexW/OpenMutexW（记录 句柄→名字）+ WaitForSingleObject/Ex/SetEvent（只记命名句柄）→ 看游戏打开哪些 amdipc_shm_*/互斥/事件、卡在哪个 Wait。因为 wine 任何 WINEDEBUG channel 都不 trace 命名 section（实测 +file/+sync/+ntdll/+virtual 全无），只能 IAT 钩。日志路径回退：C:\FGOA\logs 不可写时写到主模块同目录（方便 Windows 对照组）。本地冒烟全过
- **ipcdump v2 第一轮结果（2026-09-20 深夜）**：ago 的命名对象序列 = 4 个 `Local\FGOLocalPhoto*`（fgohook 拍照共享）+ `FGODeck_*` + **`amdipc_shm_daemon_table_150805` / `amdipc_shmmtx_daemon_table_150805` / `daemon_table_150805_mtx` / `daemon_live_201223_client0` 全是 ago 自己创建的**（OpenMutex err=2 不存在 → Create，ago 先于 amdaemon 启动属正常）；daemon_live 互斥 INFINITE 获取成功；管道握手 256B 往返（同前，ECK1 密文）；之后 194 秒（着色器编译期）命名对象零活动，编译完再一次 daemon_table 互斥获取成功 → 4102。**ago 侧没有任何卡住的等待**（所有 Wait 都 0x0 返回）——断流点在 amdaemon 侧动作或 shm 内容，ago 侧观测已到极限
- **amdaemon 侧观测手段（wlanapi shim）**：`deck-deploy/wlanapi.dll` = ipcdump + 8 个 wlanapi 桩（行为逐字节镜像 wine 内建：OpenHandle→0/h=1/nv=2、EnumInterfaces→0+空列表、CloseHandle→0，本地实测一致）。amdaemon.exe 静态导入 wlanapi.dll（无线自检，Deck 日志证实零调用）→ 放 `App/am/` 下 wine 应用目录优先加载即注入成功（本地验证通过）。部署 = 拷文件，不改任何现有文件
- **Windows 对照组 transcript 到手（2026-09-20 19:33，v2）**：健康局 = 351 次 protect/unprotect 消息 + 关键差异点 = 握手后 amdaemon 继续推送消息流，ago 在 +6.5s `OpenFileMappingW amdipc_shm_daemon_backup_140825` **成功**（err=0，amdaemon 已建好）并读出 12304/2064/**131088**/32784 B 等大块状态。**Deck 的对照：握手一轮后双向沉默，daemon_backup 从未出现**——断点就在"注册应答之后 amdaemon 继续推送"这步
- **ipcdump v3**（deck-deploy 已更新，Windows App\ 已同步）：加钩 MapViewOfFile + ReleaseMutex，并在命名互斥锁获取成功/释放前**转储所有命名 shm 视图内容**（各 1536B）→ 直接读 daemon_table/daemon_backup 共享内存里写了什么。踩坑：log_line 缓冲 2048 被 4KB hex 撑爆栈（已改 4096 + 截断 1536B）
- **下一轮**：①Deck：ipcdump.dll(v3)→`drive_c/FGOA/`、wlanapi.dll→`drive_c/FGOA/App/am/`，`IPCDUMP=1 bash ~/Desktop/FGOA/fgoa-play.sh`，回传 ipc-dump.log（双进程：ago + amdaemon）②Windows 用 v3 重跑一局（dll 已在 App\），回传 App\ipc-dump.log——对照 daemon_table/backup 两边各写了啥

## 0.20 第二十二轮（2026-09-20 深夜，修复一验证通过但 4102 依旧 → 根因第二层实锤 + 修复三上线）

- 修复一确认生效：启动打印修补信息、openssl `-tls1_2` 握手成功见证书链；GL 层照常全绿（1719/1719、552 帧、exit(0) 干净退出）；但游戏仍 4102
- **根因第二层（本地实测逐层实锤）**：游戏 billing 客户端只讲 TLS1.0（Windows 侧服务器是 `PROTOCOL_TLSv1` = 只收 TLS1.0 却一直正常，反向实证）；放行 TLS1.0 要同时破两道坎：
  1. python ≥3.10 的 `PROTOCOL_TLS_SERVER` 默认 `minimum_version=TLSv1.2`（显式 `SSL_CTX_set_min_proto_version`）
  2. **OpenSSL 3 默认安全级 1 直接禁用 TLS1.0/1.1**（实测：清掉 min 后握手仍 UNSUPPORTED_PROTOCOL alert 70，必须 `@SECLEVEL=0`）
- **OPENSSL_CONF 路线证伪**：`MinProtocol=TLSv1.3` 反证实验——cnf 对 python 进程完全无效（TLS1.2 握手照样通）。修复二的 cnf 只对外部工具（openssl CLI 探针）有用，ARTEMiS 的 TLS 放行只能靠代码改 context
- **修复三（fgoa-server-start.sh 已更新，三态幂等：原始/仅修复一/修复一+坏 wrapper）**：index.py 包装 `uvicorn.config.create_ssl_context`——构造后 `minimum_version=MINIMUM_SUPPORTED` + `set_ciphers('ALL:@SECLEVEL=0')`。uvicorn 返回后还会 set_ciphers(billing 原串)，只换密码列表不动安全级。**本地全真验证：打补丁后的 uvicorn 服务器 TLS1.0（ECDHE-RSA-AES256-SHA）/1.1/1.2/1.3 握手全通**
- **踩坑两连**：①ssl.py 的 `minimum_version`/`verify_mode` setter 用 `super(SSLContext, SSLContext)` 硬引用模块全局名——包装 `ssl.SSLContext` 会让 uvicorn 后续 `verify_mode` 赋值 TypeError → billing 崩 → `asyncio.wait(FIRST_COMPLETED)` 全服关闭 → **表现为"8077 端口未就绪"**（用户实测踩中）；②清除坏 wrapper 的正则要锚定行首（`^`+re.M），否则非贪婪匹配在 finally 里的缩进行就截断，留残行
- Deck 验证命令（服务器运行时）：`openssl s_client -connect 127.0.0.1:9999 -tls1 </dev/null` 应见 `Cipher is ECDHE-RSA-AES256-SHA`（修复前 = errno=104/no peer cert，ssl_out_2.txt 有存档）。注意 Deck 上 openssl CLI 测 tls1 需带 `OPENSSL_CONF=$CNF`（客户端侧也要 SECLEVEL=0）
- 若仍 4102：回传 `~/Desktop/FGOA/logs/<时间戳>/` + `FGOA-Server/logs/artemis-stderr.log`，看 asyncio 握手报错是 protocol version（修复三未生效）还是 no shared cipher（需放松 billing 的 ssl_ciphers 白名单）

## 0.19 第二十一轮（2026-09-20 晚，billing TLS 根因实锤 + 修复就绪，待验证）

- 修复链见 0.18。本轮要做的只有验证：Deck 拷新版 fgoa-server-start.sh → stop/start（应打印 `[server] 已修补 billing TLS context`）→ `openssl s_client -connect 127.0.0.1:9999 -tls1_2 </dev/null` 见证书链 → `bash ~/Desktop/FGOA/fgoa-play.sh`
- 预期成功标志：billing.log 出现 checkin（对照 Windows：`Unregistered Billing checkin ... playcount 0`）、fgo.log 出现 `POST /SDEJ/1100//game_init`、画面越过"データ初期化"
- 若仍 4102：回传 `~/Desktop/FGOA/logs/<时间戳>/`（新版 play 脚本自动收集）+ `FGOA-Server/logs/artemis-stderr.log`，重点看 billing.log/game_init 是否出现、asyncio 的 "SSL handshake failed" traceback
- 若 billing 通了但卡在别处：Wine 侧游戏客户端 TLS 能力（schannel→GnuTLS）成为新嫌疑，用 +winsock/+secur32 trace 再看一轮

## 0.18 第二十轮（2026-09-20 14:27，+winsock 抓到 billing 现行 + TLS 版本根因）

- winsock trace：billing 客户端 connect 127.0.0.1:9999 → select 可写/SO_ERROR=0（连接建立）→ send 成功 → **recv 返回 0xC000020D = STATUS_CONNECTION_RESET** → 两次重试同构；billing.log 零记录（没活到 ARTEMiS 处理器）；8077 只有 auth+DownloadOrder 两次连接，game_init 从未发出
- **根因定位（代码级，2026-09-20 晚二次修正）**：ARTEMiS `index.py` billing 用 uvicorn + SSL 且 `ssl_version=3`（= 废弃的 `ssl.PROTOCOL_TLSv1`）。**新版 python（Deck 3.13）下 `SSLContext(3)` 创建的是客户端用途 context，不能作服务器**——本地 A/B 实测：同代码 `SSLContext(3)` 对所有 TLS 版本一律 `internal error` RST，`PROTOCOL_TLS_SERVER` 立即握手成功。Windows 侧旧 python 的废弃常量还是通用 context 所以能用——平台差异的真正来源
- **修法（双层，fgoa-server-start.sh 内建，幂等）**：①启动前补丁 index.py：`ssl_version=3` → `_ssl.PROTOCOL_TLS_SERVER`（两处，launch_main/launch_billing）②`OPENSSL_CONF=openssl-legacy.cnf`（MinProtocol=TLSv1 + SECLEVEL=0）兜底游戏客户端可能只讲 TLS1.0 的情况
- 验证命令（服务器运行时）：`openssl s_client -connect 127.0.0.1:9999 -tls1_2 </dev/null` 应见证书链（不再需要 -tls1，新版 context 接受所有版本；tls1 路径需配 legacy cnf 测）
- 旁证：stderr 日志里 aimedb（7777，裸 TCP）每分钟 campaign 正常应答——同一进程里只有 TLS 的 billing 死，佐证版本锁定而非进程问题
- 注意：launch 脚本已固化 `MESA_SHADER_CACHE_DISABLE=true` + glshim 默认开（embedded-struct 改写是必需品）+ stub 237,238 默认；fgoa-collect-logs.sh 独立收集脚本已建（含 FGOA-Server/logs 的 artemis stderr）

## 0.17 第十九轮（2026-09-20 12:53，GL 层全绿，卡在 ERROR 4102——问题域转入网络层）

- **GL 层全绿收官**：embedded struct 族最终 12 个（325/328/330/333/335/338/341/344/346/349/351/354）全部 v8.1 改写通过；**1719/1719 编译完成、926 个 program 链接全 status=1、零编译 error**；游戏 60fps 连续渲染 19 分钟，最终 exit(0) 干净退出（用户确认错误画面后的正常退出路径）
- **新阻塞 = ERROR 4102（Unexpected Game Program Failure）**：画面走完"データ初期化"后弹 4102。对照 Windows 成功日志（`FGOA/logs/*.2026-09-16`）：
  - Windows 序列：auth → DownloadOrder → **billing checkin(+10s 起，含 Power on/Sys Auth OK 轨迹）** → **POST /SDEJ/1100//game_init** → …→ 标题
  - Deck：auth + DownloadOrder 成功（allnet.log 12:53:31）后 **fgo.log 零 game_init、billing.log 零 checkin**——**DownloadOrder 之后网络层静默**
- 候选原因：billing checkin 可能来自 amdaemon（轨迹是 amdaemon 的 sys 消息），其 fgohook 实例未必装网络重定向；或 DNS/连接在 192.168.100.1:9999/8077 上静默挂起。**需要用 +winsock trace 看实际 connect 目标与返回值**，不再猜测
- 网络配置侧已排除的：launch 脚本每轮把 segatools.ini 的 dns.default 改写为 192.168.100.1、billingPort=9999/aimedbPort=7777；lo 绑 192.168.100.1 有效（auth 同端口 8077 已通）；fgohook 重定向 192.168.100.1→127.0.0.1 对 ago.exe 生效
- 遗留：fgoa-play.sh 自动收日志未生效（用户报告）——下次跑前确认 Deck 上的 fgoa-play.sh 已更新为带收集器的版本
- **下一轮**：`WINEDEBUG=+winsock MESA_SHADER_CACHE_DISABLE=true GLSHIM=1 GLSHIM_STUB_IDS=237,238 bash ~/Desktop/FGOA/fgoa-play.sh`；跑前先在 Deck 验证 `ss -tlnp | grep -E '8077|9999|7777'` 与 `curl -s -o /dev/null -w '%{http_code}' http://192.168.100.1:9999/`

## 0.16 第十八轮（2026-09-20 午后，v8 改写生效但踩到自己的 bug；embedded struct 族共 6 个）

- **v8 改写生效**：rewritten.txt 记录 325/328/330/333/335/338 共 **6 个** embedded struct shader 全部提升改写；游戏越过 325、`present frames=1 success=1`（呈现过真帧），随后停在 **shader 338 编译失败**：`0:112(1): syntax error, unexpected invalid token, expecting end of file`
- **根因 = v8 自身 bug**：`fix_embedded_structs` 返回的缓冲区**未 NUL 结尾**，而改写路径以 `lens=NULL` 调 glShaderSource → Mesa 用 strlen 读到缓冲区外的驻留垃圾（fix 文件 111 行，错误却报 112 行 = 读了界外字节）。325~335 侥幸（界外恰好是 0），338 中招
- 引擎对 338 失败的处理：重传回退源码（shader_338_q1 存在）后**挂住**（CPU 0% 黑屏无崩溃）——与 325 时代的 0xC0000005 崩溃不同，错误路径行为不止一种
- **v8.1**（md5 `1e9f8a0499f782ffe36855993896a2f7`）：补 `final[fl]=0`；6 个 shader 全部 strlen==out_len 校验通过
- 衍生改进：`fgoa-play.sh` 游戏退出后自动打包日志到 `~/Desktop/FGOA/logs/<时间戳>/`（/tmp/glshim 整目录 + compat.log + inject/exit-trace/amdaemon 日志 + drive_c/logs），且 GLSHIM=1 时每轮启动前自动清 /tmp/glshim（stubbed.txt/loaded.txt 是追加模式）
- **下一轮**：v8.1 + `MESA_SHADER_CACHE_DISABLE=true GLSHIM=1 GLSHIM_STUB_IDS=237,238` 重跑；再往后没有新失败点的话试去 stub

## 0.15 第十七轮（2026-09-20 12:00，MESA_SHADER_CACHE_DISABLE 实锤 + shader 325 根因修复就绪）

- **`MESA_SHADER_CACHE_DISABLE=true` 一轮通过全部 133 个 program**（linklog 到 324 全 status=1），停在 shader 325 编译失败——与冷缓存的第十五轮完全一致 → **zink 崩溃实锤与 Mesa 磁盘着色器缓存的命中/写盘路径相关，禁用缓存 = 现成 workaround**（代价：每次启动重编译，startup 变慢）
- **shader 325 失败源码抓到**（v7 的 shader_325_q0.glsl）：顶点 shader，`layout(std140) uniform PerDraw{ struct{ vec4 m_transform[3]; ...; uint64_t m_opacity_map; ... }g_draws[512]; };`——**接口块内匿名 struct**，NV 编译器接受、Mesa 拒绝（`embedded structure declarations are not allowed`，指向 `}g_draws[512];` 行）。引擎失败后对同 id 重传平凡回退（q1=`result=vec4(1.f)`），随后错误处理 bug 0xC0000005
- 全量扫描本轮 dump：嵌入式 struct + gl_DrawIDARB 族**仅 325 一个**
- **glshim v8 就绪**（md5 275804495dd38c26b7f8ed5fe2e9f639）：`fix_embedded_structs` 把接口块内嵌入 struct（匿名/命名）提升为全局命名 struct `GLSHIM_EMBn`（std140 布局语义不变），插入点在 #version/#extension 之后；本地正例（325 变换输出合法）+ 多个负例（不改写）验证通过；改写记录 `rewritten.txt` + `shader_N_fix.glsl`
- 注意（测试假象留档）：判断改写器负例时 `$(basename)` 会重置 `$?`；shader_237/238 被 stub 时不产生 dump 文件
- **下一轮**：`MESA_SHADER_CACHE_DISABLE=true GLSHIM=1 GLSHIM_STUB_IDS=237,238` + v8 → 验证 325 及后续是否还有同类失败；若进标题画面，再试**去掉 stub**（`MESA_SHADER_CACHE_DISABLE=true GLSHIM=1`）——若真实 237/238 也能过，则 zink 崩溃纯是缓存路径问题、与 shader 内容无关，237/238 材质恢复正常
- Mesa bug 报告素材更新：缓存禁用即通 + 预热必崩（pid 260 十几次同点）+ 0xa76034 NIR 访问器野指针

## 0.14 第十六轮（2026-09-20 11:14，glshim v7 重跑：死点提前到 program 242，案情性质改变）

- stub 正常生效（237→8b31/238→8b30，v7 loaded）；**program 239 link status=1 通过**
- **新死点：`program 242 status query enter` 无 done**（linklog 止于 242，100 个 program status=1）；inject 报 exit(1)（= 无声死亡/外国线程双重 fault 签名，非上轮 0xC0000005）
- **shader 240/241 是普通着色器**：无 textureSize/bindless 构造/BaseInstance/textureLod，仅有共享 heap 模板 → **zink 崩溃不是 237/238 独有内容触发那么简单**
- 与第十五轮（同 stub 配置跑到 324）的关键差异：本轮 fgoglcompat 翻译缓存 + **Mesa 磁盘着色器缓存对 240~324 全部预热**（上轮首次链接时写入）→ 缓存命中/爆发式提交路径嫌疑骤升（呼应 0.10 的 Mesa 缓存线程嫌疑）
- compat.log 关键行（第十五轮 pid 256 块）：`compile shader=325 capture=185 success=0` —— 已确认 `App/captures/` 里**没有** capture 185 源码文件：capture 只是内部计数，fgoglcompat 只在**翻译**失败时 dump，编译失败不落盘 → shader 325 失败源码只能靠 glshim v7 的 `shader_325_q0.glsl` 抓
- compat.log 中 pid 260 的块有十几段、全部止于 depth promote = 用户本轮重复跑了多次均死在同一点 → **预热状态下崩溃是确定性的，非偶发竞态**（MESA_SHADER_CACHE_DISABLE 实验判别力更强）
- 待办：①回传 `App/captures/`（capture 185 = shader 325 失败源码）②本轮 core 的首次 fault 偏移（是否仍 0xa76034）③决定性实验：`MESA_SHADER_CACHE_DISABLE=true GLSHIM=1 GLSHIM_STUB_IDS=237,238` —— 越过 242 = Mesa 磁盘缓存路径实锤；仍死 242 = 确定性内容/状态问题，再考虑扩 stub 到 240,241 ④同配置直接重跑一遍看死点是否漂移（判竞态）

## 0.13 第十五轮（2026-09-20，stub 实验成功 + 新阻塞 shader 325 编译失败）

- **定点 stub 实验成功，判读矩阵走分支一**：stubbed.txt 确认 237→8b31/238→8b30（修复版 v6 生效）；linklog 一路链到 **program 324 status=1**（133 个 program），越过 239 → **实锤：zink 崩溃由 shader 237/238 内容触发**，与本地静态分析互证（shader 237 是全场唯一在 **vertex 阶段对 bindless 句柄做 textureSize** 的着色器，外加唯一的 gl_BaseInstanceARB 用法；其余 11 个 bindless 着色器全是片元）
- **新阻塞（与 zink 崩溃无关）**：shader 325 编译 ERROR `0:77(3): error: embedded structure declarations are not allowed`（本轮新编译的 240~325 中唯一 error，其余 infolog 均为 warning）；programs.txt 止于 glCreateProgram 326（未链接）；inject 报 ago.exe 退出码 **0xC0000005**（= 已知的引擎编译失败错误处理 bug，stub 时代记录于 ago.exe+0xC084F7；本轮 wine SEH 拿到了退出码，说明崩在 wine 线程，非外国线程双重 fault）
- **失败源码丢失**：shader_325.glsl dump 内容是平凡回退 shader（`result=vec4(1.f)`+effect_opacity 包装）——引擎编译失败后对同一 id 重新 ShaderSource 了回退源码，覆盖了 dump；旧缓存 131 文件中嵌入式 struct 检测 0 命中，fgoglcompat 翻译产物此前无此问题，失败源码是本轮新翻译的
- **glshim v7**（deck-deploy/glshim.c，md5 ad300def23d8c36ac4279ff3785f8328）：每次 glShaderSource 额外写 `shader_<id>_q<seq>.glsl` 序号文件，覆盖不再丢现场
- **下一轮**：Deck 更新 glshim.so 后重跑（仍 `GLSHIM=1 GLSHIM_STUB_IDS=237,238`），回传 /tmp/glshim 整目录 + App/captures/compat.log + App/shader-cache-r10 新增文件 + deck-inject-live.log；目标 = 拿到 shader_325_q0.glsl（失败源码原文）
- 待办不变：zink bug（237/238 触发）向 Mesa/fgoglcompat 作者反馈——本轮后证据链已完整（内容触发实锤 + 独有构造定位 + 最小复现方向 = VS bindless textureSize）

## 0.12 第十四轮（2026-09-18 21:45，stub 实验被自身 bug 污染 + 修复）

- v6 stub 生效了（stubbed.txt 有记录）**但 stage=0**：glCreateShader 没走被拦截的通道（dlsym 直传了真实地址），导致 237/238 都被套上 vertex stub → program 239 变成双顶点着色器 → **链接必败**
- 本轮 core（pid 43152）的首次 fault = `libgallium+0x3cf731`：函数入口 `mov 0x8(%rcx)`，rcx=NULL（CR2=0x8）；调用方 `0x3cfeb3` 从某对象 +0xb8 取出 NULL 字段传入——**典型的"链接失败的 program 缺 fragment 成员"路径**，是 stub bug 造成的次生崩溃，与原始 bug（0xa76034 哈希链野指针）不是同一个
- v6 修复：glCreateShader 加入 is_hooked_gl + dispatch 跳板（my_CreateShader_dispatch），ELF/dlsym 双通道都记录 stage。stubbed.txt 里应看到 237→stage=8b31、238→stage=8b30 才算生效
- **下一轮判读**：stub 生效后——还崩且偏移=0xa76034 → 内容无关的 zink 结构性 bug；不崩/越过 239 → 实锤 237/238 的 NIR 内容触发；崩在新偏移 → 再分析

## 0.11 第十三轮（2026-09-18 21:20，coredump 指令级定位）

- 崩溃线程寄存器：wine 处理器 fault 时 rax=0（NtCurrentTeb 返回值）→ `mov 0x2f0(%rax)` 实锤 NULL TEB 双重 fault
- **原始 fault 现场恢复**：wine 处理器正在拷贝的 ucontext 还在栈上，gregs[RIP] = `libgallium+0xa76034`，RAX=0x7f3838578850（未映射垃圾指针）
- 崩溃函数（起点 0xa75e80）结构：按 rsi+0x18 类型字节分派（3/4），类型 4 走 14 项跳转表（值 0x33-0x40），类型 3 遍历数组；crash 在其中的哈希链探测循环（0x28 字节条目：+0x18 next、+0x1c hash 字节、+0x20 key）——典型的 NIR/SPIR-V 指令访问器/分析 pass
- 结论：program 239 这对 shader（237+238）的 NIR 处理触发了 libgallium 内部状态损坏（越界写/UAF），非竞态特征更明显（100% 同一偏移复现）
- jump table 大部分 case 收敛到两个子函数（0x65b2f0 / 0xa71850）；pass 表逆向未命中（函数未注册进 nir pass 元数据表）
- **glshim v6 就绪**：`GLSHIM_STUB_IDS=237,238` 定点替换这两个 shader（vertex/fragment 各自最小合法实现，品红色输出），其余功能不变。若 stub 后越过崩溃点 → 实锤 shader 内容触发；游戏也可能直接可玩（该材质渲染异常而已）
- 待跑实验（按序）：①`GLSHIM=1 GLSHIM_STUB_IDS=237,238` ②`MESA_SHADER_CACHE_DISABLE=true`（顺手排除磁盘缓存线程嫌疑）

## 0.10 第十二轮（2026-09-18 20:44，RADV_DEBUG=syncshaders 对照无效）

- syncshaders 后死亡点不变：linklog 仍止于 `program 239 status query enter`，wine 仍无报错输出 → **zink GL 层的异步队列不受 RADV_DEBUG=syncshaders 影响**（那只控制 RADV 内部 ISA 编译）
- 崩溃在外国工作线程、主线程被连坐的模式不变；deck-inject-live.log 无 "Unhandled exception"
- 崩溃线程栈含多个 libc malloc/IO 型帧 → **Mesa 磁盘着色器缓存写入线程**进入嫌疑名单；新增零成本实验 `MESA_SHADER_CACHE_DISABLE=true`
- 待收：libgallium-25.3.0.so（解析栈偏移的钥匙，已两次索要）、20:44 轮新 coredump、journalctl

## 0.9 第十一轮定位（2026-09-18 20:40，coredump gdb 分析：全链路闭合）

- core 文件 `core.ago...14391...zst`（60MB 压缩/3.5GB 解压）gdb 分析：
  - **崩溃线程 = LWP 14708（非主线程）**，栈顶是 wine 信号处理器，栈上 pre-handler 帧全是 `libgallium-25.3.3.0.so`（+0x15126b8/+0x1517da0/+0xa76034 等）+ libc + libGLX_mesa/libGLdispatch
  - fault IP `ntdll.so+0x4a491` = `call NtCurrentTeb` 后的 `mov 0x2f0(%rax)`；**NtCurrentTeb 反汇编 = `jmp pthread_getspecific`**（wine 把 TEB 放在 pthread TLS key 里）→ Mesa 工作线程非 wine 创建 → key 未设置 → NULL+0x2f0 → 二次 SEGV
  - 处理器栈帧在拷贝 EXCEPTION_CONTEXT（wine signal_x86_64 的上下文转换）——首次 fault 触发 SIGSEGV 处理器，处理器自己崩了
  - **主线程 LWP 14391**：`libc+0x116f6d ← libgallium+0x4eaebb ← libc+0x8da75` = 阻塞在 libgallium 内的等待调用（glGetProgramiv 等编译完成），与 linklog "status query enter 无 done" 吻合
- systemd-coredump/journal 里"main 线程崩溃"是误标；ago.exe 历史每轮都有 core
- **责任划分**：首锅 zink/RADV（编译线程 SEGV），次锅 wine（外国线程信号处理不防御 NULL TEB——上游已知薄弱点）
- 下一步：①拷 `/usr/lib/libgallium-25.3.0.so` 回来解析 0x15126b8/0x1517da0/0xa76034 等偏移定位首次 fault 函数 ②`RADV_DEBUG=syncshaders` 对照实验

## 0.8 第十轮定位（2026-09-18 19:45，journalctl 回溯：SIGSEGV 实锤）

- `journalctl --since 19:39` 显示：`Process 14391 (ago.exe) terminated abnormally with signal 11/SEGV`（19:42:34），并生成 coredump；**历史每一轮都有 ago.exe coredump**（mangoapp 的崩溃是无关噪音）
- 崩溃线程栈（systemd 打印）：fault IP `ntdll.so+0x4a491`，调用侧 `opengl32.so+0xa25bf`（wine GL syscall thunk）；unwinder 在 #2-#5 丢帧——后由 0.9 的 core 分析补全为 wine 信号处理器 + libgallium
- 时间线对齐：linklog `status query enter`（19:42:34）== SEGV（19:42:34）
- 排查手段：objdump 解析 soda runner 的 ntdll.so +0x4a491/+0x4d69f、opengl32.so +0xa25bf；coredump gdb 回溯
- systemd-coredump 处理本核耗时 10s、峰值 3.6G 内存（是 coredump 工具本身，与案情无关）

## 0.7 第九轮定位（2026-09-18 19:42，OOM 排除 + 剩余假设收敛）

- **OOM 正式排除**：dmesg 的 `OOM killer disabled/enabled` 是 SteamOS 开机（t=751s）zram 调优，非本次运行；memwatch 显示全程 available ~8GB 无内存压力
- memwatch v1 有 bug 已修：`pgrep -f 'ago\.exe'` 抓到了命令行含 ago.exe 的 start.exe 包装进程；v2 改 `pgrep -x ago.exe`（wine 会把 comm 设成 ago.exe）
- linklog 再次止于 `program 239 status query enter`（19:42:34）；exitlog ago.exe 再次零记录；inject 再次 exit(1)（19:42:41）——两轮完全同构
- exitlog 解读补充：大量 `_exit(0)` + `NtCreateUserProcess` + 各 exe `_start` 的条目是 wine 进程创建的 fork-exec 中间桩，不是真退出；`exit(128)` 空 backtrace = taskkill 找不到目标（Windows 惯例退出码 128），启动清理
- **剩余假设（按嫌疑排序）**：
  1. **Mesa/zink 工作线程（编译线程）SIGSEGV/SIGBUS**：wine 的信号处理器对非 wine 线程（无 TEB）无法转 SEH，re-raise 默认 → 进程瞬间死亡，不经 exit()、不经 wineserver——完美吻合"零记录"。glshim 的信号处理器装在构造函数里，wine ntdll 初始化后会覆盖，抓不到
  2. systemd-oomd / PSI 杀（内存无恙，嫌疑低）
  3. glGetProgramiv 真死锁 + 某个未知的 SIGKILL 来源（嫌疑低，wineserver 正常会话不主动 SIGKILL）
- **可立即回溯验证（不用重跑）**：Deck 上 `coredumpctl list --since "19:30"` 和 `journalctl --since "19:39" --until "19:44" | grep -iE 'segfault|core|oom|kill|wine|ago'`——systemd 会记录段错误/吐核，19:42 那轮若有 SIGSEGV 直接现形
- 下一轮跑时回传 `C:\FGOA\logs\deck-inject-live.log`（err+all 后很小）——wine 崩溃通常有 `wine: Unhandled exception` 或 assertion 行

## 0.6 第八轮定位（2026-09-18 19:40，taskkill 之谜与 SIGKILL 推理）

- 用户操作流程确认：跑脚本 → 窗口创建 → 渲染一帧全白 → **闪退**（无手动杀进程）
- ago.exe 二进制内含 ASCII 模板 `"taskkill  /im %s"`；+file 日志证实 taskkill 由**主线程在启动早期**加载执行（紧跟读 `GameData\SDEJ\ram\PreData.bin` 144 字节之后）= 杀上一轮残留的启动清理，**与死亡无关**
- exitlog 重新解读：pid 9088 `exit(128)` 空 backtrace ≈ taskkill "找不到进程"（Windows taskkill 找不到目标时退出码 128）；pid 9089/9090 taskkill exit(0) = 成功清掉残留；均为启动期事件
- **ago.exe 本体在 exitlog 中零记录**：无 exit/_exit/_Exit/abort、无信号 → 只剩 SIGKILL（不可捕）一条路径。wineserver 不会主动 SIGKILL 正常会话内的进程 → **kernel OOM-killer 是最大嫌疑**（zink/RADV 编译 program 239 的着色器时 LLVM 内存尖峰，Deck 16GB 共享内存）
- 闪退外观吻合 SIGKILL（窗口瞬间消失，非无响应挂住）；inject 看到目标死亡报 exit(1)
- 此前 "dmesg 无异常" 结论作废：只 grep 了 amdgpu|reset|hang，**OOM 行（"Out of memory: Killed process"）不匹配该模式**，需重查
- 待办实验：①跑完立刻 `sudo dmesg | grep -iE 'oom|killed process|out of memory'`（或第二终端 `dmesg -wH` 实时看）②挂死/闪退前 `top -H -p $(pgrep -f ago.exe)` + `free -m` 看内存曲线 ③`RADV_DEBUG=syncshaders` 对照（若编译内存是元凶，同步编译降低峰值且挂点会移到 glLinkProgram 内）

## 0.5 第七轮定位（2026-09-18 19:10，glshim v5 上机）

> ⚠️ 本节对 taskkill 的解读已被 0.6 推翻：taskkill 是 ago.exe 启动清理（杀上轮残留），非 watchdog。保留原始记录备查。

- linklog 尾部：`program 239 status query enter` 后**没有 done** → 死亡点实锤在 `glGetProgramiv(239)` 内部（glshim.c:254 的 real_GetProgramiv 调用）
- **dmesg 干净**：只有开机 amdgpu ring 初始化和电源管理提示，无 GPU lockup/reset/timeout → 排除 GPU hang/device-lost，卡死在 CPU 侧（shader 编译线程）
- exitlog 关键条目：
  - **ago.exe 本身没有任何 exit/abort/信号记录**——GL 线程挂死后从未走过退出路径
  - **bottle 里跑过 taskkill.exe**（pid 9089/9090，exit 0 = 成功杀掉目标）；deck-deploy 脚本、launcher ps1、fgohook/inject/amdaemon 二进制里均无 taskkill 引用 → 来源是用户手动杀或 Bottles/系统 UI 的"停止"按钮（待用户确认）
  - pid 9088 `exit(128)` 且 backtrace 为空（unwind 失败）——疑似 ago.exe 被 wineserver 终止时走的异步退出路径
  - inject.exe `exit(1)`（NtTerminateProcess 路径）= 历史上"exit code 1"的真正来源：游戏被外部杀后 inject 报的码
- **历史现象全部吻合挂死模型**：主线程（0108）沉默后其他线程继续轮询几万行（进程活着）、fgohook 退出诊断无触发（外部终止不经 ago.exe 的 RtlExitUserProcess）、wine trace 尾部无异常
- 待确认：本轮 taskkill 是用户手动杀的还是自动触发的；若是手动 → 此前各轮"exit code 1"大概率都是"挂死后被杀"
- 下一步实验：①`RADV_DEBUG=syncshaders` 跑一轮——挂点从 glGetProgramiv 移到 glLinkProgram（无 returned）则异步编译卡死实锤；若直接通过 239 则是异步竞态 ②挂死时先别杀，`top -H -p $(pgrep -f ago.exe)` 看线程是 100% CPU（LLVM 死循环）还是 0%（死锁）

## 0.4 第六轮定位（2026-09-18，删缓存冷启动 + 缓存↔着色器映射）

- **冷启动照样死，死亡点纹丝不动**：删 shader-cache-r10 后，fgoglcompat 重新翻译 131 个着色器并写盘，新缓存与旧缓存**文件名集合与内容逐字节 100% 一致**（翻译器完全确定性）→ "首轮截断写盘"假设**推翻**
- 冷热两轮死在同一位置：暖轮主线程最后一次缓存**读** = `16271151796059152268-…glsl`；冷轮最后一次缓存**写**（131 次 .tmp+rename）= 同一个文件
- **缓存↔glshim 着色器编号全量映射成功**（md5 对碰）：131 个缓存文件按写盘顺序精确对应 glshim 编译序 #1~#238 中的 131 个（最后一个 = shader_238.glsl，与缓存文件内容相同）→ 死亡点在 shader_238（片元）编译完成后
- program 239 = 顶点 shader_237 + 片元 shader_238 的成对 program，两者都是 heap-SSBO 翻译着色器（蒙皮/灯光系，binding 32-39）
- v4 linklog 尾部：`enter glLinkProgram 239` → `real glLinkProgram 239 returned` → **无 status 行**。glshim 源码（glshim.c:253-255）证实返回后第一条指令就是自己调 `real_GetProgramiv` → **死亡在 Mesa/zink 内部的 glGetProgramiv 里**（或同时在 Mesa 工作线程——zink 异步编译）
- compat.log 每次会话止于 `compile progress 100/1719`（下一 tick 200 永远到不了；总编译量 1719，本阶段只做 238 个，翻译 131 个）
- **glshim v5 已就绪**（deck-deploy/glshim.c）：`exit/_exit/_Exit/abort` 同名插桩 + 8 种致命信号处理器，命中即写 backtrace 到 `/tmp/glshim/exitlog.txt`（含 pid，逐条 fsync）；my_LinkProgram 里 GetProgramiv 前后各加一行 dlog。WSL 冒烟验证：exit(1)/abort 均抓到完整 backtrace。注意：libc 内部 `__libc_start_main→exit` 走隐藏别名抓不到，但 Mesa/wine/fgoglcompat 的显式 exit 都能抓
- **下一步**：Deck 上 `GLSHIM=1 bash ~/Desktop/FGOA/fgo-launch-deck.sh` 跑一轮，回传 `/tmp/glshim/`；同时跑完立刻 `sudo dmesg | grep -iE 'amdgpu|reset|hang' | tail`（验证 RADV GPU hang/device-lost 假设——exit(1) 若是 Mesa 收到 VK_ERROR_DEVICE_LOST 后的自裁，dmesg 会有 ring timeout）

## 0.3 第五轮定位（2026-09-18，+file/+winsock + FGO_EXIT_DIAGNOSTICS）

- Windows 成功序列（9/16 fgo-inject-live.log）：12×UI visibility → Spatial audio → Battle effects → **SMAA init** → 2×motion blur bypass → 硬件后端 → …。Deck 全部死在 12×UI visibility 之后
- FGO_SMAA=0 无效（照样死）→ 不是 SMAA 单体问题，是该阶段整个"惰性着色器加载"路径
- +file 日志：主线程最后动作 = 读完一个 shader-cache-r10 缓存 .glsl（138 次缓存读，全部成功）；**之后到进程结束主线程再无任何文件/网络操作**（56k 行里只有其他线程的轮询）
- fgohook 退出诊断：只捕获到 main 进入 + 一个工作线程正常自退；**RtlExitUserProcess/NtTerminateProcess/PostQuitMessage 均未触发** → main 正常返回路径也不成立
- 服务器对照：Deck 的 billing.log 全空（Windows 成功时 auth+10s 后有 billing 签到）；aimedb 的 "Store ID cannot be 0" 是探针首包，无害
- amdaemon 的 "Failed to send aime play log" 在 Windows 时代同样存在，无害
- 结论：死亡点在 fgoglcompat 内部"读缓存 → 调 glShaderSource"之间，且终止方式绕过常规 API（直接 syscall 或外部进程）

## 0.2 诊断过程留档（2026-09-18，含已被推翻的中间结论，勿再浪费时间）

- ~~"program 239 链接失败导致退出"~~ → **已推翻**：v4 glshim 证实 239 链接成功返回 Mesa；真正死亡点在更后面（见 0.3）
- ~~"wine 连 ELF 插桩都绕过"~~ → 实为 Deck 上 glshim.so 版本未更新（v1/v2 日志无版本标记无法区分，v3 起加版本号）
- ~~"D3D9 动画适配器失败"~~ → +d3d9 trace 全程零调用，排除
- ~~"SMAA 着色器"~~ → FGO_SMAA=0 照样死，排除单体
- 翻译层兼容性结论（有效）：238 个着色器全部编译通过、99 个 program 链接 status=1，fgoglcompat 翻译产物对 Mesa/Zink 完全兼容
- 本地复现不可行（留档）：WSL 只有 llvmpipe（无 ARB_bindless_texture）；zink+lavapipe 在 surfaceless 平台 "failed to choose pdev"

## 0.1 fgoglcompat.dll v0.3.0 逆向要点（2026-09-18 分析）

- 来源：`E:\Games\FGOA\AMD\fgoglcompat-v0.3.0.zip`（解压密码 amd），sha256 `6a7d3b60...96b16d6`，已拷入 `deck-deploy/`
- 导出 `FgoInstall`/`FgoGetProcAddress`，内部名 fgo_gl_compat.dll；仅导入标准 DLL（OPENGL32/USER32/GDI32/KERNEL32...）
- 机制：hook Windows opengl32/WGL 层，自建 resolver 提供全部 NV 入口（glBufferAddressRangeNV/glMakeNamedBufferResidentNV/glGetTextureHandleNV/glMultiDraw*BindlessNV 等）；GLSL 源码翻译：NV 指针→uint64_t+SSBO（蒙皮 binding 16/17、灯光 20/21、草 22、粒子 23、雨 24），warp vote→atomic，产物 `#version 450`
- 自带诊断：翻译失败生成 `#error Heap_shader_translation_failed`，dump `*.original.glsl`/`*.translated.glsl`；日志 `App\captures\compat.log`；缓存 `App\shader-cache-r10`（勿删）
- 风险：`atio6axx_loaded=` 检测 AMD Windows 驱动，Wine 下必不存在，行为未知（仅记录 or 拒绝运行）；只在 RX 9070 XT + 驱动 26.8.1 测过，Mesa GLSL 更严格
- **首次启动约 6 分钟编译着色器缓存，窗口无响应属正常，勿杀**

## 1. 目标与最终形态

- 在 Steam Deck（SteamOS, AMD RDNA2 APU）上运行 FGOA（Fate/Grand Order Arcade，11.00）
- 架构：Bottles（soda-11.0-10 = Wine 11，DXVK 关）跑 ago.exe；ARTEMiS + MariaDB 跑 Linux 原生
- 部署文档与脚本：`E:\Games\FGOA\SteamDeck-Bottles-FGOA部署方案.md` + `E:\Games\FGOA\deck-deploy\`

## 2. 已验证可用的部分（不要再动）

| 组件 | 状态 |
|---|---|
| ARTEMiS 服务器（8077/9999/7777） | ✅ `Service OK`（注意：pip 源用清华镜像；aiomysql==0.3.2 + PyMySQL==1.2.0 锁定；requirements.txt 漏 msgpack 已补；log_dir 解析到 `drive_c/logs` 需预建） |
| MariaDB 10.11.19 tarball（清华镜像） | ✅ Windows 版 datadir 直接复用，玩家数据保留 |
| 192.168.100.1 绑 lo（fgoa-net.service） | ✅ 一次性 sudo |
| inject + fgohook 注入链 | ✅ 全部 hook 初始化成功，amdaemon 正常拉起 |
| fgoapifix.dll | ✅ 修补 ago.exe 静态导入的 `user32.SetWindowFeedbackSetting`（上游 Wine 全版本未导出，IAT 填中止桩 → 0x80000100）。注入时遍历 ago.exe 导入表改写 IAT 槽为返回 TRUE 的桩，只改内存。WSL 对照实验验证。launch 脚本第一个 `-k` |
| Zink GL 后端 | ✅ `GL_BACKEND=zink`（默认），`MESA_LOADER_DRIVER_OVERRIDE=zink`，renderer = `zink Vulkan 1.4 RADV VANGOGH` |
| NV GL 扩展 override | ✅ `MESA_EXTENSION_OVERRIDE="+GL_NV_bindless_texture +GL_NV_shader_buffer_load +GL_NV_vertex_buffer_unified_memory +GL_NV_vertex_attrib_integer_64bit +GL_NV_bindless_multi_draw_indirect"` — fgoglcompat 启用后仅作兜底 |
| fgoglcompat.dll v0.3.0 | ✅ Wine 下完整接管 GL 层：atio6axx_loaded=0 不拒绝运行、16 个 NV→ARB 别名 OK、翻译着色器 238 个零编译失败、99 program 链接全 status=1、翻译缓存 131 文件 |
| ALL.Net 认证链 | ✅ auth + DownloadOrder 每轮成功（`stat:1`）；aimedb 刷卡通信到达过一轮 |
| 窗口创建 | ✅ 1280x720（黑屏→白屏=GL 已成功呈现过 clear 帧） |

## 3. GLSL 层（已由 fgoglcompat 解决，留档）

ago.exe 的着色器分两类：普通着色器（~150 个）+ **NV 指针语法着色器**（蒙皮 41 个 `GL_NV_gpu_shader5`、灯光网格 `GL_NV_shader_buffer_load`）。Mesa 无任何版本支持 GLSL 指针。此问题已由 fgoglcompat.dll（见 0.1）完整解决：NV 指针→uint64_t+SSBO 翻译，238 个着色器在 Zink 下零失败。

stub 时代历史结论（留档备查）：编译失败后游戏错误处理有 bug，在 `ago.exe+0xC084F7` 固定二次崩溃；引擎有常规回退路径（`glBufferAddressRangeNV` 全 address=0）。

## 4. glshim.so（诊断+修补工具，`deck-deploy/glshim.c`，当前 v8.1）

LD_PRELOAD shim，编译：`gcc -shared -fPIC -O2 -o glshim.so glshim.c -ldl`。**注意：v8 起它不只是诊断工具——embedded-struct 提升改写是 12 个 shader 编译通过的必要条件，日常运行也必须带（launch 脚本已默认开启）**

机制要点（都踩过坑，勿回退）：
- wine 的扩展 GL 函数通过 `dlopen+dlsym` 解析 → 必须拦截 `dlsym`/`dlvsym` 本身
- 但**核心 GL 2.0 符号（glLinkProgram 等）wine 走 ELF 直绑** → 必须以同名导出符号做 LD_PRELOAD 插桩（v3 起）
- 真实 `dlsym`/`dlvsym` 地址用手工 GNU hash 表解析 libc 获得（`_dl_iterate_phdr`），无递归
- 功能：dump shader 源码 `/tmp/glshim/shader_N.glsl`、非空 InfoLog `infolog_N.txt`、dlsym 查询 `dlsym.log`、**链接进/出+状态全量记录 `linklog.txt`（逐条 fflush，TerminateProcess 也丢不了）**、链接失败 dump `linkfail_<id>.txt`、`glGetProgramBinary` 进出插桩
- stub 修补模式仍在（NV marker→空实现）但已退役；版本标记：loaded.txt 打 "glshim v6 loaded"
- **v5（2026-09-18 17:10）**：`exit/_exit/_Exit/abort` 同名插桩 + 致命信号处理器 → backtrace 写 `/tmp/glshim/exitlog.txt`（含 pid、fsync）；my_LinkProgram 的 GetProgramiv 前后加 dlog 边界。注意 glibc 内部 `__libc_start_main→exit` 走隐藏别名抓不到
- **v6（2026-09-18 21:20）**：`GLSHIM_STUB_IDS=237,238` 定点 stub（glCreateShader 记录 id→stage，命中名单的 glShaderSource 换成最小合法 GLSL 450：vertex 归零 gl_Position、fragment 输出品红）。用于验证/绕过 program 239 崩溃
- **v7（2026-09-20）**：每次 glShaderSource 额外写 `shader_<id>_q<seq>.glsl` 序号文件——同一 id 被引擎重新 ShaderSource（编译失败回退）时不再丢失败源码现场
- **v8（2026-09-20）**：`fix_embedded_structs` 嵌入式 struct 提升（接口块内匿名/命名 struct → 全局 `GLSHIM_EMBn`，Mesa 拒绝此类声明而 NV 接受）；改写记录 `rewritten.txt` + `shader_N_fix.glsl`

使用：直接 `bash ~/Desktop/FGOA/fgoa-play.sh`（GLSHIM/stub/缓存禁用已固化默认；play 脚本负责起/停服务器；直接跑 fgo-launch-deck.sh 仅在服务器已启动时用）

## 5. 文件清单

`E:\Games\FGOA\deck-deploy\`：
- `fgoa-deck-setup.sh` — 一次性初始化（MariaDB/端口/venv/网络服务/fgoapifix+glshim+fgoglcompat 自动装入 bottle，fgoglcompat 校验 sha256）
- `fgoa-server-start.sh` / `fgoa-server-stop.sh` — 服务器启停
- `fgo-launch-deck.sh` — 游戏启动。环境变量：GL_BACKEND/GLSHIM/FGO_ZH_DLL/FGOGLCOMPAT/FGO_SMAA/WINEDEBUG（0.4 起默认 `err+all`；+file/+winsock 已结案，需手动覆盖启用）；已内置 `FGO_EXIT_DIAGNOSTICS=1`（fgohook 退出诊断）
- `fgoa-play.sh` + 3 个 `.desktop`（Exec 写死 `/home/deck/Desktop/FGOA/`，bash 显式调用免 chmod）；`fgoa-collect-logs.sh` — 日志收集（play 退出时自动调，也可随时手动跑，含 FGOA-Server/logs 的 artemis stderr）
- `fgoapifix.dll` + `fgoapifix.c` — Wine API 补丁（mingw 交叉编译）
- `ipcdump.dll` + `ipcdump.c` — amdipc IPC 诊断 v7（0.29）：v6 全部 + **cngfix**（`cngfix.h`：钩 BCryptSecretAgreement/DeriveKey/DestroySecret，合成 ECDH-HASH 派生密钥修复 soda bcrypt 缺陷=4102 根因修复，`IPCDUMP_CNGFIX=0` 可关）。构建（0.25 踩坑修正）：**ago 侧必须 `x86_64-w64-mingw32-gcc -shared -O2 -DIPCDUMP_STANDALONE -o ipcdump.dll ipcdump.c`（inject -k 只触发 DllMain，非 STANDALONE 编译没有 DllMain = 静默不加载）**；wlanapi 侧相反必须不带该宏
- `cngfix.h` — cngfix 实现（0.29，被 ipcdump.c 和 cngsmoke.c 共有）
- `cngsmoke.c` — CNG/amdipc 密码路径冒烟（ECDH_P256 全链 + AES-CBC + SHA256；`-DCNGFIX_SELFTEST` 自钩验证修复）
- `wlanapi.dll` + `wlanapi.c` — amdaemon 侧 ipcdump 注入载体（应用目录 shadow + 8 个行为镜像桩）。构建：`x86_64-w64-mingw32-gcc -shared -O2 -o wlanapi.dll wlanapi.c ipcdump.c`
- `pipesmoke.c` — ipcdump 本地冒烟工具（配合 `-DIPCDUMP_STANDALONE` 编的 ipcdump_smoke.dll 用，用完删二进制留源码）
- `glshim.so` + `glshim.c` — GL 诊断/修补 shim v6（见第 4 节）
- `fgoglcompat.dll` — AMD GL 兼容补丁 v0.3.0（见 0.1 节）
- `linktest.c` — 本地 GL 链接复现工具（EGL surfaceless；WSL 上 zink 不可用，暂无用）
- `mariadb-10.11.19-linux-systemd-x86_64.tar.gz`（341MB，清华镜像已下好）

Deck 侧位置：脚本+desktop+tarball → `~/Desktop/FGOA/`；glshim.so+fgoapifix.dll+ipcdump.dll → bottle `drive_c/FGOA/`；wlanapi.dll → bottle `drive_c/FGOA/App/am/`（诊断期临时，结案后删）；fgoglcompat.dll → bottle `drive_c/FGOA/App/`；游戏 → bottle `drive_c/FGOA/`（App/AMFS/DEVICE/GameData）；服务器 → bottle `drive_c/FGOA-Server/`

日志回传（全部拷到 `E:\Games\FGOA\deck-logs\`）：
- `drive_c/FGOA/logs/`：`deck-inject-live.log`（主日志）、`fgo-exit-trace.log`（fgohook 退出诊断）、`fgo-av-compat.log`、`fgo-deck-trace.log`
- `drive_c/FGOA/App/captures/compat.log` — fgoglcompat 日志（追加式，多轮累计，pid 区分）
- `drive_c/FGOA/App/shader-cache-r10/` — fgoglcompat 翻译缓存（131 个 .glsl，hash 命名）
- `drive_c/FGOA/GameData/SDEJ/amdaemon.exe.log` — amdaemon 日志
- `drive_c/logs/` — ARTEMiS 服务端日志（allnet/fgo/billing/aimedb/mucha.log 等）
- `drive_c/FGOA-Server/logs/` — 服务器进程 stdout/stderr
- `/tmp/glshim/` 整个目录（GLSHIM=1 时）

## 6. 下一步（新会话从这里继续）

1. **4102 已结案（0.30 终验通过：Deck 进标题）**：根因=soda bcrypt 缺 ECC secret 派生（0.29）；修复=ipcdump v7 内置 cngfix。**Deck 上 v7 的 ipcdump.dll + wlanapi.dll 是永久必需品，勿删**。若某轮又 4102：先确认这两个 dll 还在且是 v7
2. ~~MESA_SHADER_CACHE_DISABLE 根治~~ **已结案（0.39，Deck 复测通过）**：根因二次修正 = 缓存恢复后 `_mesa_program_get_resource_name` 解引用 -1 哨兵（非 0.37 判的 serialize 写路径）。**游戏用的是宿主机系统 Mesa /usr/lib/libgallium-25.3.0.so（直跑 runner 不经 flatpak），部署走用户态 LD_LIBRARY_PATH+LIBGL_DRIVERS_PATH 重定向**（mesa-patch-install.sh / revert.sh）。缓存默认值已随补丁自适应（补丁在→默认开）。**残留：源码补丁两份（get-resource-name-sentinel-fix / serialize-sentinel-hardening）待报 Mesa 上游；SteamOS 更新换 Mesa 后需用 mesa-binpatch.py 对新 libgallium 重打**
2. ~~billing TLS~~ **已结案**（0.20/0.21：修复一~三固化进 fgoa-server-start.sh，billing checkin 全通；注：客户端其实讲 TLS1.2，修复三的 TLS1.0 放行是加固而非必需）
3. **GL 层已结案**：着色器 1719/1719 全过、926 program 全链接、embedded struct 族 12 个全部改写成功（0.17）。纯验证项（不阻塞）：去 stub 跑 `GLSHIM_STUB_IDS="" bash ~/Desktop/FGOA/fgoa-play.sh` 看真实 237/238 在禁缓存下是否也过
4. **zink/Mesa bug 反馈**（证据已齐）：缓存禁用即通 vs 预热必崩（0.14/0.15）、libgallium+0xa76034 NIR 访问器哈希链野指针、wine 外国线程双重 fault；渠道 = fgoglcompat 作者 + Mesa gitlab + wine（NtCurrentTeb NULL 防御）；embedded struct 问题也值得告知作者（翻译器可直接输出提升后的形式）；ARTEMiS 的 ssl_version=3 问题也值得给上游提 issue
5. 遗留：`zh/fgozh.dll`（中文资源）在 Wine 下 DllMain 于 REDIRECT_INDEX 后返回 FALSE，未排查；测试时 `FGO_ZH_DLL=1` 加载（默认不带）
6. 收尾时：launch 脚本 WINEDEBUG 默认值改回 `-all`；glshim 日常保留（embedded-struct 改写是必需品，不是纯诊断）；Windows 侧日志写入 `.workbuddy/memory/`；**Deck 上的 wlanapi.dll + ipcdump.dll 是 cngfix 载体=运行必需品，永久保留（0.30 变更）**

## 7. 关键技术备忘（防重复踩坑）

- **soda-11.0-10 的 bcrypt 编译时缺 ECC secret 派生**：`BCryptSecretAgreement` 成功但 `BCryptDeriveKey(HASH)` 必返 0xC0000002（`+bcrypt` trace 见 `Compiled without ECC secret support`）→ amdipc ECK1 协商静默失败 → 4102。修复=cngfix（ipcdump v7 内置，永久保留）。验证工具 `deck-deploy/cngsmoke.c`（`-DCNGFIX_SELFTEST` 自钩验证）。上游 wine-11.0 是运行时 LOAD_FUNCPTR_OPT 加载 gnutls_privkey_derive_secret，无此编译守卫——是 soda/tkg 构建环境问题
- **Messenger 状态机**（amdaemon.exe，逆向坐标留档）：`putMention`=0x1400706a0（门=`[this+0x20]==3`）、`setState`=0x14006f990（state 2 自动 writeKey）、`pump`=0x14006fa70（state2+key写完+cryptoReady→3）、KEY 体处理=0x140075880（ECK1 导入+makeSecretAgreementKey，label=L"SHA256"，成功置 ctx+0x240）。RTTI+demangled 签名齐全（strings 搜 `amdaemon::ipc::`）

- `MESA_EXTENSION_OVERRIDE` 只对 API 层扩展字符串有效，**GLSL 编译器不认**（NV GLSL 扩展无解，只能改源码）
- Mesa 的 `glXGetProcAddress` 对未支持扩展的入口也返回非 NULL；wine 会自己先查扩展字符串再决定是否返回 NULL —— 所以 override 字符串就能解锁
- Zink/radeonsi 都**没有** `GL_EXT_buffer_reference`（Mesa 26.0 也没有），SSBO 是 Mesa 上唯一的地址型内存访问替代
- SteamOS Mesa 25.3.0；SteamOS 无 gcc（交叉编译在 WSL 做，glibc 前向兼容没问题）
- pkill 模式匹配会误伤自身 shell（用 pid 文件代替）
- Python raw string 不能以 `\` 结尾（脚本里嵌 Python heredoc 时的坑）
- ago.exe 文件不可改（fgohook 校验 PE 时间戳）；fgoapifix 只改内存 IAT
- **wine trace 经管道（tee）缓冲，TerminateProcess 时丢尾部**——"最后一条 trace"不可信；glshim 逐条 fflush 才可信
- **wine 对 GL 扩展函数走 dlsym/glXGetProcAddress，对 GL 2.0 核心符号走 ELF 直绑**——LD_PRELOAD 拦截核心符号必须导出同名符号（见 glshim v3+）
- 进程退出绕过 fgohook 的 RtlExitUserProcess/NtTerminateProcess 内联钩子 = 外部进程终止或直接 syscall（2026-09-18 终案：实为 **SEGV 双重 fault**——wine 信号处理器在外国线程 NtCurrentTeb=NULL 上再崩，不经任何退出 API；见 0.9）
- 调试利器：fgohook 内置 `FGO_EXIT_DIAGNOSTICS=1`（写 `logs/fgo-exit-trace.log`，钩 ExitProcess/NtTerminate*/PostQuitMessage/DestroyWindow + main 进入）；fgoglcompat 日志在 `App\captures\compat.log`（追加式）
- **wine 进程死亡别猜原因，先查 `journalctl | grep SEGV` + `coredumpctl list`**——systemd-coredump 全记录；coredump 在 `/var/lib/systemd/coredump/`（zst 压缩，unzstd 后 gdb 可读）
- **inject.exe `-k` = CreateRemoteThread+LoadLibraryW，只触发 DllMain**——被注入的诊断 dll 必须有 DllMain（ipcdump 的 `-DIPCDUMP_STANDALONE`）；导出函数没人调（0.25 踩坑）
- **amdipc 管道读写走 ReadFileEx/WriteFileEx + 完成例程**（objdump 导入表实锤），ReadFile/WriteFile 钩子看不到数据；alertable 泵点 = WaitForSingleObjectEx/SleepEx，返回 0xC0=WAIT_IO_COMPLETION 是 APC 活着的证据
- **非 OVERLAPPED 句柄上 wine 的 ReadFileEx 会退化为同步等数据挂住**（冒烟踩坑）；amdipc 实际全用 FILE_FLAG_OVERLAPPED 管道，不受影响
- **wlanapi.dll 在真 Windows 的 KnownDLLs 列表里**——应用目录 shadow 注入只在 wine 下好使，Windows 上无效（0.27）
- 多进程写同一日志文件要 **FILE_APPEND_DATA 原子追加**；OPEN_ALWAYS+SetFilePointer(END)+WriteFile 在 wine 下会交错损坏（0.22 踩坑）
- **wine 的 SIGSEGV 处理器在非 wine 线程（无 TEB，如 Mesa 工作线程）上会二次崩溃**（NtCurrentTeb=pthread_getspecific→NULL）：现象=无任何报错/日志的瞬间死亡
- **coredump 里找原始 fault RIP**：wine 处理器崩溃时 rsi 指向正在拷贝的 ucontext，gregs[RIP] 在 ucontext+0xa8（gregs 基址 +0x28，RIP 为 gregs[16]）；CR2 在 gregs[22]
- **Deck 无调试符号的 .so 也能定位**：拷回 .so 用 objdump 反汇编偏移；pass/函数表可用"函数地址字节序搜索 .data.rel.ro + 邻近槽位找 rodata 字符串"识别
- glshim 拦 GL 函数要**双通道覆盖**：dlsym/glXGetProcAddress（is_hooked_gl+dispatch）+ ELF 同名导出——漏一个就会出现"一半 hook 生效"的假象（v6 第一版 glCreateShader 漏 dlsym 通道的教训）
