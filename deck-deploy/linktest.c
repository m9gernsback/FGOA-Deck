/* linktest — 本地复现 program 239 链接：EGL surfaceless + zink/lavapipe
 * 用法: VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json MESA_LOADER_DRIVER_OVERRIDE=zink ./linktest vert.glsl frag.glsl
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <GL/gl.h>

/* ---- 最小 EGL 声明（系统无 EGL 头文件） ---- */
typedef void *EGLDisplay, *EGLConfig, *EGLContext, *EGLSurface;
typedef int EGLint;
typedef unsigned int EGLBoolean, EGLenum;
#define EGL_NO_DISPLAY ((EGLDisplay)0)
#define EGL_NO_CONTEXT ((EGLContext)0)
#define EGL_NO_SURFACE ((EGLSurface)0)
#define EGL_DEFAULT_DISPLAY ((void*)0)
#define EGL_OPENGL_API 0x30A2
#define EGL_SURFACE_TYPE 0x3033
#define EGL_PBUFFER_BIT 0x0001
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_BIT 0x0008
#define EGL_NONE 0x3038
#define EGL_CONTEXT_MAJOR_VERSION 0x3098
#define EGL_CONTEXT_MINOR_VERSION 0x30FB
#define EGL_CONTEXT_OPENGL_PROFILE_MASK 0x30FD
#define EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT 0x00000001
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD

extern EGLDisplay eglGetPlatformDisplay(EGLenum, void*, const EGLint*);
extern EGLBoolean eglInitialize(EGLDisplay, EGLint*, EGLint*);
extern EGLBoolean eglBindAPI(EGLenum);
extern EGLBoolean eglChooseConfig(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*);
extern EGLContext eglCreateContext(EGLDisplay, EGLConfig, EGLContext, const EGLint*);
extern EGLBoolean eglMakeCurrent(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
extern void *eglGetProcAddress(const char*);

typedef GLuint (*PFN_glCreateShader)(GLenum);
typedef void (*PFN_glShaderSource)(GLuint, GLsizei, const GLchar**, const GLint*);
typedef void (*PFN_glCompileShader)(GLuint);
typedef void (*PFN_glGetShaderiv)(GLuint, GLenum, GLint*);
typedef void (*PFN_glGetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
typedef GLuint (*PFN_glCreateProgram)(void);
typedef void (*PFN_glAttachShader)(GLuint, GLuint);
typedef void (*PFN_glLinkProgram)(GLuint);
typedef void (*PFN_glGetProgramiv)(GLuint, GLenum, GLint*);
typedef void (*PFN_glGetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);

static void *gpa(const char *n) { return eglGetProcAddress(n); }

static char *readfile(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) { perror(p); exit(2); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc(n + 1); if (fread(b, 1, n, f) != (size_t)n) { perror("read"); exit(2); } b[n] = 0; fclose(f);
    return b;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s vert.glsl frag.glsl\n", argv[0]); return 2; }
    EGLDisplay dpy = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    if (dpy == EGL_NO_DISPLAY) { fprintf(stderr, "no surfaceless display\n"); return 2; }
    if (!eglInitialize(dpy, NULL, NULL)) { fprintf(stderr, "eglInitialize failed\n"); return 2; }
    eglBindAPI(EGL_OPENGL_API);
    EGLint cfgattrs[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE };
    EGLConfig cfg; EGLint ncfg = 0;
    if (!eglChooseConfig(dpy, cfgattrs, &cfg, 1, &ncfg) || !ncfg) { fprintf(stderr, "no config\n"); return 2; }
    EGLint ctxattrs[] = { EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 5,
                          EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxattrs);
    if (ctx == EGL_NO_CONTEXT) { fprintf(stderr, "no 4.5 core ctx\n"); return 2; }
    if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) { fprintf(stderr, "make current failed\n"); return 2; }
    printf("GL_RENDERER=%s\nGL_VERSION=%s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));

    PFN_glCreateShader CreateShader = (PFN_glCreateShader)gpa("glCreateShader");
    PFN_glShaderSource ShaderSource = (PFN_glShaderSource)gpa("glShaderSource");
    PFN_glCompileShader CompileShader = (PFN_glCompileShader)gpa("glCompileShader");
    PFN_glGetShaderiv GetShaderiv = (PFN_glGetShaderiv)gpa("glGetShaderiv");
    PFN_glGetShaderInfoLog GetShaderInfoLog = (PFN_glGetShaderInfoLog)gpa("glGetShaderInfoLog");
    PFN_glCreateProgram CreateProgram = (PFN_glCreateProgram)gpa("glCreateProgram");
    PFN_glAttachShader AttachShader = (PFN_glAttachShader)gpa("glAttachShader");
    PFN_glLinkProgram LinkProgram = (PFN_glLinkProgram)gpa("glLinkProgram");
    PFN_glGetProgramiv GetProgramiv = (PFN_glGetProgramiv)gpa("glGetProgramiv");
    PFN_glGetProgramInfoLog GetProgramInfoLog = (PFN_glGetProgramInfoLog)gpa("glGetProgramInfoLog");

    GLenum types[2] = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER };
    GLuint prog = CreateProgram();
    for (int i = 0; i < 2; i++) {
        char *src = readfile(argv[i + 1]);
        GLuint s = CreateShader(types[i]);
        const GLchar *c = src;
        ShaderSource(s, 1, &c, NULL);
        CompileShader(s);
        GLint ok = 0; GetShaderiv(s, GL_COMPILE_STATUS, &ok);
        char log[16384]; GLsizei l = 0; GetShaderInfoLog(s, sizeof(log) - 1, &l, log); log[l > 0 ? l : 0] = 0;
        printf("shader %s compile=%d log=%s\n", argv[i + 1], ok, l ? log : "(empty)");
        if (!ok) return 3;
        AttachShader(prog, s);
    }
    printf("linking...\n"); fflush(stdout);
    LinkProgram(prog);
    printf("link returned\n");
    GLint ok = 0; GetProgramiv(prog, GL_LINK_STATUS, &ok);
    char log[16384]; GLsizei l = 0; GetProgramInfoLog(prog, sizeof(log) - 1, &l, log); log[l > 0 ? l : 0] = 0;
    printf("LINK_STATUS=%d log=%s\n", ok, l ? log : "(empty)");
    return ok ? 0 : 4;
}
