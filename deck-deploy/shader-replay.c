/* replay.c — FGOA shader/program replay harness for Mesa cache bug
 * GL symbols resolved via eglGetProcAddress (system headers are GL1.x only).
 * Usage: replay <dumpdir>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <EGL/egl.h>
#include <GL/gl.h>

#define MAXID 65536

typedef const GLubyte *(APIENTRYP glGetStringi_t)(GLenum, GLuint);
typedef GLuint (APIENTRYP glCreateShader_t)(GLenum);
typedef void (APIENTRYP glShaderSource_t)(GLuint, GLsizei, const GLchar *const *, const GLint *);
typedef void (APIENTRYP glCompileShader_t)(GLuint);
typedef void (APIENTRYP glGetShaderiv_t)(GLuint, GLenum, GLint *);
typedef void (APIENTRYP glGetShaderInfoLog_t)(GLuint, GLsizei, GLsizei *, GLchar *);
typedef GLuint (APIENTRYP glCreateProgram_t)(void);
typedef void (APIENTRYP glAttachShader_t)(GLuint, GLuint);
typedef void (APIENTRYP glLinkProgram_t)(GLuint);
typedef void (APIENTRYP glGetProgramiv_t)(GLuint, GLenum, GLint *);
typedef void (APIENTRYP glGetProgramInfoLog_t)(GLuint, GLsizei, GLsizei *, GLchar *);
typedef void (APIENTRYP glGetIntegerv_t)(GLenum, GLint *);

static glGetStringi_t pglGetStringi;
static glCreateShader_t pglCreateShader;
static glShaderSource_t pglShaderSource;
static glCompileShader_t pglCompileShader;
static glGetShaderiv_t pglGetShaderiv;
static glGetShaderInfoLog_t pglGetShaderInfoLog;
static glCreateProgram_t pglCreateProgram;
static glAttachShader_t pglAttachShader;
static glLinkProgram_t pglLinkProgram;
static glGetProgramiv_t pglGetProgramiv;
static glGetProgramInfoLog_t pglGetProgramInfoLog;
static glGetIntegerv_t pglGetIntegerv;

#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPUTE_SHADER 0x91B9
#define GL_NUM_EXTENSIONS 0x821D

static char *read_file(const char *path, long *outlen)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(n + 1);
    if (fread(buf, 1, n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
    fclose(f);
    buf[n] = 0;
    if (outlen) *outlen = n;
    return buf;
}

static int detect_stage(const char *src)
{
    if (strstr(src, "local_size_x")) return GL_COMPUTE_SHADER;
    if (strstr(src, "gl_Position") || strstr(src, "gl_VertexID") || strstr(src, "gl_InstanceID") || strstr(src, "gl_DrawID")) return GL_VERTEX_SHADER;
    return GL_FRAGMENT_SHADER;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <dumpdir>\n", argv[0]); return 2; }
    const char *dir = argv[1];

    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY) { fprintf(stderr, "no display\n"); return 1; }
    if (!eglInitialize(dpy, NULL, NULL)) { fprintf(stderr, "eglInit fail\n"); return 1; }
    eglBindAPI(EGL_OPENGL_API);
    EGLint cfgattr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
                         EGL_OPENGL_BIT, EGL_NONE };
    EGLConfig cfg; EGLint ncfg = 0;
    eglChooseConfig(dpy, cfgattr, &cfg, 1, &ncfg);
    EGLContext ctx = eglCreateContext(dpy, ncfg ? cfg : NULL, EGL_NO_CONTEXT,
                       (EGLint[]){ EGL_CONTEXT_MAJOR_VERSION, 4,
                                   EGL_CONTEXT_MINOR_VERSION, 5,
                                   EGL_CONTEXT_OPENGL_PROFILE_MASK,
                                   EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE });
    if (ctx == EGL_NO_CONTEXT)
        ctx = eglCreateContext(dpy, ncfg ? cfg : NULL, EGL_NO_CONTEXT, (EGLint[]){ EGL_NONE });
    if (ctx == EGL_NO_CONTEXT) { fprintf(stderr, "ctx fail 0x%x\n", eglGetError()); return 1; }
    EGLSurface srf = eglCreatePbufferSurface(dpy, ncfg ? cfg : NULL,
                       (EGLint[]){ EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE });
    if (!eglMakeCurrent(dpy, srf, srf, ctx))
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);

    pglGetStringi = (glGetStringi_t)eglGetProcAddress("glGetStringi");
    pglCreateShader = (glCreateShader_t)eglGetProcAddress("glCreateShader");
    pglShaderSource = (glShaderSource_t)eglGetProcAddress("glShaderSource");
    pglCompileShader = (glCompileShader_t)eglGetProcAddress("glCompileShader");
    pglGetShaderiv = (glGetShaderiv_t)eglGetProcAddress("glGetShaderiv");
    pglGetShaderInfoLog = (glGetShaderInfoLog_t)eglGetProcAddress("glGetShaderInfoLog");
    pglCreateProgram = (glCreateProgram_t)eglGetProcAddress("glCreateProgram");
    pglAttachShader = (glAttachShader_t)eglGetProcAddress("glAttachShader");
    pglLinkProgram = (glLinkProgram_t)eglGetProcAddress("glLinkProgram");
    pglGetProgramiv = (glGetProgramiv_t)eglGetProcAddress("glGetProgramiv");
    pglGetProgramInfoLog = (glGetProgramInfoLog_t)eglGetProcAddress("glGetProgramInfoLog");
    pglGetIntegerv = (glGetIntegerv_t)eglGetProcAddress("glGetIntegerv");

    printf("GL_VERSION: %s\nGL_RENDERER: %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));
    GLint next = 0;
    pglGetIntegerv(GL_NUM_EXTENSIONS, &next);
    int has_bindless = 0;
    for (GLint i = 0; i < next; i++) {
        const char *e = (const char *)pglGetStringi(GL_EXTENSIONS, i);
        if (e && strstr(e, "bindless_texture")) has_bindless = 1;
    }
    printf("GL_ARB_bindless_texture: %s\n", has_bindless ? "yes" : "NO");
    fflush(stdout);

    char path[1024];
    snprintf(path, sizeof path, "%s/linklog.txt", dir);
    char *ll = read_file(path, NULL);
    if (!ll) { fprintf(stderr, "no linklog.txt\n"); return 1; }
    char is_program[MAXID] = {0};
    int last_prog = 0, nprog = 0;
    char *save = NULL;
    for (char *line = strtok_r(ll, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        unsigned p;
        if (sscanf(line, "enter glLinkProgram %u", &p) == 1 && p < MAXID) {
            if (!is_program[p]) { is_program[p] = 1; nprog++; if ((int)p > last_prog) last_prog = p; }
        }
    }
    free(ll);
    printf("programs in linklog: %d (last=%d)\n", nprog, last_prog);

    GLuint pending[512]; int npending = 0;
    int links_done = 0, link_fail = 0, compile_fail = 0, shaders_done = 0;

    for (int id = 1; id <= last_prog; id++) {
        if (is_program[id]) {
            GLuint p = pglCreateProgram();
            for (int i = 0; i < npending; i++) pglAttachShader(p, pending[i]);
            npending = 0;
            pglLinkProgram(p);
            GLint st = 0;
            pglGetProgramiv(p, GL_LINK_STATUS, &st);
            links_done++;
            if (!st) {
                link_fail++;
                char logb[512]; GLsizei l = 0;
                pglGetProgramInfoLog(p, sizeof logb - 1, &l, logb); logb[l > 0 ? l : 0] = 0;
                printf("[link] program %d FAIL: %.200s\n", id, logb);
            }
            continue;
        }
        char q0[1024], q1[1024], base[1024];
        snprintf(q0, sizeof q0, "%s/shader_%d_q0.glsl", dir, id);
        snprintf(q1, sizeof q1, "%s/shader_%d_q1.glsl", dir, id);
        snprintf(base, sizeof base, "%s/shader_%d.glsl", dir, id);
        char *src = read_file(q0, NULL);
        if (!src) src = read_file(base, NULL);
        if (!src) continue;

        GLenum stage = detect_stage(src);
        GLuint sh = pglCreateShader(stage);
        const char *sp = src;
        pglShaderSource(sh, 1, &sp, NULL);
        pglCompileShader(sh);
        GLint ok = 0;
        pglGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char *fb = read_file(q1, NULL);
            if (fb) {
                const char *fp = fb;
                pglShaderSource(sh, 1, &fp, NULL);
                pglCompileShader(sh);
                pglGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
                free(fb);
                if (ok) printf("[shader] %d q0 fail -> q1 fallback ok\n", id);
            }
            if (!ok) {
                compile_fail++;
                char logb[2048]; GLsizei l = 0;
                pglGetShaderInfoLog(sh, sizeof logb - 1, &l, logb); logb[l > 0 ? l : 0] = 0;
                printf("[shader] %d COMPILE FAIL (stage=%s):\n%.1800s\n", id, stage==GL_VERTEX_SHADER?"VS":stage==GL_FRAGMENT_SHADER?"FS":"CS", logb);
            }
        }
        shaders_done++;
        free(src);
        if (npending < 512) pending[npending++] = sh;
    }
    printf("DONE: shaders=%d links=%d link_fail=%d compile_fail=%d\n",
           shaders_done, links_done, link_fail, compile_fail);
    return 0;
}
