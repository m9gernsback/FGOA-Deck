/*
 * glshim.so — FGOA Deck GL 诊断/修补 shim（LD_PRELOAD）
 *
 * 原理：wine 的 opengl 层通过 dlsym/dlvsym（或 glXGetProcAddress）解析 GL 函数。
 *       本 shim 同时拦截 glXGetProcAddress(ARB)、dlsym、dlvsym，
 *       对我们关心的 GL 函数返回包装实现，其余透传。
 *       真实 dlsym/dlvsym 地址通过手工解析 libc 的 GNU hash 表获得，
 *       不依赖任何 dlsym 调用，无引导/递归问题，构造前被调用也安全。
 *
 * 当前功能（诊断模式）：
 *   - glShaderSource    : dump 着色器源码到 /tmp/glshim/shader_<id>.glsl
 *   - glGetShaderInfoLog: 非空日志 dump 到 /tmp/glshim/infolog_<id>.txt
 *   - glCreateProgram   : 记录到 /tmp/glshim/programs.txt
 *   - glLinkProgram     : 链接失败时 dump 关联着色器+日志到 /tmp/glshim/linkfail_<id>.txt
 *   - dlsym/dlvsym 查询名记录到 /tmp/glshim/dlsym.log（摸清 wine 解析路径）
 *
 * 编译：gcc -shared -fPIC -O2 -o glshim.so glshim.c -ldl
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>
#include <link.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <execinfo.h>
#include <ctype.h>
#include <sys/stat.h>

typedef unsigned int GLuint;
typedef unsigned int GLenum;
typedef int GLsizei;
typedef int GLint;
typedef char GLchar;
typedef void (*gl_func_t)(void);
typedef gl_func_t (*glx_gpa_t)(const unsigned char *);
typedef void *(*dlsym_t)(void *, const char *);
typedef void *(*dlvsym_t)(void *, const char *, const char *);

static glx_gpa_t real_gpa;
static dlsym_t real_dlsym_fn;
static dlvsym_t real_dlvsym_fn;
static void *libGL_handle;   /* constructor 里 dlopen 的 libGL，用于 ELF 直绑符号取真实地址 */

/* ---------------- GNU hash 手工符号解析 ---------------- */

static uint32_t gnu_hash(const char *s)
{
    uint32_t h = 5381;
    for (; *s; s++) h = h * 33 + (unsigned char)*s;
    return h;
}

struct sym_req { const char *lib; const char *sym; void *result; };

static int phdr_cb(struct dl_phdr_info *info, size_t size, void *data)
{
    struct sym_req *req = data;
    const ElfW(Dyn) *dyn;
    const char *strtab = NULL, *base = NULL;
    ElfW(Sym) *symtab = NULL;
    uint32_t *gnu = NULL;

    if (req->result) return 1;
    if (!info->dlpi_name || !strstr(info->dlpi_name, req->lib)) return 0;

    for (int i = 0; i < info->dlpi_phnum; i++) {
        if (info->dlpi_phdr[i].p_type == PT_DYNAMIC)
            dyn = (const ElfW(Dyn) *)(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
    }
    if (!dyn) return 0;

    for (; dyn->d_tag != DT_NULL; dyn++) {
        if (dyn->d_tag == DT_STRTAB) strtab = (const char *)dyn->d_un.d_ptr;
        else if (dyn->d_tag == DT_SYMTAB) symtab = (ElfW(Sym) *)dyn->d_un.d_ptr;
        else if (dyn->d_tag == DT_GNU_HASH) gnu = (uint32_t *)dyn->d_un.d_ptr;
    }
    /* PIE/共享库的 dynamic 条目通常是绝对地址（glibc 在加载后已重定位）；
       若看起来是相对值，则加上基址 */
    if (!strtab || !symtab || !gnu) return 0;
    base = (const char *)info->dlpi_addr;
    if ((uintptr_t)strtab < (uintptr_t)base) strtab += (uintptr_t)base;
    if ((uintptr_t)symtab < (uintptr_t)base) symtab = (ElfW(Sym) *)((char *)symtab + (uintptr_t)base);
    if ((uintptr_t)gnu < (uintptr_t)base) gnu = (uint32_t *)((char *)gnu + (uintptr_t)base);

    uint32_t nbuckets = gnu[0], symoffset = gnu[1], bloom_size = gnu[2], bloom_shift = gnu[3];
    const uint64_t *bloom = (const uint64_t *)(gnu + 4);
    const uint32_t *buckets = (const uint32_t *)(bloom + bloom_size);
    const uint32_t *chain = buckets + nbuckets;
    uint32_t h = gnu_hash(req->sym);

    /* 跳过 bloom 预检（其实现差异易误判），chain 遍历本身足够快且严格准确 */
    for (uint32_t i = buckets[h % nbuckets]; i >= symoffset; i++) {
        if (((chain[i - symoffset] | 1) == (h | 1)) &&
            !strcmp(strtab + symtab[i].st_name, req->sym)) {
            req->result = (void *)(base + symtab[i].st_value);
            return 1;
        }
        if (chain[i - symoffset] & 1) break;
    }
    return 0;
}

static void *find_libc_sym(const char *sym)
{
    struct sym_req req = { "libc.so", sym, NULL };
    dl_iterate_phdr(phdr_cb, &req);
    return req.result;
}

/* ---------------- GL 包装 ---------------- */

typedef void (*glShaderSource_t)(GLuint, GLsizei, const GLchar **, const GLint *);
typedef void (*glCompileShader_t)(GLuint);
typedef void (*glGetShaderInfoLog_t)(GLuint, GLsizei, GLsizei *, GLchar *);
typedef GLuint (*glCreateProgram_t)(void);
typedef void (*glLinkProgram_t)(GLuint);
typedef void (*glGetProgramiv_t)(GLuint, GLenum, GLint *);
typedef void (*glGetProgramInfoLog_t)(GLuint, GLsizei, GLsizei *, GLchar *);
typedef void (*glGetAttachedShaders_t)(GLuint, GLsizei, GLsizei *, GLuint *);

static glShaderSource_t real_ShaderSource;
static glCompileShader_t real_CompileShader;
static glGetShaderInfoLog_t real_GetShaderInfoLog;
static glCreateProgram_t real_CreateProgram;
static glLinkProgram_t real_LinkProgram;
static glGetProgramiv_t real_GetProgramiv;
static glGetProgramInfoLog_t real_GetProgramInfoLog;
static glGetAttachedShaders_t real_GetAttachedShaders;

static void *resolve_libgl(const char *name);

/* ---------------- v6: 定点 shader stub（GLSHIM_STUB_IDS="237,238"） ----------------
 * 用于验证/绕过 program 239（shader 237+238）在 zink 编译线程触发的段错误。
 * glCreateShader 记录 id→stage，glShaderSource 命中名单时替换为最小合法实现。 */

#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30

static const char *STUB_VS =
    "#version 450\n"
    "void main() { gl_Position = vec4(0.0, 0.0, 0.0, 1.0); }\n";
static const char *STUB_FS =
    "#version 450\n"
    "layout(location=0) out vec4 fgo_stub_out;\n"
    "void main() { fgo_stub_out = vec4(1.0, 0.0, 1.0, 1.0); }\n";

#define MAX_SHADER_STAGES 4096
static GLenum shader_stages[MAX_SHADER_STAGES];   /* id -> GL_*_SHADER, 0=未知 */

static GLuint stub_ids[64];
static int stub_ids_count = -1;   /* -1 = 未解析 */

static void parse_stub_ids(void)
{
    stub_ids_count = 0;
    const char *env = getenv("GLSHIM_STUB_IDS");
    if (!env || !*env) return;
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", env);
    for (char *tok = strtok(buf, ", "); tok && stub_ids_count < 64; tok = strtok(NULL, ", "))
        stub_ids[stub_ids_count++] = (GLuint)atoi(tok);
}

static int is_stub_target(GLuint s)
{
    if (stub_ids_count < 0) parse_stub_ids();
    for (int i = 0; i < stub_ids_count; i++)
        if (stub_ids[i] == s) return 1;
    return 0;
}

typedef GLuint (*glCreateShader_t)(GLenum);

static GLuint my_CreateShader(glCreateShader_t real_fn, GLenum type)
{
    GLuint id = real_fn ? real_fn(type) : 0;
    if (id && id < MAX_SHADER_STAGES) shader_stages[id] = type;
    return id;
}

__attribute__((visibility("default")))
GLuint glCreateShader(GLenum type)
{
    static glCreateShader_t real_fn;
    if (!real_fn) real_fn = (glCreateShader_t)resolve_libgl("glCreateShader");
    return my_CreateShader(real_fn, type);
}

/* dlsym/glXGetProcAddress 路径用的跳板（real 从 real_gpa 解析） */
static GLuint my_CreateShader_dispatch(GLenum type)
{
    static glCreateShader_t real_fn;
    if (!real_fn && real_gpa)
        real_fn = (glCreateShader_t)real_gpa((const unsigned char *)"glCreateShader");
    if (!real_fn) real_fn = (glCreateShader_t)resolve_libgl("glCreateShader");
    return my_CreateShader(real_fn, type);
}

/*
 * 着色器重写规则（修复模式）：
 * ago.exe 的蒙皮顶点着色器（shader_37 系列）使用 NV 指针语法
 * （layout uniform 块内 PackedVertex 与 InputVertex 的指针成员，依赖
 * NV_shader_buffer_load 的 GLSL 指针扩展），Mesa 任何版本均无等价物
 * → 编译失败 → 游戏错误处理路径崩溃。
 * 但日志证据（glBufferAddressRangeNV 全部 address=0、从未调用 glGetBufferParameterui64vNV）
 * 表明引擎在无 bindless-buffer 时走常规回退路径，该着色器可能只是无条件编译的探针。
 * 因此把它替换为可编译的空实现，验证回退路径是否完整。
 * 设 GLSHIM_NOSTUB=1 可关闭替换（回到纯诊断）。
 */
static const char *NV_SHADER_MARKERS[] = {
    "GL_NV_gpu_shader5",
    "GL_NV_shader_buffer_load",
};
static const char *STUB_SHADER =
    "#version 430\n"
    "void main() {}\n";

static int contains_nv_pointers(const GLchar **strs, const GLint *lens, GLsizei n)
{
    for (GLsizei i = 0; i < n; i++) {
        if (!strs[i]) continue;
        size_t len = lens && lens[i] > 0 ? (size_t)lens[i] : strlen(strs[i]);
        for (size_t m = 0; m < sizeof(NV_SHADER_MARKERS) / sizeof(NV_SHADER_MARKERS[0]); m++) {
            size_t mlen = strlen(NV_SHADER_MARKERS[m]);
            for (size_t j = 0; j + mlen < len; j++)
                if (!memcmp(strs[i] + j, NV_SHADER_MARKERS[m], mlen)) return 1;
        }
    }
    return 0;
}

/* v8: 接口块内匿名/嵌入 struct 提升为全局命名 struct。
 * NV 编译器接受 `uniform B { struct { ... } arr[N]; };`，Mesa 拒绝
 * ("embedded structure declarations are not allowed"，如 shader 325)。
 * 变换：struct{...}decl; → 全局 struct GLSHIM_EMBn{...}; 原位置留 GLSHIM_EMBn decl;
 * std140/std430 布局语义不变。返回 NULL = 无需改写。 */
static char *fix_embedded_structs(const char *src, size_t len, size_t *out_len)
{
    if (!memmem(src, len, "struct", 6)) return NULL;

    size_t cap = len * 2 + 8192;
    char *out = malloc(cap), *hoisted = malloc(cap);
    if (!out || !hoisted) { free(out); free(hoisted); return NULL; }
    size_t o = 0, hlen = 0, i = 0;
    int depth = 0, count = 0;

    while (i < len) {
        char c = src[i];
        /* 注释原样拷贝，不参与状态机 */
        if (c == '/' && i + 1 < len && src[i + 1] == '/') {
            while (i < len && src[i] != '\n') out[o++] = src[i++];
            continue;
        }
        if (c == '/' && i + 1 < len && src[i + 1] == '*') {
            out[o++] = src[i++]; out[o++] = src[i++];
            while (i + 1 < len && !(src[i] == '*' && src[i + 1] == '/')) out[o++] = src[i++];
            if (i + 1 < len) { out[o++] = src[i++]; out[o++] = src[i++]; }
            continue;
        }
        if (c == '{') { depth++; out[o++] = c; i++; continue; }
        if (c == '}') { if (depth > 0) depth--; out[o++] = c; i++; continue; }
        if (depth >= 1 && (i == 0 || (!isalnum((unsigned char)src[i-1]) && src[i-1] != '_'))
            && i + 6 <= len && !memcmp(src + i, "struct", 6)
            && (i + 6 == len || (!isalnum((unsigned char)src[i+6]) && src[i+6] != '_'))) {
            size_t j = i + 6;
            while (j < len && isspace((unsigned char)src[j])) j++;
            /* 跳过可选的 struct 名（嵌入命名 struct 同样非法） */
            if (j < len && (isalpha((unsigned char)src[j]) || src[j] == '_')) {
                size_t ns = j;
                while (j < len && (isalnum((unsigned char)src[j]) || src[j] == '_')) j++;
                size_t j2 = j;
                while (j2 < len && isspace((unsigned char)src[j2])) j2++;
                if (j2 < len && src[j2] == '{') j = j2; /* 命名嵌入 struct */
                else j = ns; /* "struct Foo x;" 引用用法，不是定义 */
            }
            if (j < len && src[j] == '{') {
                int d = 1;
                size_t k = j + 1;
                while (k < len && d > 0) { if (src[k] == '{') d++; else if (src[k] == '}') d--; k++; }
                if (d == 0) {
                    char name[40];
                    snprintf(name, sizeof(name), "GLSHIM_EMB%d", count++);
                    hlen += snprintf(hoisted + hlen, cap - hlen, "struct %s%.*s;\n",
                                     name, (int)(k - j), src + j);
                    size_t nl = strlen(name);
                    memcpy(out + o, name, nl); o += nl;
                    out[o++] = ' '; /* `}g_draws` 的 } 被吃掉后名字与声明符粘连 */
                    i = k;
                    continue;
                }
            }
        }
        out[o++] = c; i++;
    }
    if (!count) { free(out); free(hoisted); return NULL; }

    /* 插入点：文件头所有 # 指令行之后（#version/#extension 必须最前） */
    size_t ins = 0;
    while (ins < o) {
        size_t p = ins;
        while (p < o && (out[p] == ' ' || out[p] == '\t' || out[p] == '\r' || out[p] == '\n')) p++;
        if (p < o && out[p] == '#') {
            while (p < o && out[p] != '\n') p++;
            ins = p < o ? p + 1 : p;
        } else break;
    }
    char *final = malloc(o + hlen + 8);
    if (!final) { free(out); free(hoisted); return NULL; }
    size_t fl = 0;
    memcpy(final, out, ins); fl += ins;
    memcpy(final + fl, hoisted, hlen); fl += hlen;
    memcpy(final + fl, out + ins, o - ins); fl += o - ins;
    final[fl] = 0;   /* 必须 NUL 结尾：调用方以 lens=NULL 传给 glShaderSource，Mesa 走 strlen */
    free(out); free(hoisted);
    *out_len = fl;
    return final;
}

static void my_ShaderSource(GLuint s, GLsizei n, const GLchar **strs, const GLint *lens)
{
    /* v6: 定点 stub 优先于一切（验证 program 239 崩溃假设） */
    if (is_stub_target(s)) {
        GLenum st = (s < MAX_SHADER_STAGES) ? shader_stages[s] : 0;
        const GLchar *stub = (st == GL_FRAGMENT_SHADER) ? STUB_FS : STUB_VS;
        FILE *lf = fopen("/tmp/glshim/stubbed.txt", "a");
        if (lf) { fprintf(lf, "shader %u (stage=%x) replaced with targeted stub\n", s, st); fclose(lf); }
        if (!real_ShaderSource) real_ShaderSource = (glShaderSource_t)resolve_libgl("glShaderSource");
        if (real_ShaderSource) real_ShaderSource(s, 1, &stub, NULL);
        return;
    }

    /* v7: 同一 id 多次 ShaderSource 时全量保留（覆盖式 dump 丢过编译失败现场） */
    static unsigned int src_seq[MAX_SHADER_STAGES];
    unsigned int seq = (s < MAX_SHADER_STAGES) ? src_seq[s]++ : 0;
    char path[128];
    snprintf(path, sizeof(path), "/tmp/glshim/shader_%u.glsl", s);
    FILE *f = fopen(path, "w");
    if (f) {
        for (GLsizei i = 0; i < n; i++) {
            if (!strs[i]) continue;
            size_t len = lens && lens[i] > 0 ? (size_t)lens[i] : strlen(strs[i]);
            fwrite(strs[i], 1, len, f);
        }
        fclose(f);
    }
    snprintf(path, sizeof(path), "/tmp/glshim/shader_%u_q%u.glsl", s, seq);
    f = fopen(path, "w");
    if (f) {
        for (GLsizei i = 0; i < n; i++) {
            if (!strs[i]) continue;
            size_t len = lens && lens[i] > 0 ? (size_t)lens[i] : strlen(strs[i]);
            fwrite(strs[i], 1, len, f);
        }
        fclose(f);
    }

    if (!getenv("GLSHIM_NOSTUB") && contains_nv_pointers(strs, lens, n)) {
        const GLchar *stub = STUB_SHADER;
        FILE *lf = fopen("/tmp/glshim/stubbed.txt", "a");
        if (lf) { fprintf(lf, "shader %u replaced with stub (NV pointer syntax)\n", s); fclose(lf); }
        real_ShaderSource(s, 1, &stub, NULL);
        return;
    }

    /* v8: 嵌入式 struct 提升（Mesa 拒绝，NV 接受） */
    {
        size_t total = 0;
        for (GLsizei i = 0; i < n; i++) {
            if (!strs[i]) continue;
            total += lens && lens[i] > 0 ? (size_t)lens[i] : strlen(strs[i]);
        }
        char *flat = malloc(total + 1);
        if (flat) {
            size_t p = 0;
            for (GLsizei i = 0; i < n; i++) {
                if (!strs[i]) continue;
                size_t l = lens && lens[i] > 0 ? (size_t)lens[i] : strlen(strs[i]);
                memcpy(flat + p, strs[i], l); p += l;
            }
            flat[p] = 0;
            size_t flen = 0;
            char *fixed = fix_embedded_structs(flat, p, &flen);
            if (fixed) {
                snprintf(path, sizeof(path), "/tmp/glshim/shader_%u_fix.glsl", s);
                FILE *ff = fopen(path, "w");
                if (ff) { fwrite(fixed, 1, flen, ff); fclose(ff); }
                FILE *lf = fopen("/tmp/glshim/rewritten.txt", "a");
                if (lf) { fprintf(lf, "shader %u: embedded struct hoisted (%zu -> %zu bytes)\n", s, p, flen); fclose(lf); }
                real_ShaderSource(s, 1, (const GLchar **)&fixed, NULL);
                free(fixed); free(flat);
                return;
            }
            free(flat);
        }
    }
    real_ShaderSource(s, n, strs, lens);
}

static void my_CompileShader(GLuint s)
{
    real_CompileShader(s);
}

static void my_GetShaderInfoLog(GLuint s, GLsizei bs, GLsizei *len, GLchar *log)
{
    real_GetShaderInfoLog(s, bs, len, log);
    if (log && log[0]) {
        char path[128];
        snprintf(path, sizeof(path), "/tmp/glshim/infolog_%u.txt", s);
        FILE *f = fopen(path, "w");
        if (f) { fputs(log, f); fclose(f); }
    }
}

static GLuint my_CreateProgram(void)
{
    GLuint p = real_CreateProgram();
    FILE *f = fopen("/tmp/glshim/programs.txt", "a");
    if (f) { fprintf(f, "glCreateProgram -> %u\n", p); fclose(f); }
    return p;
}

/* 统一诊断日志（append + fflush，保证 TerminateProcess 也丢不了） */
static void dlog(const char *fmt, GLuint a, int b)
{
    static FILE *f;
    if (!f) f = fopen("/tmp/glshim/linklog.txt", "a");
    if (!f) return;
    fprintf(f, fmt, a, b);
    fflush(f);
}

static void *resolve_libgl(const char *name)
{
    if (!real_dlsym_fn) real_dlsym_fn = (dlsym_t)find_libc_sym("dlsym");
    if (real_dlsym_fn && !libGL_handle) {
        libGL_handle = dlopen("libGL.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!libGL_handle) libGL_handle = dlopen("libGL.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (real_dlsym_fn && libGL_handle) {
        void *p = real_dlsym_fn(libGL_handle, name);
        if (p) return p;
    }
    return real_gpa ? (void *)real_gpa((const unsigned char *)name) : NULL;
}

/* glLinkProgram 失败时 dump 失败日志 + 关联着色器 id 到 /tmp/glshim/linkfail_<program>.txt
 * 注意：wine 对 glLinkProgram 走 ELF 直接绑定（不经 dlsym/glXGetProcAddress），
 * 因此本函数必须以导出符号形式提供（LD_PRELOAD 插桩），仅靠 dispatch() 拦截不到。 */
static void my_LinkProgram(GLuint p)
{
    dlog("enter glLinkProgram %u\n", p, 0);
    if (!real_LinkProgram)
        real_LinkProgram = (glLinkProgram_t)resolve_libgl("glLinkProgram");
    if (!real_LinkProgram) { dlog("no real glLinkProgram for %u\n", p, 0); return; }
    real_LinkProgram(p);
    dlog("real glLinkProgram %u returned\n", p, 0);
    if (!real_GetProgramiv)
        real_GetProgramiv = (glGetProgramiv_t)resolve_libgl("glGetProgramiv");
    if (!real_GetProgramInfoLog)
        real_GetProgramInfoLog = (glGetProgramInfoLog_t)resolve_libgl("glGetProgramInfoLog");
    if (!real_GetAttachedShaders)
        real_GetAttachedShaders = (glGetAttachedShaders_t)resolve_libgl("glGetAttachedShaders");
    if (!real_GetProgramiv || !real_GetProgramInfoLog || !real_GetAttachedShaders) return;

    GLint status = 0;
    dlog("program %u status query enter\n", p, 0);
    real_GetProgramiv(p, 0x8B82 /* GL_LINK_STATUS */, &status);
    dlog("program %u status query done\n", p, 0);
    dlog("program %u link status=%d\n", p, status);
    if (status) return;

    char path[128];
    snprintf(path, sizeof(path), "/tmp/glshim/linkfail_%u.txt", p);
    FILE *f = fopen(path, "w");
    if (f) {
        GLuint sh[32]; GLsizei n = 0;
        real_GetAttachedShaders(p, 32, &n, sh);
        fprintf(f, "program %u link FAILED, attached shaders:", p);
        for (GLsizei i = 0; i < n; i++) fprintf(f, " %u", sh[i]);
        fprintf(f, "\n--- info log ---\n");
        char log[8192]; GLsizei l = 0;
        real_GetProgramInfoLog(p, (GLsizei)sizeof(log) - 1, &l, log);
        log[l > 0 ? l : 0] = '\0';
        fputs(log, f);
        fclose(f);
    }
    FILE *idx = fopen("/tmp/glshim/linkfail.txt", "a");
    if (idx) { fprintf(idx, "program %u link failed\n", p); fclose(idx); }
}

static int is_hooked_gl(const char *n)
{
    return !strcmp(n, "glShaderSource") || !strcmp(n, "glCompileShader") ||
           !strcmp(n, "glGetShaderInfoLog") || !strcmp(n, "glCreateProgram") ||
           !strcmp(n, "glLinkProgram") || !strcmp(n, "glCreateShader");
}

static gl_func_t dispatch(const char *n)
{
    if (!real_gpa) return NULL;
    if (!strcmp(n, "glShaderSource")) {
        if (!real_ShaderSource) real_ShaderSource = (glShaderSource_t)real_gpa((const unsigned char *)n);
        return (gl_func_t)my_ShaderSource;
    }
    if (!strcmp(n, "glCompileShader")) {
        if (!real_CompileShader) real_CompileShader = (glCompileShader_t)real_gpa((const unsigned char *)n);
        return (gl_func_t)my_CompileShader;
    }
    if (!strcmp(n, "glGetShaderInfoLog")) {
        if (!real_GetShaderInfoLog) real_GetShaderInfoLog = (glGetShaderInfoLog_t)real_gpa((const unsigned char *)n);
        return (gl_func_t)my_GetShaderInfoLog;
    }
    if (!strcmp(n, "glCreateProgram")) {
        if (!real_CreateProgram) real_CreateProgram = (glCreateProgram_t)real_gpa((const unsigned char *)n);
        return (gl_func_t)my_CreateProgram;
    }
    if (!strcmp(n, "glLinkProgram")) {
        return (gl_func_t)my_LinkProgram;   /* 真实地址惰性解析，见 my_LinkProgram */
    }
    if (!strcmp(n, "glCreateShader")) {
        return (gl_func_t)my_CreateShader_dispatch;
    }
    return real_gpa((const unsigned char *)n);
}

/* 记录查询过的符号名（去重） */
static void log_sym(const char *tag, const char *n)
{
    static char seen[512][64];
    static int count;
    if (!n || strlen(n) >= 64) return;
    for (int i = 0; i < count; i++) if (!strcmp(seen[i], n)) return;
    if (count < 512) {
        strcpy(seen[count++], n);
        FILE *f = fopen("/tmp/glshim/dlsym.log", "a");
        if (f) { fprintf(f, "%s %s\n", tag, n); fclose(f); }
    }
}

/* ---------------- 导出的拦截符号 ---------------- */

__attribute__((visibility("default")))
gl_func_t glXGetProcAddress(const unsigned char *name)
{
    return dispatch((const char *)name);
}

__attribute__((visibility("default")))
gl_func_t glXGetProcAddressARB(const unsigned char *name)
{
    return dispatch((const char *)name);
}

static int ready(void)
{
    if (!real_dlsym_fn) real_dlsym_fn = (dlsym_t)find_libc_sym("dlsym");
    if (!real_dlvsym_fn) real_dlvsym_fn = (dlvsym_t)find_libc_sym("dlvsym");
    return real_dlsym_fn != NULL;
}

__attribute__((visibility("default")))
void *dlsym(void *handle, const char *name)
{
    if (!ready()) return NULL;
    log_sym("dlsym", name);
    if (name && (!strcmp(name, "glXGetProcAddress") || !strcmp(name, "glXGetProcAddressARB")))
        return (void *)glXGetProcAddress;
    if (name && is_hooked_gl(name))
        return (void *)dispatch(name);
    return real_dlsym_fn(handle, name);
}

__attribute__((visibility("default")))
void *dlvsym(void *handle, const char *name, const char *version)
{
    if (!ready()) return NULL;
    log_sym("dlvsym", name);
    if (name && (!strcmp(name, "glXGetProcAddress") || !strcmp(name, "glXGetProcAddressARB")))
        return (void *)glXGetProcAddress;
    if (name && is_hooked_gl(name))
        return (void *)dispatch(name);
    if (real_dlvsym_fn) return real_dlvsym_fn(handle, name, version);
    return real_dlsym_fn(handle, name);
}

/* ELF 直接绑定插桩：wine 对 glLinkProgram 等核心 GL 符号不经 dlsym/glXGetProcAddress，
 * 只有导出同名符号才能被 LD_PRELOAD 截获 */
__attribute__((visibility("default")))
void glLinkProgram(GLuint p)
{
    my_LinkProgram(p);
}

typedef void (*glGetProgramBinary_t)(GLuint, GLsizei, GLsizei *, GLenum *, void *);
/* fgoglcompat 链接后可能调 glGetProgramBinary 写 shader-cache —— 记录进出以定位退出点 */
__attribute__((visibility("default")))
void glGetProgramBinary(GLuint program, GLsizei bufSize, GLsizei *length, GLenum *binaryFormat, void *binary)
{
    static glGetProgramBinary_t real_fn;
    dlog("enter glGetProgramBinary %u\n", program, 0);
    if (!real_fn) real_fn = (glGetProgramBinary_t)resolve_libgl("glGetProgramBinary");
    if (!real_fn) { dlog("no real glGetProgramBinary %u\n", program, 0); return; }
    real_fn(program, bufSize, length, binaryFormat, binary);
    dlog("glGetProgramBinary %u returned\n", program, 0);
}

/* ---------------- v5: 退出路径抓捕 ----------------
 * 死因收敛为 glLinkProgram(239) 返回后、glGetProgramiv 查询期间进程 exit(1)，
 * 且绕过 fgohook 的 RtlExitUserProcess/NtTerminateProcess 钩子。
 * 这里在 Linux 侧插桩 exit/_exit/_Exit/abort + 致命信号，记录 backtrace，
 * 直接回答"是谁终止了进程"（Mesa/zink 内部 exit？wine？信号？）。
 * backtrace 对 wine 进程只能给 Linux 侧帧，但足以区分 Mesa/wine/glshim。 */

static void elog_bt(const char *fmt, int a)
{
    int fd = open("/tmp/glshim/exitlog.txt", O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0) return;
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "[pid=%d] ", getpid());
    if (n > 0) { ssize_t w = write(fd, buf, (size_t)n); (void)w; }
    n = snprintf(buf, sizeof(buf), fmt, a);
    if (n > 0) { ssize_t w = write(fd, buf, (size_t)n); (void)w; }
    void *bt[32];
    int frames = backtrace(bt, 32);
    backtrace_symbols_fd(bt, frames, fd);
    fsync(fd);
    close(fd);
}

typedef void (*exit_t)(int);
static exit_t real_exit_fn;

__attribute__((visibility("default")))
void exit(int code)
{
    if (!real_exit_fn) real_exit_fn = (exit_t)find_libc_sym("exit");
    elog_bt("=== exit(%d) intercepted ===\n", code);
    if (real_exit_fn) real_exit_fn(code);
    _exit(code); /* 兜底 */
}

__attribute__((visibility("default")))
void _exit(int code)
{
    typedef void (*uexit_t)(int);
    static uexit_t real_fn;
    if (!real_fn) real_fn = (uexit_t)find_libc_sym("_exit");
    elog_bt("=== _exit(%d) intercepted ===\n", code);
    if (real_fn) real_fn(code);
    for (;;) { } /* 不应到达 */
}

__attribute__((visibility("default")))
void _Exit(int code)
{
    elog_bt("=== _Exit(%d) intercepted ===\n", code);
    _exit(code);
}

__attribute__((visibility("default")))
void abort(void)
{
    typedef void (*abort_t)(void);
    static abort_t real_fn;
    if (!real_fn) real_fn = (abort_t)find_libc_sym("abort");
    elog_bt("=== abort() intercepted ===\n", 0);
    if (real_fn) real_fn();
    _exit(134);
}

static void sig_handler(int sig)
{
    elog_bt("=== signal %d received ===\n", sig);
    signal(sig, SIG_DFL);
    raise(sig);
}

__attribute__((constructor))
static void glshim_init(void)
{
    ready();
    if (real_dlsym_fn) {
        libGL_handle = dlopen("libGL.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!libGL_handle) libGL_handle = dlopen("libGL.so", RTLD_NOW | RTLD_LOCAL);
        if (libGL_handle) real_gpa = (glx_gpa_t)real_dlsym_fn(libGL_handle, "glXGetProcAddress");
    }
    mkdir("/tmp/glshim", 0777);
    /* v5: 致命信号抓捕（sig_handler 里 re-raise 恢复默认行为） */
    int sigs[] = { SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE, SIGTERM, SIGINT, SIGQUIT };
    for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
        signal(sigs[i], sig_handler);
    FILE *f = fopen("/tmp/glshim/loaded.txt", "a");
    if (f) {
        fprintf(f, "glshim v8.1 loaded, real_gpa=%p real_dlsym=%p real_dlvsym=%p\n",
                (void *)real_gpa, (void *)real_dlsym_fn, (void *)real_dlvsym_fn);
        fclose(f);
    }
}
