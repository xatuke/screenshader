/*
 * screenshader-preview - Shader preview renderer
 *
 * Two modes:
 *   Single-shot: capture screen → apply shader → write PPM to stdout
 *   Live:        open a window with continuous screen capture + shader at ~5fps
 *
 * Usage:
 *   screenshader-preview <shader.frag>                          # single PPM
 *   screenshader-preview <shader.frag> --live [--fps N]         # live window
 *   screenshader-preview --screenshot-only                      # raw screenshot
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glx.h>
#include <GL/glext.h>

static volatile sig_atomic_t g_running = 1;

static void sig_handler(int sig) {
    (void)sig;
    g_running = 0;
}

/* ========================================================================== */
/* Vertex shader (embedded)                                                   */
/* ========================================================================== */

static const char *QUAD_VERT_SRC =
    "#version 330 core\n"
    "layout(location = 0) in vec2 a_position;\n"
    "layout(location = 1) in vec2 a_texcoord;\n"
    "out vec2 v_texcoord;\n"
    "void main() {\n"
    "    gl_Position = vec4(a_position, 0.0, 1.0);\n"
    "    v_texcoord = a_texcoord;\n"
    "}\n";

/* ========================================================================== */
/* Utilities (same as screenshader.c)                                         */
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
/* Screen capture (BGRA XImage → texture upload)                              */
/* ========================================================================== */

/* Capture root window into GL texture. */
static int capture_to_texture(Display *dpy, GLuint tex, int scr_w, int scr_h) {
    Window root = DefaultRootWindow(dpy);
    XImage *img = XGetImage(dpy, root, 0, 0, scr_w, scr_h, AllPlanes, ZPixmap);
    if (!img) return -1;

    glBindTexture(GL_TEXTURE_2D, tex);
    if (img->bits_per_pixel == 32) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, scr_w, scr_h,
                        GL_BGRA, GL_UNSIGNED_BYTE, img->data);
    } else {
        unsigned char *rgb = malloc(scr_w * scr_h * 3);
        for (int y = 0; y < scr_h; y++)
            for (int x = 0; x < scr_w; x++) {
                unsigned long p = XGetPixel(img, x, y);
                int i = (y * scr_w + x) * 3;
                rgb[i+0] = (p >> 16) & 0xFF;
                rgb[i+1] = (p >> 8) & 0xFF;
                rgb[i+2] = p & 0xFF;
            }
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, scr_w, scr_h,
                        GL_RGB, GL_UNSIGNED_BYTE, rgb);
        free(rgb);
    }

    XDestroyImage(img);
    return 0;
}

/* Capture root window to an RGB malloc'd buffer (for single-shot PPM mode) */
static unsigned char *capture_screen_rgb(Display *dpy, int *out_w, int *out_h) {
    int screen = DefaultScreen(dpy);
    *out_w = DisplayWidth(dpy, screen);
    *out_h = DisplayHeight(dpy, screen);

    XImage *img = XGetImage(dpy, DefaultRootWindow(dpy), 0, 0,
                            *out_w, *out_h, AllPlanes, ZPixmap);
    if (!img) { fprintf(stderr, "XGetImage failed\n"); return NULL; }

    unsigned char *rgb = malloc(*out_w * *out_h * 3);
    if (!rgb) { XDestroyImage(img); return NULL; }

    if (img->bits_per_pixel == 32 && img->byte_order == LSBFirst) {
        for (int y = 0; y < *out_h; y++) {
            const unsigned char *row = (unsigned char *)img->data + y * img->bytes_per_line;
            unsigned char *out = rgb + y * *out_w * 3;
            for (int x = 0; x < *out_w; x++) {
                out[x*3+0] = row[x*4+2];
                out[x*3+1] = row[x*4+1];
                out[x*3+2] = row[x*4+0];
            }
        }
    } else {
        for (int y = 0; y < *out_h; y++)
            for (int x = 0; x < *out_w; x++) {
                unsigned long p = XGetPixel(img, x, y);
                int i = (y * *out_w + x) * 3;
                rgb[i+0] = (p >> 16) & 0xFF;
                rgb[i+1] = (p >> 8) & 0xFF;
                rgb[i+2] = p & 0xFF;
            }
    }
    XDestroyImage(img);
    return rgb;
}

/* ========================================================================== */
/* Image scaling (bilinear)                                                   */
/* ========================================================================== */

static unsigned char *scale_rgb(const unsigned char *src,
                                int sw, int sh, int dw, int dh) {
    unsigned char *dst = malloc(dw * dh * 3);
    if (!dst) return NULL;
    for (int dy = 0; dy < dh; dy++) {
        float sy = (float)dy / (float)dh * (float)sh;
        int y0 = (int)sy, y1 = y0 + 1 < sh ? y0 + 1 : y0;
        float fy = sy - (float)y0;
        for (int dx = 0; dx < dw; dx++) {
            float sx = (float)dx / (float)dw * (float)sw;
            int x0 = (int)sx, x1 = x0 + 1 < sw ? x0 + 1 : x0;
            float fx = sx - (float)x0;
            for (int c = 0; c < 3; c++) {
                float v = src[(y0*sw+x0)*3+c] * (1-fx)*(1-fy)
                        + src[(y0*sw+x1)*3+c] * fx*(1-fy)
                        + src[(y1*sw+x0)*3+c] * (1-fx)*fy
                        + src[(y1*sw+x1)*3+c] * fx*fy;
                dst[(dy*dw+dx)*3+c] = (unsigned char)(v + 0.5f);
            }
        }
    }
    return dst;
}

/* ========================================================================== */
/* PPM I/O                                                                    */
/* ========================================================================== */

static void write_ppm_stdout(const unsigned char *rgb, int w, int h) {
    fprintf(stdout, "P6\n%d %d\n255\n", w, h);
    fwrite(rgb, 1, w * h * 3, stdout);
    fflush(stdout);
}

static unsigned char *read_ppm_stdin(int *out_w, int *out_h) {
    char magic[3];
    if (fscanf(stdin, "%2s", magic) != 1 || strcmp(magic, "P6") != 0) {
        fprintf(stderr, "Invalid PPM: expected P6 magic\n"); return NULL;
    }
    int c;
    while ((c = fgetc(stdin)) != EOF) {
        if (c == '#') { while ((c = fgetc(stdin)) != EOF && c != '\n'); }
        else if (c > ' ') { ungetc(c, stdin); break; }
    }
    int w, h, maxval;
    if (fscanf(stdin, "%d %d %d", &w, &h, &maxval) != 3) {
        fprintf(stderr, "Invalid PPM header\n"); return NULL;
    }
    fgetc(stdin);
    unsigned char *rgb = malloc(w * h * 3);
    if (!rgb) return NULL;
    size_t n = fread(rgb, 1, w * h * 3, stdin);
    if ((int)n != w * h * 3) { free(rgb); return NULL; }
    *out_w = w; *out_h = h;
    return rgb;
}

/* ========================================================================== */
/* GLX pbuffer (for single-shot mode)                                         */
/* ========================================================================== */

static int create_pbuffer_context(Display *dpy, int w, int h,
                                  GLXContext *ctx_out, GLXPbuffer *pbuf_out) {
    int fb_attrs[] = {
        GLX_DRAWABLE_TYPE, GLX_PBUFFER_BIT,
        GLX_RENDER_TYPE,   GLX_RGBA_BIT,
        GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8,
        GLX_BLUE_SIZE, 8, GLX_ALPHA_SIZE, 8,
        None
    };
    int nc;
    GLXFBConfig *cfgs = glXChooseFBConfig(dpy, DefaultScreen(dpy), fb_attrs, &nc);
    if (!cfgs || nc == 0) { fprintf(stderr, "No pbuffer FBConfig\n"); return -1; }

    int pa[] = { GLX_PBUFFER_WIDTH, w, GLX_PBUFFER_HEIGHT, h, None };
    *pbuf_out = glXCreatePbuffer(dpy, cfgs[0], pa);
    *ctx_out = glXCreateNewContext(dpy, cfgs[0], GLX_RGBA_TYPE, NULL, True);
    XFree(cfgs);
    if (!*pbuf_out || !*ctx_out) return -1;
    if (!glXMakeCurrent(dpy, *pbuf_out, *ctx_out)) return -1;
    return 0;
}

/* ========================================================================== */
/* Shared: setup fullscreen quad + compile shader program                     */
/* ========================================================================== */

static void setup_quad(GLuint *vao, GLuint *vbo) {
    /* Texcoords flipped: v=1 at bottom, v=0 at top.
     * XGetImage stores row 0 at top, GL texcoord 0 is bottom,
     * so we flip to get the correct orientation. */
    float quad[] = {
        -1, -1,  0, 1,
         1, -1,  1, 1,
        -1,  1,  0, 0,
         1,  1,  1, 0,
    };
    glGenVertexArrays(1, vao);
    glGenBuffers(1, vbo);
    glBindVertexArray(*vao);
    glBindBuffer(GL_ARRAY_BUFFER, *vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void *)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void *)8);
    glEnableVertexAttribArray(1);
}

static GLuint build_program(const char *shader_path) {
    GLuint vert = compile_shader(GL_VERTEX_SHADER, QUAD_VERT_SRC, "quad.vert");
    if (!vert) return 0;
    char *frag_src = load_file(shader_path);
    if (!frag_src) return 0;
    GLuint frag = compile_shader(GL_FRAGMENT_SHADER, frag_src, shader_path);
    free(frag_src);
    if (!frag) return 0;
    GLuint prog = link_program(vert, frag);
    glDeleteShader(vert);
    glDeleteShader(frag);
    return prog;
}

/* ========================================================================== */
/* Single-shot render (pbuffer → PPM stdout)                                  */
/* ========================================================================== */

static unsigned char *render_single(Display *dpy, const char *shader_path,
                                    const unsigned char *input_rgb, int w, int h) {
    GLXContext ctx; GLXPbuffer pbuf;
    if (create_pbuffer_context(dpy, w, h, &ctx, &pbuf) < 0) return NULL;

    GLuint prog = build_program(shader_path);
    if (!prog) return NULL;

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, input_rgb);

    GLuint vao, vbo;
    setup_quad(&vao, &vbo);

    glViewport(0, 0, w, h);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(prog);

    GLint loc;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    if ((loc = glGetUniformLocation(prog, "u_screen")) >= 0) glUniform1i(loc, 0);
    if ((loc = glGetUniformLocation(prog, "u_resolution")) >= 0)
        glUniform2f(loc, (float)w, (float)h);
    if ((loc = glGetUniformLocation(prog, "u_time")) >= 0) glUniform1f(loc, 0.5f);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glFinish();

    unsigned char *pixels = malloc(w * h * 3);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, pixels);

    /* glReadPixels returns bottom-up, PPM needs top-down → flip rows */
    unsigned char *tmp = malloc(w * 3);
    for (int y = 0; y < h / 2; y++) {
        unsigned char *a = pixels + y * w * 3;
        unsigned char *b = pixels + (h - 1 - y) * w * 3;
        memcpy(tmp, a, w * 3); memcpy(a, b, w * 3); memcpy(b, tmp, w * 3);
    }
    free(tmp);

    glDeleteTextures(1, &tex);
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(prog);
    glXMakeCurrent(dpy, None, NULL);
    glXDestroyPbuffer(dpy, pbuf);
    glXDestroyContext(dpy, ctx);
    return pixels;
}

/* ========================================================================== */
/* Live preview window                                                        */
/* ========================================================================== */

static int run_live(Display *dpy, const char *shader_path, int fps) {
    int screen = DefaultScreen(dpy);
    int scr_w = DisplayWidth(dpy, screen);
    int scr_h = DisplayHeight(dpy, screen);

    /* Preview window at half screen size */
    int win_w = scr_w / 2;
    int win_h = scr_h / 2;

    /* Choose FBConfig for the window */
    int fb_attrs[] = {
        GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
        GLX_RENDER_TYPE,   GLX_RGBA_BIT,
        GLX_DOUBLEBUFFER,  True,
        GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8,
        GLX_BLUE_SIZE, 8,
        None
    };
    int nc;
    GLXFBConfig *cfgs = glXChooseFBConfig(dpy, screen, fb_attrs, &nc);
    if (!cfgs || nc == 0) {
        fprintf(stderr, "No suitable FBConfig for window\n");
        return 1;
    }

    XVisualInfo *vi = glXGetVisualFromFBConfig(dpy, cfgs[0]);
    if (!vi) { fprintf(stderr, "No visual\n"); return 1; }

    /* Create window */
    XSetWindowAttributes swa;
    swa.colormap = XCreateColormap(dpy, RootWindow(dpy, screen), vi->visual, AllocNone);
    swa.event_mask = StructureNotifyMask | KeyPressMask | ExposureMask;

    Window win = XCreateWindow(dpy, RootWindow(dpy, screen),
                               scr_w / 4, scr_h / 4, win_w, win_h, 0,
                               vi->depth, InputOutput, vi->visual,
                               CWColormap | CWEventMask, &swa);

    /* Set window title */
    char title[256];
    snprintf(title, sizeof(title), "screenshader preview - %s", shader_path);
    XStoreName(dpy, win, title);

    /* Handle window close */
    Atom wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete, 1);

    XFree(vi);

    /* GLX context (created before window is mapped so we can capture first) */
    GLXContext ctx = glXCreateNewContext(dpy, cfgs[0], GLX_RGBA_TYPE, NULL, True);
    XFree(cfgs);
    if (!ctx) { fprintf(stderr, "Failed to create GLX context\n"); return 1; }
    glXMakeCurrent(dpy, win, ctx);

    /* Compile shader */
    GLuint prog = build_program(shader_path);
    if (!prog) return 1;

    /* Screen capture texture — capture ONCE before showing the window */
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, scr_w, scr_h, 0,
                 GL_BGRA, GL_UNSIGNED_BYTE, NULL);

    /* Capture desktop BEFORE showing window (so it can't capture itself) */
    capture_to_texture(dpy, tex, scr_w, scr_h);

    /* Now show the window */
    XMapWindow(dpy, win);
    XSync(dpy, False);

    /* Track window position for move-based re-capture */
    int win_x = scr_w / 4, win_y = scr_h / 4;

    /* Fullscreen quad */
    GLuint vao, vbo;
    setup_quad(&vao, &vbo);

    /* Uniform locations */
    GLint u_screen_loc = glGetUniformLocation(prog, "u_screen");
    GLint u_res_loc = glGetUniformLocation(prog, "u_resolution");
    GLint u_time_loc = glGetUniformLocation(prog, "u_time");

    struct timespec start_ts;
    clock_gettime(CLOCK_MONOTONIC, &start_ts);

    long render_ns = 1000000000L / fps;      /* render at full fps */
    long capture_ns = 2000000000L;            /* re-capture every 2s */
    struct timespec last_capture = start_ts;

    fprintf(stderr, "Live preview: %s @ %d fps (R=refresh, Q/Esc=quit)\n",
            shader_path, fps);

    while (g_running) {
        /* Handle X events */
        int do_refresh = 0;
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            if (ev.type == ClientMessage &&
                (Atom)ev.xclient.data.l[0] == wm_delete) {
                g_running = 0;
            } else if (ev.type == KeyPress) {
                KeySym key = XLookupKeysym(&ev.xkey, 0);
                if (key == XK_q || key == XK_Escape)
                    g_running = 0;
                else if (key == XK_r || key == XK_R)
                    do_refresh = 1;
            } else if (ev.type == ConfigureNotify) {
                win_x = ev.xconfigure.x;
                win_y = ev.xconfigure.y;
                win_w = ev.xconfigure.width;
                win_h = ev.xconfigure.height;
            }
        }
        if (!g_running) break;

        /* Re-capture desktop periodically or on R key.
         * Moves window off-screen momentarily to avoid self-capture.
         * XMoveWindow is much faster than unmap/remap. */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long since_capture = (now.tv_sec - last_capture.tv_sec) * 1000000000L
                           + (now.tv_nsec - last_capture.tv_nsec);
        if (do_refresh || since_capture >= capture_ns) {
            XMoveWindow(dpy, win, -10000, -10000);
            XSync(dpy, False);
            struct timespec wait = { 0, 50000000L }; /* 50ms for compositor */
            nanosleep(&wait, NULL);
            capture_to_texture(dpy, tex, scr_w, scr_h);
            XMoveWindow(dpy, win, win_x, win_y);
            XSync(dpy, False);
            last_capture = now;
        }

        /* Render shader with animated u_time */
        glViewport(0, 0, win_w, win_h);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(prog);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        if (u_screen_loc >= 0) glUniform1i(u_screen_loc, 0);
        if (u_res_loc >= 0) glUniform2f(u_res_loc, (float)scr_w, (float)scr_h);
        if (u_time_loc >= 0) {
            float t = (float)(now.tv_sec - start_ts.tv_sec)
                    + (float)(now.tv_nsec - start_ts.tv_nsec) / 1e9f;
            glUniform1f(u_time_loc, t);
        }

        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glXSwapBuffers(dpy, win);

        /* Frame rate limiter */
        struct timespec sleep_ts = { 0, render_ns };
        nanosleep(&sleep_ts, NULL);
    }

    /* Cleanup */
    glDeleteTextures(1, &tex);
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(prog);
    glXMakeCurrent(dpy, None, NULL);
    glXDestroyContext(dpy, ctx);
    XDestroyWindow(dpy, win);

    return 0;
}

/* ========================================================================== */
/* Main                                                                       */
/* ========================================================================== */

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage:\n"
        "  %s <shader.frag> [--width W] [--height H] [--input-ppm]\n"
        "  %s <shader.frag> --live [--fps N]\n"
        "  %s --screenshot-only [--width W] [--height H]\n",
        prog, prog, prog);
}

int main(int argc, char *argv[]) {
    const char *shader_path = NULL;
    int target_w = 0, target_h = 0;
    bool screenshot_only = false;
    bool input_ppm = false;
    bool live = false;
    int fps = 30;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--screenshot-only") == 0) screenshot_only = true;
        else if (strcmp(argv[i], "--input-ppm") == 0) input_ppm = true;
        else if (strcmp(argv[i], "--live") == 0) live = true;
        else if (strcmp(argv[i], "--fps") == 0 && i+1 < argc) fps = atoi(argv[++i]);
        else if (strcmp(argv[i], "--width") == 0 && i+1 < argc) target_w = atoi(argv[++i]);
        else if (strcmp(argv[i], "--height") == 0 && i+1 < argc) target_h = atoi(argv[++i]);
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]); return 0;
        } else if (argv[i][0] != '-' && !shader_path) shader_path = argv[i];
        else { fprintf(stderr, "Unknown: %s\n", argv[i]); usage(argv[0]); return 1; }
    }

    if (!screenshot_only && !shader_path) {
        fprintf(stderr, "Error: no shader specified\n");
        usage(argv[0]); return 1;
    }

    if (fps < 1) fps = 1;
    if (fps > 60) fps = 60;

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    /* ---- Live mode ---- */
    if (live) {
        Display *dpy = XOpenDisplay(NULL);
        if (!dpy) { fprintf(stderr, "Cannot open display\n"); return 1; }
        int ret = run_live(dpy, shader_path, fps);
        XCloseDisplay(dpy);
        return ret;
    }

    /* ---- Single-shot mode ---- */
    unsigned char *rgb = NULL;
    int img_w = 0, img_h = 0;

    if (input_ppm) {
        rgb = read_ppm_stdin(&img_w, &img_h);
        if (!rgb) return 1;
    } else {
        Display *dpy = XOpenDisplay(NULL);
        if (!dpy) { fprintf(stderr, "Cannot open display\n"); return 1; }
        rgb = capture_screen_rgb(dpy, &img_w, &img_h);
        XCloseDisplay(dpy);
        if (!rgb) return 1;
    }

    if (target_w <= 0 || target_h <= 0) {
        target_w = img_w;
        target_h = img_h;
    }

    if (img_w != target_w || img_h != target_h) {
        unsigned char *s = scale_rgb(rgb, img_w, img_h, target_w, target_h);
        free(rgb); rgb = s;
        if (!rgb) return 1;
        img_w = target_w; img_h = target_h;
    }

    if (screenshot_only) {
        write_ppm_stdout(rgb, img_w, img_h);
        free(rgb);
        return 0;
    }

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "Cannot open display\n"); free(rgb); return 1; }
    unsigned char *result = render_single(dpy, shader_path, rgb, img_w, img_h);
    free(rgb);
    XCloseDisplay(dpy);
    if (!result) return 1;

    write_ppm_stdout(result, img_w, img_h);
    free(result);
    return 0;
}
