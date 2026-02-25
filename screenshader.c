/*
 * screenshader - Minimal X11 compositor with GLSL post-processing shaders
 *
 * Captures all X11 windows via XComposite, composites them into an OpenGL FBO,
 * then applies a post-processing fragment shader before displaying to the
 * XComposite overlay window.
 *
 * Usage: screenshader [path/to/shader.frag]
 *        Defaults to shaders/crt.frag if no argument given.
 *        Send SIGUSR1 to hot-reload the shader file.
 *        Send SIGINT/SIGTERM to stop.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <signal.h>
#include <time.h>
#include <poll.h>
#include <unistd.h>
#include <math.h>
#include <libgen.h>
#include <limits.h>
#include <errno.h>
#include <sys/stat.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/shape.h>

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glx.h>
#include <GL/glext.h>
#include <GL/glxext.h>

/* ========================================================================== */
/* Data structures                                                            */
/* ========================================================================== */

typedef struct WinEntry {
    Window          xid;
    Pixmap          pixmap;
    GLXPixmap       glx_pixmap;
    GLuint          texture;
    Damage          damage;
    int             x, y;
    int             width, height;
    int             border_width;
    bool            mapped;
    bool            override_redirect;
    bool            damaged;
    bool            pixmap_valid;
    struct WinEntry *next; /* above (toward viewer) */
    struct WinEntry *prev; /* below */
} WinEntry;

typedef struct {
    /* X11 core */
    Display        *dpy;
    int             screen;
    Window          root;
    Window          overlay;
    int             root_width, root_height;

    /* Extension event bases */
    int             damage_event, damage_error;

    /* GLX */
    GLXContext      glx_ctx;
    GLXWindow       glx_win;
    GLXFBConfig     fbconfig;

    /* Texture-from-pixmap FBConfigs indexed by window depth (0-32) */
    #define MAX_DEPTH 33
    GLXFBConfig     tfp_config[MAX_DEPTH];
    int             tfp_format[MAX_DEPTH]; /* GLX_TEXTURE_FORMAT_*_EXT */
    bool            tfp_available[MAX_DEPTH];

    /* GLX extension function pointers */
    PFNGLXBINDTEXIMAGEEXTPROC    glXBindTexImageEXT;
    PFNGLXRELEASETEXIMAGEEXTPROC glXReleaseTexImageEXT;

    /* OpenGL objects */
    GLuint          fbo;
    GLuint          fbo_texture;
    GLuint          vao;
    GLuint          vbo;

    /* Shader programs */
    GLuint          vert_shader;     /* kept alive for hot-reload */
    GLuint          composite_prog;
    GLuint          postproc_prog;

    /* Post-process uniform locations */
    GLint           u_screen_tex;
    GLint           u_resolution;
    GLint           u_time;

    /* Composite uniform locations */
    GLint           uc_texture;

    /* Window list (doubly-linked, bottom-to-top) */
    WinEntry       *win_head;
    WinEntry       *win_tail;

    /* Runtime shader parameters (read from /tmp/screenshader.params) */
    #define MAX_PARAMS 16
    struct {
        char  name[64];
        float value;
        GLint location;
    } params[MAX_PARAMS];
    int             param_count;
    time_t          param_mtime;     /* last modification time of params file */
    int             param_check_ctr; /* frame counter for periodic check */

    /* Runtime state */
    bool            running;
    bool            needs_redraw;
    bool            shader_reload;
    char           *shader_path;
    char           *shader_dir;      /* directory containing the executable */
    struct timespec start_time;
} Compositor;

#define PARAM_FILE "/tmp/screenshader.params"

/* ========================================================================== */
/* Globals (for signal handlers)                                              */
/* ========================================================================== */

static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_reload  = 0;

/* ========================================================================== */
/* Signal handlers                                                            */
/* ========================================================================== */

static void handle_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static void handle_sigusr1(int sig) {
    (void)sig;
    g_reload = 1;
}

/* ========================================================================== */
/* X error handler                                                            */
/* ========================================================================== */

static volatile int g_last_xerror = 0;

static int x_error_handler(Display *dpy, XErrorEvent *ev) {
    g_last_xerror = ev->error_code;
    (void)dpy;
    return 0;
}

/* ========================================================================== */
/* Utility: load file to string                                               */
/* ========================================================================== */

static char *load_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open %s: %s\n", path, strerror(errno));
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(len + 1);
    if (!buf) { fclose(f); return NULL; }
    if ((long)fread(buf, 1, len, f) != len) {
        fprintf(stderr, "Short read on %s\n", path);
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[len] = '\0';
    fclose(f);
    return buf;
}

/* ========================================================================== */
/* Shader compilation                                                         */
/* ========================================================================== */

static GLuint compile_shader(GLenum type, const char *src, const char *name) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);

    GLint ok;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        fprintf(stderr, "Shader compile error (%s):\n%s\n", name, log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint link_program(GLuint vert, GLuint frag) {
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vert);
    glAttachShader(prog, frag);
    glLinkProgram(prog);

    GLint ok;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        fprintf(stderr, "Shader link error:\n%s\n", log);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

/* ========================================================================== */
/* Window pixmap management                                                   */
/* ========================================================================== */

static void unbind_window_pixmap(Compositor *comp, WinEntry *w) {
    if (!w->pixmap_valid) return;

    glBindTexture(GL_TEXTURE_2D, w->texture);
    comp->glXReleaseTexImageEXT(comp->dpy, w->glx_pixmap, GLX_FRONT_LEFT_EXT);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDeleteTextures(1, &w->texture);
    w->texture = 0;

    glXDestroyPixmap(comp->dpy, w->glx_pixmap);
    w->glx_pixmap = 0;

    XFreePixmap(comp->dpy, w->pixmap);
    w->pixmap = 0;

    w->pixmap_valid = false;
}

static void bind_window_pixmap(Compositor *comp, WinEntry *w) {
    if (w->pixmap_valid) unbind_window_pixmap(comp, w);
    if (!w->mapped || w->width <= 0 || w->height <= 0) return;

    /* Get window attributes to determine depth */
    XWindowAttributes attr;
    if (!XGetWindowAttributes(comp->dpy, w->xid, &attr)) return;
    if (attr.map_state != IsViewable) return;

    /* Look up FBConfig by window depth */
    int depth = attr.depth;
    if (depth <= 0 || depth >= MAX_DEPTH || !comp->tfp_available[depth]) {
        return; /* no matching FBConfig for this depth */
    }
    GLXFBConfig fbcfg = comp->tfp_config[depth];
    int tex_fmt = comp->tfp_format[depth];

    /* Get composite pixmap */
    w->pixmap = XCompositeNameWindowPixmap(comp->dpy, w->xid);
    if (!w->pixmap) return;

    /* Create GLX pixmap */
    int pixmap_attrs[] = {
        GLX_TEXTURE_TARGET_EXT, GLX_TEXTURE_2D_EXT,
        GLX_TEXTURE_FORMAT_EXT, tex_fmt,
        None
    };
    w->glx_pixmap = glXCreatePixmap(comp->dpy, fbcfg, w->pixmap, pixmap_attrs);

    /* XSync to catch errors from the above calls */
    XSync(comp->dpy, False);
    if (!w->glx_pixmap) {
        XFreePixmap(comp->dpy, w->pixmap);
        w->pixmap = 0;
        return;
    }

    /* Create GL texture */
    glGenTextures(1, &w->texture);
    glBindTexture(GL_TEXTURE_2D, w->texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    comp->glXBindTexImageEXT(comp->dpy, w->glx_pixmap, GLX_FRONT_LEFT_EXT, NULL);
    glBindTexture(GL_TEXTURE_2D, 0);

    w->pixmap_valid = true;
    w->damaged = true;
}

/* ========================================================================== */
/* Window list management                                                     */
/* ========================================================================== */

static WinEntry *find_win(Compositor *comp, Window xid) {
    for (WinEntry *w = comp->win_head; w; w = w->next) {
        if (w->xid == xid) return w;
    }
    return NULL;
}

/* Insert at the top of the stacking order */
static WinEntry *add_win(Compositor *comp, Window xid) {
    if (find_win(comp, xid)) return NULL; /* already tracked */
    if (xid == comp->overlay || xid == comp->root) return NULL;

    WinEntry *w = calloc(1, sizeof(WinEntry));
    if (!w) return NULL;
    w->xid = xid;

    /* Link at tail (top of stack) */
    w->prev = comp->win_tail;
    w->next = NULL;
    if (comp->win_tail) comp->win_tail->next = w;
    else comp->win_head = w;
    comp->win_tail = w;

    return w;
}

static void remove_win(Compositor *comp, WinEntry *w) {
    if (!w) return;

    if (w->damage) {
        XDamageDestroy(comp->dpy, w->damage);
        w->damage = 0;
    }
    unbind_window_pixmap(comp, w);

    /* Unlink */
    if (w->prev) w->prev->next = w->next;
    else comp->win_head = w->next;
    if (w->next) w->next->prev = w->prev;
    else comp->win_tail = w->prev;

    free(w);
}

static void restack_win(Compositor *comp, WinEntry *w, Window above_xid) {
    if (!w) return;

    /* Unlink */
    if (w->prev) w->prev->next = w->next;
    else comp->win_head = w->next;
    if (w->next) w->next->prev = w->prev;
    else comp->win_tail = w->prev;
    w->prev = w->next = NULL;

    if (above_xid == None || above_xid == 0) {
        /* Place at bottom */
        w->next = comp->win_head;
        w->prev = NULL;
        if (comp->win_head) comp->win_head->prev = w;
        else comp->win_tail = w;
        comp->win_head = w;
    } else {
        WinEntry *below = find_win(comp, above_xid);
        if (below) {
            /* Place above 'below' */
            w->prev = below;
            w->next = below->next;
            if (below->next) below->next->prev = w;
            else comp->win_tail = w;
            below->next = w;
        } else {
            /* Sibling not found, place at top */
            w->prev = comp->win_tail;
            w->next = NULL;
            if (comp->win_tail) comp->win_tail->next = w;
            else comp->win_head = w;
            comp->win_tail = w;
        }
    }
}

/* ========================================================================== */
/* Event handlers                                                             */
/* ========================================================================== */

static void handle_map(Compositor *comp, XMapEvent *ev) {
    WinEntry *w = find_win(comp, ev->window);
    if (!w) {
        w = add_win(comp, ev->window);
        if (!w) return;
    }

    XWindowAttributes attr;
    if (!XGetWindowAttributes(comp->dpy, ev->window, &attr)) {
        remove_win(comp, w);
        return;
    }

    w->x = attr.x;
    w->y = attr.y;
    w->width = attr.width;
    w->height = attr.height;
    w->border_width = attr.border_width;
    w->override_redirect = attr.override_redirect;
    w->mapped = true;

    /* Create damage tracking */
    if (!w->damage) {
        g_last_xerror = 0;
        w->damage = XDamageCreate(comp->dpy, w->xid, XDamageReportNonEmpty);
        XSync(comp->dpy, False);
        if (g_last_xerror) {
            w->damage = 0;
            g_last_xerror = 0;
        }
    }

    bind_window_pixmap(comp, w);
    comp->needs_redraw = true;
}

static void handle_unmap(Compositor *comp, XUnmapEvent *ev) {
    WinEntry *w = find_win(comp, ev->window);
    if (!w) return;
    w->mapped = false;
    unbind_window_pixmap(comp, w);
    if (w->damage) {
        XDamageDestroy(comp->dpy, w->damage);
        w->damage = 0;
    }
    comp->needs_redraw = true;
}

static void handle_destroy(Compositor *comp, XDestroyWindowEvent *ev) {
    WinEntry *w = find_win(comp, ev->window);
    if (w) {
        remove_win(comp, w);
        comp->needs_redraw = true;
    }
}

static void handle_configure(Compositor *comp, XConfigureEvent *ev) {
    /* Root window resize (e.g. xrandr) */
    if (ev->window == comp->root) {
        if (ev->width != comp->root_width || ev->height != comp->root_height) {
            comp->root_width = ev->width;
            comp->root_height = ev->height;

            /* Resize FBO texture */
            glBindTexture(GL_TEXTURE_2D, comp->fbo_texture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                         comp->root_width, comp->root_height, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            glBindTexture(GL_TEXTURE_2D, 0);
        }
        comp->needs_redraw = true;
        return;
    }

    WinEntry *w = find_win(comp, ev->window);
    if (!w) return;

    bool resized = (w->width != ev->width || w->height != ev->height);
    w->x = ev->x;
    w->y = ev->y;
    w->width = ev->width;
    w->height = ev->height;
    w->border_width = ev->border_width;

    /* Handle restacking */
    restack_win(comp, w, ev->above);

    /* Rebind pixmap if resized */
    if (resized && w->mapped) {
        bind_window_pixmap(comp, w);
    }

    comp->needs_redraw = true;
}

static void handle_reparent(Compositor *comp, XReparentEvent *ev) {
    if (ev->parent == comp->root) {
        /* Window reparented into root -- treat like map if visible */
        XWindowAttributes attr;
        if (XGetWindowAttributes(comp->dpy, ev->window, &attr) &&
            attr.map_state == IsViewable) {
            XMapEvent fake = { .window = ev->window };
            handle_map(comp, &fake);
        }
    } else {
        /* Window reparented away from root -- remove */
        WinEntry *w = find_win(comp, ev->window);
        if (w) {
            remove_win(comp, w);
            comp->needs_redraw = true;
        }
    }
}

static void handle_circulate(Compositor *comp, XCirculateEvent *ev) {
    WinEntry *w = find_win(comp, ev->window);
    if (!w) return;

    /* Unlink */
    if (w->prev) w->prev->next = w->next;
    else comp->win_head = w->next;
    if (w->next) w->next->prev = w->prev;
    else comp->win_tail = w->prev;
    w->prev = w->next = NULL;

    if (ev->place == PlaceOnTop) {
        w->prev = comp->win_tail;
        if (comp->win_tail) comp->win_tail->next = w;
        else comp->win_head = w;
        comp->win_tail = w;
    } else {
        w->next = comp->win_head;
        if (comp->win_head) comp->win_head->prev = w;
        else comp->win_tail = w;
        comp->win_head = w;
    }

    comp->needs_redraw = true;
}

static void handle_event(Compositor *comp, XEvent *ev) {
    if (ev->type == MapNotify) {
        handle_map(comp, &ev->xmap);
    } else if (ev->type == UnmapNotify) {
        handle_unmap(comp, &ev->xunmap);
    } else if (ev->type == DestroyNotify) {
        handle_destroy(comp, &ev->xdestroywindow);
    } else if (ev->type == ConfigureNotify) {
        handle_configure(comp, &ev->xconfigure);
    } else if (ev->type == ReparentNotify) {
        handle_reparent(comp, &ev->xreparent);
    } else if (ev->type == CirculateNotify) {
        handle_circulate(comp, &ev->xcirculate);
    } else if (ev->type == comp->damage_event + XDamageNotify) {
        XDamageNotifyEvent *dev = (XDamageNotifyEvent *)ev;
        WinEntry *w = find_win(comp, dev->drawable);
        if (w && w->damage) {
            w->damaged = true;
            XDamageSubtract(comp->dpy, w->damage, None, None);
            comp->needs_redraw = true;
        }
    }
}

/* Forward declarations */
static void apply_params(Compositor *comp);

/* ========================================================================== */
/* Rendering                                                                  */
/* ========================================================================== */

static void render_frame(Compositor *comp) {
    /* --- Pass 1: Composite all windows into FBO --- */
    glBindFramebuffer(GL_FRAMEBUFFER, comp->fbo);
    glViewport(0, 0, comp->root_width, comp->root_height);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(comp->composite_prog);

    for (WinEntry *w = comp->win_head; w; w = w->next) {
        if (!w->mapped || !w->pixmap_valid) continue;
        if (w->width <= 0 || w->height <= 0) continue;

        /* Re-bind texture if window content changed */
        if (w->damaged) {
            glBindTexture(GL_TEXTURE_2D, w->texture);
            comp->glXReleaseTexImageEXT(comp->dpy, w->glx_pixmap,
                                        GLX_FRONT_LEFT_EXT);
            comp->glXBindTexImageEXT(comp->dpy, w->glx_pixmap,
                                     GLX_FRONT_LEFT_EXT, NULL);
            w->damaged = false;
        }

        /* Use glViewport to position this window within the FBO */
        int wy = comp->root_height - w->y - w->height;
        glViewport(w->x, wy, w->width, w->height);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, w->texture);
        glUniform1i(comp->uc_texture, 0);

        glBindVertexArray(comp->vao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    glDisable(GL_BLEND);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    /* --- Pass 2: Post-process FBO to overlay --- */
    glViewport(0, 0, comp->root_width, comp->root_height);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(comp->postproc_prog);

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    float elapsed = (float)(now.tv_sec - comp->start_time.tv_sec)
                  + (float)(now.tv_nsec - comp->start_time.tv_nsec) / 1e9f;

    glUniform2f(comp->u_resolution,
                (float)comp->root_width, (float)comp->root_height);
    glUniform1f(comp->u_time, elapsed);

    /* Apply user-controlled shader parameters */
    apply_params(comp);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, comp->fbo_texture);
    glUniform1i(comp->u_screen_tex, 0);

    glBindVertexArray(comp->vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* ========================================================================== */
/* Shader hot-reload                                                          */
/* ========================================================================== */

static void reload_postproc_shader(Compositor *comp) {
    fprintf(stderr, "Reloading shader: %s\n", comp->shader_path);

    char *frag_src = load_file(comp->shader_path);
    if (!frag_src) return;

    GLuint frag = compile_shader(GL_FRAGMENT_SHADER, frag_src, comp->shader_path);
    free(frag_src);
    if (!frag) {
        fprintf(stderr, "Hot-reload failed (compile error), keeping current shader\n");
        return;
    }

    GLuint new_prog = link_program(comp->vert_shader, frag);
    glDeleteShader(frag);
    if (!new_prog) {
        fprintf(stderr, "Hot-reload failed (link error), keeping current shader\n");
        return;
    }

    glDeleteProgram(comp->postproc_prog);
    comp->postproc_prog = new_prog;

    comp->u_screen_tex = glGetUniformLocation(new_prog, "u_screen");
    comp->u_resolution = glGetUniformLocation(new_prog, "u_resolution");
    comp->u_time       = glGetUniformLocation(new_prog, "u_time");

    /* Re-resolve param uniform locations for new program */
    for (int i = 0; i < comp->param_count; i++) {
        comp->params[i].location = glGetUniformLocation(new_prog, comp->params[i].name);
    }

    fprintf(stderr, "Shader hot-reloaded successfully\n");
}

/* ========================================================================== */
/* Runtime shader parameters                                                  */
/* ========================================================================== */

static void read_params(Compositor *comp) {
    struct stat st;
    if (stat(PARAM_FILE, &st) != 0) return;
    if (st.st_mtime == comp->param_mtime) return; /* unchanged */
    comp->param_mtime = st.st_mtime;

    FILE *f = fopen(PARAM_FILE, "r");
    if (!f) return;

    comp->param_count = 0;
    char line[256];
    while (fgets(line, sizeof(line), f) && comp->param_count < MAX_PARAMS) {
        char name[64];
        float value;
        if (sscanf(line, "%63s %f", name, &value) == 2) {
            int idx = comp->param_count++;
            strncpy(comp->params[idx].name, name, sizeof(comp->params[idx].name) - 1);
            comp->params[idx].name[sizeof(comp->params[idx].name) - 1] = '\0';
            comp->params[idx].value = value;
            comp->params[idx].location = glGetUniformLocation(comp->postproc_prog, name);
        }
    }
    fclose(f);

    fprintf(stderr, "Loaded %d params from %s\n", comp->param_count, PARAM_FILE);
    comp->needs_redraw = true;
}

static void apply_params(Compositor *comp) {
    for (int i = 0; i < comp->param_count; i++) {
        if (comp->params[i].location >= 0) {
            glUniform1f(comp->params[i].location, comp->params[i].value);
        }
    }
}

/* ========================================================================== */
/* Resolve shader path (relative to executable or absolute)                   */
/* ========================================================================== */

static char *resolve_shader_path(const char *shader_dir, const char *input) {
    /* If absolute or starts with ./ or ../, use as-is */
    if (input[0] == '/' || (input[0] == '.' && (input[1] == '/' || input[1] == '.'))) {
        return strdup(input);
    }

    /* Try relative to executable directory first */
    char *path = malloc(PATH_MAX);
    if (!path) return NULL;
    snprintf(path, PATH_MAX, "%s/%s", shader_dir, input);

    if (access(path, R_OK) == 0) return path;

    /* Fall back to input as-is (relative to CWD) */
    free(path);
    return strdup(input);
}

/* ========================================================================== */
/* Get executable directory                                                   */
/* ========================================================================== */

static char *get_exe_dir(void) {
    char exe_path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len < 0) return strdup(".");
    exe_path[len] = '\0';
    char *dir = dirname(exe_path);
    return strdup(dir);
}

/* ========================================================================== */
/* Find GLXFBConfigs for texture_from_pixmap, indexed by depth                */
/* ========================================================================== */

static void find_tfp_fbconfigs(Compositor *comp) {
    memset(comp->tfp_available, 0, sizeof(comp->tfp_available));

    int nfb = 0;
    GLXFBConfig *configs = glXGetFBConfigs(comp->dpy, comp->screen, &nfb);
    if (!configs) return;

    for (int i = 0; i < nfb; i++) {
        int drawable_type = 0, tex_tgt = 0, dbl = 0, bind_rgb = 0, bind_rgba = 0;

        glXGetFBConfigAttrib(comp->dpy, configs[i], GLX_DRAWABLE_TYPE, &drawable_type);
        if (!(drawable_type & GLX_PIXMAP_BIT)) continue;

        glXGetFBConfigAttrib(comp->dpy, configs[i], GLX_BIND_TO_TEXTURE_TARGETS_EXT, &tex_tgt);
        if (!(tex_tgt & GLX_TEXTURE_2D_BIT_EXT)) continue;

        glXGetFBConfigAttrib(comp->dpy, configs[i], GLX_DOUBLEBUFFER, &dbl);
        if (dbl) continue;

        glXGetFBConfigAttrib(comp->dpy, configs[i], GLX_BIND_TO_TEXTURE_RGB_EXT, &bind_rgb);
        glXGetFBConfigAttrib(comp->dpy, configs[i], GLX_BIND_TO_TEXTURE_RGBA_EXT, &bind_rgba);
        if (!bind_rgb && !bind_rgba) continue;

        /* Get the depth from this config's visual */
        XVisualInfo *vi = glXGetVisualFromFBConfig(comp->dpy, configs[i]);
        if (!vi) continue;
        int depth = vi->depth;
        XFree(vi);

        if (depth <= 0 || depth >= MAX_DEPTH) continue;
        if (comp->tfp_available[depth]) continue; /* keep first match per depth */

        comp->tfp_config[depth] = configs[i];
        comp->tfp_format[depth] = bind_rgba ? GLX_TEXTURE_FORMAT_RGBA_EXT
                                             : GLX_TEXTURE_FORMAT_RGB_EXT;
        comp->tfp_available[depth] = true;
    }
    XFree(configs);
}

/* ========================================================================== */
/* Initialization                                                             */
/* ========================================================================== */

static int init_compositor(Compositor *comp, const char *shader_path_input) {
    memset(comp, 0, sizeof(*comp));
    comp->running = true;
    clock_gettime(CLOCK_MONOTONIC, &comp->start_time);

    /* Resolve paths */
    comp->shader_dir = get_exe_dir();
    comp->shader_path = resolve_shader_path(comp->shader_dir, shader_path_input);
    fprintf(stderr, "Using shader: %s\n", comp->shader_path);

    /* Open display */
    comp->dpy = XOpenDisplay(NULL);
    if (!comp->dpy) {
        fprintf(stderr, "Cannot open X display\n");
        return -1;
    }

    XSetErrorHandler(x_error_handler);

    comp->screen = DefaultScreen(comp->dpy);
    comp->root = RootWindow(comp->dpy, comp->screen);
    comp->root_width = DisplayWidth(comp->dpy, comp->screen);
    comp->root_height = DisplayHeight(comp->dpy, comp->screen);

    fprintf(stderr, "Screen: %dx%d\n", comp->root_width, comp->root_height);

    /* --- Check extensions --- */
    int ev_base, err_base;

    if (!XCompositeQueryExtension(comp->dpy, &ev_base, &err_base)) {
        fprintf(stderr, "XComposite extension not available\n");
        return -1;
    }
    int major = 0, minor = 0;
    XCompositeQueryVersion(comp->dpy, &major, &minor);
    if (major == 0 && minor < 2) {
        fprintf(stderr, "XComposite >= 0.2 required (have %d.%d)\n", major, minor);
        return -1;
    }

    if (!XDamageQueryExtension(comp->dpy, &comp->damage_event, &comp->damage_error)) {
        fprintf(stderr, "XDamage extension not available\n");
        return -1;
    }

    int xfixes_ev, xfixes_err;
    if (!XFixesQueryExtension(comp->dpy, &xfixes_ev, &xfixes_err)) {
        fprintf(stderr, "XFixes extension not available\n");
        return -1;
    }

    /* --- Redirect subwindows --- */
    /*
     * Use CompositeRedirectAutomatic so the X server continues to draw
     * windows normally (preserving correct input routing / stacking).
     * We still get off-screen pixmaps via XCompositeNameWindowPixmap.
     * Our overlay window covers the root, showing the shaded output.
     */
    XCompositeRedirectSubwindows(comp->dpy, comp->root, CompositeRedirectAutomatic);
    XSync(comp->dpy, False);

    /* --- Get overlay window --- */
    comp->overlay = XCompositeGetOverlayWindow(comp->dpy, comp->root);
    if (!comp->overlay) {
        fprintf(stderr, "Failed to get composite overlay window\n");
        return -1;
    }

    /* Make overlay completely transparent to input */
    XserverRegion region = XFixesCreateRegion(comp->dpy, NULL, 0);
    XFixesSetWindowShapeRegion(comp->dpy, comp->overlay, ShapeInput, 0, 0, region);
    XFixesDestroyRegion(comp->dpy, region);

    /* Ensure no event selection or grabs on overlay */
    XSelectInput(comp->dpy, comp->overlay, 0);

    /* --- Select events on root --- */
    XSelectInput(comp->dpy, comp->root,
                 SubstructureNotifyMask |
                 StructureNotifyMask |
                 ExposureMask);

    /* --- GLX setup --- */
    int fbconfig_attrs[] = {
        GLX_DRAWABLE_TYPE,  GLX_WINDOW_BIT,
        GLX_RENDER_TYPE,    GLX_RGBA_BIT,
        GLX_DOUBLEBUFFER,   True,
        GLX_RED_SIZE,       8,
        GLX_GREEN_SIZE,     8,
        GLX_BLUE_SIZE,      8,
        GLX_ALPHA_SIZE,     8,
        None
    };

    int nconfigs;
    GLXFBConfig *configs = glXChooseFBConfig(comp->dpy, comp->screen,
                                             fbconfig_attrs, &nconfigs);
    if (!configs || nconfigs == 0) {
        fprintf(stderr, "No suitable GLX FBConfig found\n");
        return -1;
    }
    comp->fbconfig = configs[0];
    XFree(configs);

    /* Find FBConfigs for texture_from_pixmap, indexed by depth */
    find_tfp_fbconfigs(comp);

    bool have_any_tfp = false;
    fprintf(stderr, "TFP FBConfigs by depth:");
    for (int d = 0; d < MAX_DEPTH; d++) {
        if (comp->tfp_available[d]) {
            fprintf(stderr, " %d(%s)", d,
                    comp->tfp_format[d] == GLX_TEXTURE_FORMAT_RGBA_EXT ? "RGBA" : "RGB");
            have_any_tfp = true;
        }
    }
    fprintf(stderr, "\n");

    if (!have_any_tfp) {
        fprintf(stderr, "No GLX FBConfig with texture_from_pixmap support\n");
        return -1;
    }

    /* Create GLX context */
    comp->glx_ctx = glXCreateNewContext(comp->dpy, comp->fbconfig,
                                         GLX_RGBA_TYPE, NULL, True);
    if (!comp->glx_ctx) {
        fprintf(stderr, "Failed to create GLX context\n");
        return -1;
    }

    comp->glx_win = glXCreateWindow(comp->dpy, comp->fbconfig, comp->overlay, NULL);
    if (!comp->glx_win) {
        fprintf(stderr, "Failed to create GLX window\n");
        return -1;
    }

    if (!glXMakeCurrent(comp->dpy, comp->glx_win, comp->glx_ctx)) {
        fprintf(stderr, "Failed to make GLX context current\n");
        return -1;
    }

    fprintf(stderr, "OpenGL: %s\n", glGetString(GL_VERSION));
    fprintf(stderr, "Renderer: %s\n", glGetString(GL_RENDERER));

    /* Load GLX extension function pointers */
    comp->glXBindTexImageEXT = (PFNGLXBINDTEXIMAGEEXTPROC)
        glXGetProcAddress((const GLubyte *)"glXBindTexImageEXT");
    comp->glXReleaseTexImageEXT = (PFNGLXRELEASETEXIMAGEEXTPROC)
        glXGetProcAddress((const GLubyte *)"glXReleaseTexImageEXT");

    if (!comp->glXBindTexImageEXT || !comp->glXReleaseTexImageEXT) {
        fprintf(stderr, "GLX_EXT_texture_from_pixmap not available\n");
        return -1;
    }

    /* Enable vsync */
    PFNGLXSWAPINTERVALEXTPROC glXSwapIntervalEXT =
        (PFNGLXSWAPINTERVALEXTPROC)
        glXGetProcAddress((const GLubyte *)"glXSwapIntervalEXT");
    if (glXSwapIntervalEXT) {
        glXSwapIntervalEXT(comp->dpy, comp->glx_win, 1);
    }

    /* --- Create OpenGL resources --- */

    /* FBO */
    glGenTextures(1, &comp->fbo_texture);
    glBindTexture(GL_TEXTURE_2D, comp->fbo_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                 comp->root_width, comp->root_height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, &comp->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, comp->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, comp->fbo_texture, 0);
    GLenum fbo_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (fbo_status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "FBO incomplete: 0x%x\n", fbo_status);
        return -1;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    /* Fullscreen quad */
    float quad[] = {
        /* pos x,y      texcoord u,v */
        -1.0f, -1.0f,   0.0f, 0.0f,
         1.0f, -1.0f,   1.0f, 0.0f,
        -1.0f,  1.0f,   0.0f, 1.0f,
         1.0f,  1.0f,   1.0f, 1.0f,
    };

    glGenVertexArrays(1, &comp->vao);
    glGenBuffers(1, &comp->vbo);
    glBindVertexArray(comp->vao);
    glBindBuffer(GL_ARRAY_BUFFER, comp->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (void *)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);

    /* --- Compile shaders --- */

    /* Vertex shader (kept alive for hot-reload) */
    char *vert_path = resolve_shader_path(comp->shader_dir, "shaders/quad.vert");
    char *vert_src = load_file(vert_path);
    free(vert_path);
    if (!vert_src) return -1;

    comp->vert_shader = compile_shader(GL_VERTEX_SHADER, vert_src, "quad.vert");
    free(vert_src);
    if (!comp->vert_shader) return -1;

    /* Composite shader */
    char *comp_frag_path = resolve_shader_path(comp->shader_dir, "shaders/composite.frag");
    char *comp_frag_src = load_file(comp_frag_path);
    free(comp_frag_path);
    if (!comp_frag_src) return -1;

    GLuint comp_frag = compile_shader(GL_FRAGMENT_SHADER, comp_frag_src, "composite.frag");
    free(comp_frag_src);
    if (!comp_frag) return -1;

    comp->composite_prog = link_program(comp->vert_shader, comp_frag);
    glDeleteShader(comp_frag);
    if (!comp->composite_prog) return -1;

    comp->uc_texture = glGetUniformLocation(comp->composite_prog, "u_texture");

    /* Post-process shader */
    char *pp_src = load_file(comp->shader_path);
    if (!pp_src) return -1;

    GLuint pp_frag = compile_shader(GL_FRAGMENT_SHADER, pp_src, comp->shader_path);
    free(pp_src);
    if (!pp_frag) return -1;

    comp->postproc_prog = link_program(comp->vert_shader, pp_frag);
    glDeleteShader(pp_frag);
    if (!comp->postproc_prog) return -1;

    comp->u_screen_tex = glGetUniformLocation(comp->postproc_prog, "u_screen");
    comp->u_resolution = glGetUniformLocation(comp->postproc_prog, "u_resolution");
    comp->u_time       = glGetUniformLocation(comp->postproc_prog, "u_time");

    /* --- Enumerate existing windows --- */
    Window root_ret, parent_ret;
    Window *children = NULL;
    unsigned int nchildren = 0;

    if (XQueryTree(comp->dpy, comp->root, &root_ret, &parent_ret,
                   &children, &nchildren)) {
        for (unsigned int i = 0; i < nchildren; i++) {
            XWindowAttributes attr;
            if (!XGetWindowAttributes(comp->dpy, children[i], &attr)) continue;
            if (attr.map_state != IsViewable) continue;
            if (children[i] == comp->overlay) continue;

            WinEntry *w = add_win(comp, children[i]);
            if (!w) continue;

            w->x = attr.x;
            w->y = attr.y;
            w->width = attr.width;
            w->height = attr.height;
            w->border_width = attr.border_width;
            w->override_redirect = attr.override_redirect;
            w->mapped = true;

            g_last_xerror = 0;
            w->damage = XDamageCreate(comp->dpy, w->xid, XDamageReportNonEmpty);
            XSync(comp->dpy, False);
            if (g_last_xerror) {
                w->damage = 0;
                g_last_xerror = 0;
            }
            bind_window_pixmap(comp, w);
        }
        if (children) XFree(children);
    }

    comp->needs_redraw = true;
    fprintf(stderr, "Compositor initialized, entering main loop\n");
    return 0;
}

/* ========================================================================== */
/* Cleanup                                                                    */
/* ========================================================================== */

static void cleanup_compositor(Compositor *comp) {
    fprintf(stderr, "Cleaning up...\n");

    /* Remove all windows */
    WinEntry *w = comp->win_head;
    while (w) {
        WinEntry *next = w->next;
        if (w->damage) XDamageDestroy(comp->dpy, w->damage);
        unbind_window_pixmap(comp, w);
        free(w);
        w = next;
    }
    comp->win_head = comp->win_tail = NULL;

    /* Delete GL resources */
    if (comp->composite_prog) glDeleteProgram(comp->composite_prog);
    if (comp->postproc_prog) glDeleteProgram(comp->postproc_prog);
    if (comp->vert_shader) glDeleteShader(comp->vert_shader);
    if (comp->fbo) glDeleteFramebuffers(1, &comp->fbo);
    if (comp->fbo_texture) glDeleteTextures(1, &comp->fbo_texture);
    if (comp->vbo) glDeleteBuffers(1, &comp->vbo);
    if (comp->vao) glDeleteVertexArrays(1, &comp->vao);

    /* Destroy GLX */
    if (comp->glx_ctx) {
        glXMakeCurrent(comp->dpy, None, NULL);
        if (comp->glx_win) glXDestroyWindow(comp->dpy, comp->glx_win);
        glXDestroyContext(comp->dpy, comp->glx_ctx);
    }

    /* Release compositor resources */
    if (comp->dpy) {
        XCompositeUnredirectSubwindows(comp->dpy, comp->root, CompositeRedirectAutomatic);
        XCompositeReleaseOverlayWindow(comp->dpy, comp->root);
        XSync(comp->dpy, False);
        XCloseDisplay(comp->dpy);
    }

    free(comp->shader_path);
    free(comp->shader_dir);

    fprintf(stderr, "Cleanup complete\n");
}

/* ========================================================================== */
/* Main                                                                       */
/* ========================================================================== */

int main(int argc, char *argv[]) {
    const char *shader_input = "shaders/crt.frag";
    if (argc > 1) {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
            fprintf(stderr,
                "Usage: %s [shader.frag]\n"
                "  Default shader: shaders/crt.frag\n"
                "  Send SIGUSR1 to hot-reload the shader.\n"
                "  Send SIGINT/SIGTERM to stop.\n",
                argv[0]);
            return 0;
        }
        shader_input = argv[1];
    }

    /* Install signal handlers */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));

    sa.sa_handler = handle_signal;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    sa.sa_handler = handle_sigusr1;
    sigaction(SIGUSR1, &sa, NULL);

    /* Initialize compositor */
    Compositor comp;
    if (init_compositor(&comp, shader_input) != 0) {
        fprintf(stderr, "Failed to initialize compositor\n");
        cleanup_compositor(&comp);
        return 1;
    }

    /* Main loop */
    while (g_running) {
        /* Process all pending X events */
        while (XPending(comp.dpy) > 0) {
            XEvent ev;
            XNextEvent(comp.dpy, &ev);
            handle_event(&comp, &ev);
        }

        /* Check for shader reload */
        if (g_reload) {
            g_reload = 0;
            reload_postproc_shader(&comp);
            comp.needs_redraw = true;
        }

        /* Check for param file changes (~every 30 frames / 0.5s) */
        if (++comp.param_check_ctr >= 30) {
            comp.param_check_ctr = 0;
            read_params(&comp);
        }

        /* Render */
        if (comp.needs_redraw) {
            render_frame(&comp);
            glXSwapBuffers(comp.dpy, comp.glx_win);
            comp.needs_redraw = false;
        }

        /* Poll for next event or timeout (16ms for ~60fps animation) */
        struct pollfd pfd = {
            .fd = ConnectionNumber(comp.dpy),
            .events = POLLIN
        };
        poll(&pfd, 1, 16);

        /* Always mark redraw for animated shaders */
        comp.needs_redraw = true;
    }

    cleanup_compositor(&comp);
    return 0;
}
