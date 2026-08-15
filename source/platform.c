/* platform.c -- Switch platform layer for the PVZ2 port.
 *
 * Implements the pieces main.c factors out: the SO load buffer, the GLES2/EGL
 * context (recreated on dock/undock so docked runs 1080p and handheld 720p),
 * and input -- handheld touchscreen fingers plus a docked left-stick cursor
 * with A as the tap "finger", both delivered as unified PtrEvents.
 *
 * MIT license -- see LICENSE.
 */

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <stdint.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "config.h"
#include "error.h"
#include "platform.h"
#include "util.h"

#ifndef PVZ2_ENABLE_GL_TRACE
#define PVZ2_ENABLE_GL_TRACE 1
#endif

#ifndef PVZ2_ENABLE_TOUCH_TRACE
#define PVZ2_ENABLE_TOUCH_TRACE 1
#endif

/* ===================== SO load zone ======================================= *
 * so_load() uses this as the RW buffer it assembles the mapped image into
 * (then maps to executable virtmem itself). It must be >= the .so's aligned
 * load size and persist for the process lifetime. A page-aligned allocation
 * from the (large) applet heap is all that's needed; if you ever run out of
 * memory for textures, add a __libnx_initheap override that claims full RAM. */
static void  *g_so_base = NULL;
static size_t g_so_size = 0;

void *heap_so_base(void) {
  if (!g_so_base) {
    g_so_size = (size_t)SO_ZONE_MB * 1024 * 1024;
    g_so_base = memalign(0x1000, g_so_size);
    if (!g_so_base) debugPrintf("heap_so_base: memalign(%zu) failed\n", g_so_size);
  }
  return g_so_base;
}
size_t heap_so_limit(void) { heap_so_base(); return g_so_size; }

/* ===================== GLES2 / EGL ======================================== */
static EGLDisplay g_egl_display = EGL_NO_DISPLAY;
static EGLSurface g_egl_surface = EGL_NO_SURFACE;
static EGLContext g_egl_context = EGL_NO_CONTEXT;

/* The main loop presents through egl_swap_buffers(), not the game's imported
 * eglSwapBuffers symbol. Keep the frame tracer on that real presentation path. */
void gl_frame_report(void);
void cursor_draw(void);

void egl_init_context(void) {
  static const EGLint config_attribs[] = {
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
    EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
    EGL_NONE
  };
  static const EGLint context_attribs[] = {
    EGL_CONTEXT_CLIENT_VERSION, 2,
    EGL_NONE
  };
  EGLConfig config;
  EGLint count;
  NWindow *window = nwindowGetDefault();

  nwindowSetDimensions(window, screen_width, screen_height);
  nwindowSetCrop(window, 0, 0, screen_width, screen_height);
  nwindowSetTransform(window, 0u);
  g_egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (g_egl_display == EGL_NO_DISPLAY || !eglInitialize(g_egl_display, NULL, NULL) ||
      !eglBindAPI(EGL_OPENGL_ES_API) ||
      !eglChooseConfig(g_egl_display, config_attribs, &config, 1, &count) || !count)
    fatal_error("Could not initialize EGL (0x%x).", eglGetError());
  g_egl_surface = eglCreateWindowSurface(g_egl_display, config, window, NULL);
  g_egl_context = eglCreateContext(g_egl_display, config, EGL_NO_CONTEXT,
                                   context_attribs);
  if (g_egl_surface == EGL_NO_SURFACE || g_egl_context == EGL_NO_CONTEXT ||
      !eglMakeCurrent(g_egl_display, g_egl_surface, g_egl_surface, g_egl_context))
    fatal_error("Could not create the GLES2 context (0x%x).", eglGetError());
  eglSwapInterval(g_egl_display, 1);
  debugPrintf("egl: GLES2 context ready (%dx%d)\n", screen_width, screen_height);
}
void egl_swap_buffers(void) {
  if (PVZ2_ENABLE_GL_TRACE) gl_frame_report();
  cursor_draw();
  eglSwapBuffers(g_egl_display, g_egl_surface);
}
void egl_exit_context(void) {
  if (g_egl_display == EGL_NO_DISPLAY) return;
  eglMakeCurrent(g_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (g_egl_context != EGL_NO_CONTEXT) eglDestroyContext(g_egl_display, g_egl_context);
  if (g_egl_surface != EGL_NO_SURFACE) eglDestroySurface(g_egl_display, g_egl_surface);
  eglTerminate(g_egl_display);
}

/* ===================== Input ============================================== */
static PadState s_pad;
static int      s_pad_ready = 0;

/* The Android game treats its working mouse/controller path as one stable
 * cursor pointer.  Use the same pointer identity for the physical touchscreen
 * instead of forwarding Horizon's raw finger ids.  Secondary contacts are
 * ignored while the primary gesture is active, which prevents a second finger
 * from accidentally completing a gesture the first finger began. */
#define CURSOR_PTR_ID 8
#define TOUCH_INVALID_FINGER_ID UINT32_MAX

static int   s_touch_active;
static u32   s_touch_finger_id = TOUCH_INVALID_FINGER_ID;
static float s_touch_x, s_touch_y;
static float s_touch_previous_x, s_touch_previous_y;

/* Virtual cursor -- available in BOTH handheld and docked.
 *   '+'  shows it
 *   '-'  hides it
 *   'A'  taps at the cursor
 * In handheld the touchscreen stays live at the same time, so you can use
 * either (or both). Docked has no touchscreen, so the cursor starts visible
 * there; in handheld it starts hidden since touch is the natural input. */
static float s_cur_x, s_cur_y;
static int   s_cur_down_prev;
static int   s_cursor_visible = -1;   /* -1 = not yet initialised */
static int   s_was_docked = -1;

static void ensure_pad(void) {
  if (s_pad_ready) return;
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&s_pad);
  hidInitializeTouchScreen();
  s_cur_x = screen_width  * 0.5f;
  s_cur_y = screen_height * 0.5f;
  s_pad_ready = 1;
}

static int is_docked(void) {
  return appletGetOperationMode() == AppletOperationMode_Console;
}

int is_switch_handheld(void) {
  return !is_docked();
}

/* Collected once per frame by padUpdate_all(), drained by platform_poll_pointers. */
static PtrEvent s_events[16];
static int      s_nevents;
/* Keep a lightweight multi-frame GL trace window; framebuffer readback is
 * disabled in normal gameplay builds. */
static int      s_capture_frames_remaining = 0;

void trigger_touch_draw_capture(int frames) {
  if (frames > s_capture_frames_remaining)
    s_capture_frames_remaining = frames;
}

void request_debug_framebuffer_capture(const char *basename, int trace_frames) {
  (void)basename;
  if (trace_frames < 1) trace_frames = 1;
  if (s_capture_frames_remaining < trace_frames)
    s_capture_frames_remaining = trace_frames;
}

void request_hud_framebuffer_capture(void) {
  request_debug_framebuffer_capture(NULL, 3);
}

/* The Switch touch panel always reports in its native 1280x720 space.  Map
 * that panel to the current Android View dimensions; handheld is 1:1 while a
 * configured mode can still choose a different presentation size. */
#define NX_TOUCH_PANEL_W 1280.0f
#define NX_TOUCH_PANEL_H 720.0f

/* Appends to s_events.  A touch gesture intentionally mirrors the proven
 * controller cursor contract: one stable pointer id, DOWN/MOVE/UP in order,
 * and the previous sample preserved on release.  Return non-zero for every
 * frame that must suppress controller-cursor events, including the UP frame. */
static int collect_touch_events(void) {
  HidTouchScreenState ts = {0};
  hidGetTouchScreenStates(&ts, 1);
  const int was_active = s_touch_active;
  int emitted = 0;
  int match = -1;

  if (s_touch_active) {
    for (int i = 0; i < ts.count; i++) {
      if (ts.touches[i].finger_id == s_touch_finger_id) {
        match = i;
        break;
      }
    }
  } else if (ts.count > 0) {
    /* Choose one primary contact and hold that identity until its UP. */
    match = 0;
    s_touch_active = 1;
    s_touch_finger_id = ts.touches[0].finger_id;
  }

  const float sx = (float)screen_width  / NX_TOUCH_PANEL_W;
  const float sy = (float)screen_height / NX_TOUCH_PANEL_H;

  if (s_touch_active && match >= 0) {
    float x = (float)ts.touches[match].x * sx;
    float y = (float)ts.touches[match].y * sy;
    if (x < 0) x = 0;
    if (x > screen_width - 1) x = (float)(screen_width - 1);
    if (y < 0) y = 0;
    if (y > screen_height - 1) y = (float)(screen_height - 1);

    const int phase = was_active ? PTR_MOVE : PTR_DOWN;
    const float previous_x = was_active ? s_touch_x : x;
    const float previous_y = was_active ? s_touch_y : y;
    if (s_nevents < 16) {
      PtrEvent *e = &s_events[s_nevents++];
      e->id = CURSOR_PTR_ID;
      e->x = x;
      e->y = y;
      e->previous_x = previous_x;
      e->previous_y = previous_y;
      e->phase = phase;
      emitted = 1;
    }

    s_touch_previous_x = previous_x;
    s_touch_previous_y = previous_y;
    s_touch_x = x;
    s_touch_y = y;
    s_cur_x = x;
    s_cur_y = y;

  } else if (was_active) {
    /* Do not transfer an in-progress gesture to another contact.  End this
     * pointer first; a still-held secondary contact may begin a new gesture on
     * the following frame. */
    if (s_nevents < 16) {
      PtrEvent *e = &s_events[s_nevents++];
      e->id = CURSOR_PTR_ID;
      e->x = s_touch_x;
      e->y = s_touch_y;
      e->previous_x = s_touch_previous_x;
      e->previous_y = s_touch_previous_y;
      e->phase = PTR_UP;
      emitted = 1;
    }
    s_touch_active = 0;
    s_touch_finger_id = TOUCH_INVALID_FINGER_ID;
  }

  return was_active || s_touch_active || emitted;
}

/* Appends to s_events. Left stick moves the cursor, A is the tap. */
static void collect_cursor_events(void) {
  const float previous_x = s_cur_x, previous_y = s_cur_y;
  HidAnalogStickState ls = padGetStickPos(&s_pad, 0);
  const float SPEED = 14.0f;           /* px per frame at full deflection */
  s_cur_x += (ls.x / 32767.0f) * SPEED;
  s_cur_y -= (ls.y / 32767.0f) * SPEED;
  /* clamp to the last valid pixel, not one past it (screen_width would be an
   * off-screen column and the engine's hit-testing would miss the edge) */
  if (s_cur_x < 0) s_cur_x = 0;
  if (s_cur_x > screen_width  - 1) s_cur_x = (float)(screen_width  - 1);
  if (s_cur_y < 0) s_cur_y = 0;
  if (s_cur_y > screen_height - 1) s_cur_y = (float)(screen_height - 1);

  int down = (padGetButtons(&s_pad) & HidNpadButton_A) ? 1 : 0;
  int phase = 0;
  if (down && !s_cur_down_prev)      phase = PTR_DOWN;
  else if (down && s_cur_down_prev)  phase = PTR_MOVE;
  else if (!down && s_cur_down_prev) phase = PTR_UP;
  s_cur_down_prev = down;
  if (phase && s_nevents < 16) {
    PtrEvent *e = &s_events[s_nevents++];
    e->id = CURSOR_PTR_ID; e->x = s_cur_x; e->y = s_cur_y; e->phase = phase;
    e->previous_x = previous_x; e->previous_y = previous_y;
  }
}

void padUpdate_all(void) {
  ensure_pad();
  padUpdate(&s_pad);

  const int docked = is_docked();

  /* First run, and whenever we dock/undock: docked has no touchscreen, so make
   * sure the cursor is up there; handheld defaults to touch. */
  if (docked != s_was_docked) {
    if (s_cursor_visible < 0 || docked)
      s_cursor_visible = docked ? 1 : 0;
    s_was_docked = docked;
  }

  /* '+' shows the cursor, '-' hides it -- in both modes. */
  const u64 pressed = padGetButtonsDown(&s_pad);
  const u64 held = padGetButtons(&s_pad);
  if (pressed & HidNpadButton_Plus)  s_cursor_visible = 1;
  if (pressed & HidNpadButton_Minus) s_cursor_visible = 0;
  s_nevents = 0;
  const int touch_busy = !docked ? collect_touch_events() : 0;
  if (touch_busy) {
    /* Touch and the working controller cursor intentionally share pointer id 8.
     * Never emit both sources in one frame, and mirror the current A state so
     * releasing touch cannot manufacture a controller DOWN edge. */
    s_cur_down_prev = (held & HidNpadButton_A) ? 1 : 0;
  } else if (s_cursor_visible) {
    collect_cursor_events();
  }
}

/* Queried by the renderer (eglSwapBuffers_fake) to draw the cursor. */
int  cursor_is_visible(void) { return s_cursor_visible > 0; }
void cursor_get_pos(float *x, float *y) { if (x) *x = s_cur_x; if (y) *y = s_cur_y; }

int platform_poll_pointers(PtrEvent *out, int max) {
  int n = s_nevents < max ? s_nevents : max;
  memcpy(out, s_events, n * sizeof(PtrEvent));
  s_nevents -= n;
  if (s_nevents) memmove(s_events, s_events + n, s_nevents * sizeof(PtrEvent));
  return n;
}

/* B is the Android BACK key. Plus used to be wired here too, but it now shows
 * the cursor -- leaving it as BACK would fire a back-press every time you
 * brought the cursor up. */
int back_edge_pressed(void) {
  return (padGetButtonsDown(&s_pad) & HidNpadButton_B) ? 1 : 0;
}
int back_edge_released(void) {
  return (padGetButtonsUp(&s_pad) & HidNpadButton_B) ? 1 : 0;
}

int should_quit(void) {
  /* HOME suspends/exits via the applet; also allow a deliberate combo. */
  u64 h = padGetButtons(&s_pad);
  return (h & HidNpadButton_Minus) && (h & HidNpadButton_StickL) &&
         (h & HidNpadButton_StickR);
}

int handle_dock_change(int *w, int *h) {
  /* Fixed 1080p in every mode -- never signal a resolution change, so the
   * frame loop never re-resizes the engine's surface. */
  (void)w; (void)h;
  return 0;
}

/* ---------------------------------------------------------------------------
 * EGL tracing wrappers.
 *
 * The engine owns EGL: it calls eglGetDisplay/eglInitialize/eglChooseConfig/
 * eglCreateContext/eglCreateWindowSurface/eglMakeCurrent/eglSwapBuffers itself.
 * The frame loop runs at a steady 60fps and the engine compiles shaders and
 * uploads textures -- yet the screen stays black, which means something in this
 * chain is failing silently (the engine does not check every return). These
 * wrappers log each step and its EGL error code so we can see exactly which one
 * breaks, instead of guessing.
 * ------------------------------------------------------------------------- */

EGLDisplay eglGetDisplay_fake(EGLNativeDisplayType dpy) {
  /* eglGetDisplay() enters libnx viInitialize().  The title calls this from
   * several startup workers; reuse the display initialized by our main thread
   * instead of racing viInitialize on every worker. */
  if (g_egl_display != EGL_NO_DISPLAY) {
    debugPrintf("egl: eglGetDisplay(%p) -> cached %p\n",
                (void *)dpy, (void *)g_egl_display);
    return g_egl_display;
  }
  EGLDisplay d = eglGetDisplay(dpy);
  debugPrintf("egl: eglGetDisplay(%p) -> %p  err=0x%x\n",
              (void *)dpy, (void *)d, eglGetError());
  return d;
}

EGLBoolean eglInitialize_fake(EGLDisplay d, EGLint *maj, EGLint *min) {
  if (g_egl_display != EGL_NO_DISPLAY && d == g_egl_display) {
    if (maj) *maj = 1;
    if (min) *min = 5;
    debugPrintf("egl: eglInitialize(%p) -> cached 1 v1.5\n", (void *)d);
    return EGL_TRUE;
  }
  EGLBoolean r = eglInitialize(d, maj, min);
  debugPrintf("egl: eglInitialize(%p) -> %d  v%d.%d  err=0x%x\n",
              (void *)d, (int)r, maj ? *maj : -1, min ? *min : -1, eglGetError());
  return r;
}

EGLBoolean eglChooseConfig_fake(EGLDisplay d, const EGLint *attrib,
                                EGLConfig *cfgs, EGLint n, EGLint *num) {
  EGLBoolean r = eglChooseConfig(d, attrib, cfgs, n, num);
  debugPrintf("egl: eglChooseConfig -> %d  got %d config(s)  err=0x%x\n",
              (int)r, num ? *num : -1, eglGetError());
  return r;
}

EGLContext eglCreateContext_fake(EGLDisplay d, EGLConfig c,
                                 EGLContext share, const EGLint *attrib) {
  EGLContext ctx = eglCreateContext(d, c, share, attrib);
  debugPrintf("egl: eglCreateContext -> %p  err=0x%x%s\n",
              (void *)ctx, eglGetError(),
              ctx == EGL_NO_CONTEXT ? "   *** NO CONTEXT ***" : "");
  return ctx;
}

EGLSurface eglCreateWindowSurface_fake(EGLDisplay d, EGLConfig c,
                                       EGLNativeWindowType win, const EGLint *attrib) {
  EGLSurface s = eglCreateWindowSurface(d, c, win, attrib);
  debugPrintf("egl: eglCreateWindowSurface(win=%p) -> %p  err=0x%x%s\n",
              (void *)win, (void *)s, eglGetError(),
              s == EGL_NO_SURFACE ? "   *** NO SURFACE -> BLACK SCREEN ***" : "");
  return s;
}

EGLBoolean eglMakeCurrent_fake(EGLDisplay d, EGLSurface draw,
                               EGLSurface read, EGLContext ctx) {
  EGLBoolean r = eglMakeCurrent(d, draw, read, ctx);
  debugPrintf("egl: eglMakeCurrent(draw=%p ctx=%p) -> %d  err=0x%x%s\n",
              (void *)draw, (void *)ctx, (int)r, eglGetError(),
              r ? "" : "   *** FAILED -> GL calls go nowhere ***");
  return r;
}

void gl_frame_report(void);   /* defined below */
EGLBoolean eglSwapBuffers_fake(EGLDisplay d, EGLSurface s) {
  if (PVZ2_ENABLE_GL_TRACE)
    gl_frame_report();        /* what did the engine actually draw this frame? */
  cursor_draw();              /* overlay the cursor on the finished frame */
  EGLBoolean r = eglSwapBuffers(d, s);
#if PVZ2_ENABLE_GL_TRACE
  static unsigned n = 0;
  /* Keep the first few boot diagnostics. Periodic glGetError/eglGetError
   * polling is useful only in a graphics-trace build and otherwise adds a GPU
   * synchronization point to the presentation path. */
  if (n < 3 || (PVZ2_ENABLE_GL_TRACE && (n % 300) == 0)) {
    const GLenum gl = glGetError();
    debugPrintf("egl: eglSwapBuffers #%u(surf=%p) -> %d  eglerr=0x%x  glerr=0x%x%s\n",
                n, (void *)s, (int)r, eglGetError(), gl,
                r ? "" : "   *** SWAP FAILED -> nothing presented ***");
  }
  n++;
#endif
  return r;
}

/* ---------------------------------------------------------------------------
 * GL frame tracing.
 *
 * EGL is proven good (valid surface/context, MakeCurrent OK, SwapBuffers
 * succeeding every frame with glerr=0) and the game logic is alive (it loads
 * and plays menu music). So the engine is drawing into a working surface and we
 * still see black. These wrappers answer the remaining questions:
 *
 *   - is the viewport the full 1920x1080, or degenerate/0-sized?
 *   - what colour is it clearing to?
 *   - is it issuing any draw calls at all, and how many per frame?
 *   - is it rendering into FBO 0 (the screen) or into an offscreen FBO it
 *     never blits back?
 * ------------------------------------------------------------------------- */

static unsigned g_draws, g_clears, g_frame;
static GLuint   g_fbo_bound, g_draw_program;
static GLuint   g_array_buffer_bound;
static unsigned g_active_texture_unit;
static GLuint   g_texture_2d[8];
static GLenum   g_blend_src = GL_ONE, g_blend_dst = GL_ZERO;
static GLenum   g_blend_src_alpha = GL_ONE, g_blend_dst_alpha = GL_ZERO;
static GLboolean g_blend_enabled;
static GLboolean g_depth_enabled, g_cull_enabled;
static GLboolean g_scissor_enabled, g_color_mask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
static GLint g_scissor_rect[4];
typedef struct { GLuint program; GLint location; GLboolean set; GLfloat value[16]; } MatrixUniform;
static MatrixUniform g_screen_matrices[32];
static unsigned g_screen_matrix_count;
typedef struct {
  GLsizei width, height;
  GLenum format;
  GLboolean uploaded, alpha_source, alpha_upload_seen, alpha_mask_emulated, sampled;
  GLint min_filter, mag_filter;
  GLboolean min_filter_set, mag_filter_set;
  GLenum alpha_upload_format, alpha_upload_type;
  unsigned char alpha_min, alpha_max;
  size_t alpha_nonzero, alpha_total;
} TextureInfo;
static TextureInfo *g_texture_info;
static size_t g_texture_info_capacity;
/* GL_ALPHA compatibility needs a two-byte luminance/alpha upload.  Reuse one
 * grow-only conversion buffer instead of allocating and freeing 2-8 MiB for
 * every almanac/store/world-map atlas upload.  GLES consumes client pixels
 * synchronously, so the buffer is safe to reuse after each call returns. */
static unsigned char *g_alpha_la_scratch;
static size_t g_alpha_la_scratch_capacity;
typedef struct {
  GLint size;
  GLenum type;
  GLboolean normalized;
  GLsizei stride;
  const void *pointer;
  GLuint buffer;
  GLboolean enabled;
} VertexAttribInfo;
static VertexAttribInfo g_vertex_attribs[16] = {
  { 4, GL_FLOAT, GL_FALSE, 0, NULL, 0, GL_FALSE }
};
typedef struct {
  GLuint program;
  unsigned draws;
  GLboolean captured, blend, cull, stencil;
  GLenum blend_src, blend_dst;
  GLenum blend_equation;
  GLuint tex0, tex1;
  GLboolean scissor, depth, depth_mask, color_mask[4];
  GLint scissor_rect[4];
  GLboolean matrix_set;
  GLfloat matrix[4]; /* m00, m11, translation x/y */
  GLfloat matrix_full[16];
  GLint viewport[4];
  GLint attr_enabled[4], attr_size[4], attr_buffer[4];
  GLboolean color_alpha_known;
  GLfloat color_alpha_min, color_alpha_max;
  GLboolean color_rgb_known;
  GLfloat color_rgb_min, color_rgb_max;
  GLenum color_alpha_type;
  GLboolean color_alpha_normalized;
  GLboolean position_bounds_known;
  GLfloat position_min_x, position_max_x, position_min_y, position_max_y;
} ProgramDrawCount;
static ProgramDrawCount g_program_draws[16];
static unsigned g_program_draw_count;

static ProgramDrawCount *note_program_draw(void) {
  for (unsigned i = 0; i < g_program_draw_count; i++) {
    if (g_program_draws[i].program == g_draw_program) {
      g_program_draws[i].draws++;
      return &g_program_draws[i];
    }
  }
  if (g_program_draw_count < sizeof(g_program_draws) / sizeof(g_program_draws[0])) {
    ProgramDrawCount *entry = &g_program_draws[g_program_draw_count++];
    *entry = (ProgramDrawCount){ .program = g_draw_program, .draws = 1 };
    return entry;
  }
  return NULL;
}

static MatrixUniform *screen_matrix_for(GLuint program, GLint location, int create) {
  for (unsigned i = 0; i < g_screen_matrix_count; i++)
    if (g_screen_matrices[i].program == program && g_screen_matrices[i].location == location)
      return &g_screen_matrices[i];
  if (create && g_screen_matrix_count < sizeof(g_screen_matrices) / sizeof(g_screen_matrices[0])) {
    MatrixUniform *matrix = &g_screen_matrices[g_screen_matrix_count++];
    *matrix = (MatrixUniform){ .program = program, .location = location };
    return matrix;
  }
  return NULL;
}

static TextureInfo *texture_info_for(GLuint texture, int create) {
  if (!texture) return NULL;
  if ((size_t)texture >= g_texture_info_capacity) {
    if (!create) return NULL;
    size_t capacity = g_texture_info_capacity ? g_texture_info_capacity : 512;
    while (capacity <= (size_t)texture) {
      if (capacity > SIZE_MAX / 2) return NULL;
      capacity *= 2;
    }
    if (capacity > SIZE_MAX / sizeof(*g_texture_info)) return NULL;
    TextureInfo *grown = realloc(g_texture_info, capacity * sizeof(*grown));
    if (!grown) return NULL;
    memset(grown + g_texture_info_capacity, 0,
           (capacity - g_texture_info_capacity) * sizeof(*grown));
    g_texture_info = grown;
    g_texture_info_capacity = capacity;
#if PVZ2_ENABLE_GL_TRACE
    debugPrintf("GL TEXTURE TRACKER: grew to %zu entries for texture %u\n",
                capacity, texture);
#endif
  }
  return &g_texture_info[texture];
}

/* The UI shaders sample the .a channel of GL_ALPHA mask textures.  Mesa's
 * Switch path can accept those uploads yet return zero alpha when sampling.
 * Keep the engine's logical format for tracing, but store the pixels in the
 * GLES2-supported GL_LUMINANCE_ALPHA format so the sampled alpha is explicit. */
static unsigned char *alpha_to_luminance_alpha(const void *pixels, GLsizei width,
                                                GLsizei height) {
  if (!pixels || width <= 0 || height <= 0)
    return NULL;
  const size_t count = (size_t)width * (size_t)height;
  if (count > SIZE_MAX / 2)
    return NULL;
  const size_t bytes = count * 2;
  if (bytes > g_alpha_la_scratch_capacity) {
    size_t capacity = g_alpha_la_scratch_capacity ? g_alpha_la_scratch_capacity : 64 * 1024;
    while (capacity < bytes) {
      if (capacity > SIZE_MAX / 2) return NULL;
      capacity *= 2;
    }
    unsigned char *grown = realloc(g_alpha_la_scratch, capacity);
    if (!grown) return NULL;
    g_alpha_la_scratch = grown;
    g_alpha_la_scratch_capacity = capacity;
#if PVZ2_ENABLE_GL_TRACE
    debugPrintf("ALPHA SCRATCH: grew to %zu bytes\n", capacity);
#endif
  }

  const unsigned char *alpha = pixels;
  uint16_t *la = (uint16_t *)g_alpha_la_scratch;
  for (size_t i = 0; i < count; i++)
    la[i] = (uint16_t)alpha[i] * UINT16_C(0x0101);
  return g_alpha_la_scratch;
}

/* PVZ2 creates alpha-mask textures with an empty glTexImage2D allocation and
 * fills them with glTexSubImage2D afterwards.  Record the original CPU bytes
 * at either entry point, before the driver sees them.  This is diagnostic only
 * and deliberately does not alter texture data or GL state. */
static void note_alpha_upload(TextureInfo *info, const char *operation,
                              GLenum format, GLenum type, GLsizei width,
                              GLsizei height, const void *pixels) {
#if PVZ2_ENABLE_GL_TRACE
  if (!info || info->format != GL_ALPHA || width <= 0 || height <= 0)
    return;

  info->alpha_upload_seen = GL_TRUE;
  info->alpha_upload_format = format;
  info->alpha_upload_type = type;

  if (!pixels) return;
  if (!info->alpha_source) {
    debugPrintf("gl: alpha texture %u source=%s %dx%d fmt=0x%x type=0x%x\n",
                g_texture_2d[g_active_texture_unit], operation, width, height,
                format, type);
  }
  if (format != GL_ALPHA || type != GL_UNSIGNED_BYTE)
    return;

  const size_t count = (size_t)width * (size_t)height;
  const unsigned char *alpha = pixels;
  unsigned char min = 255, max = 0;
  size_t nonzero = 0;
  for (size_t i = 0; i < count; i++) {
    const unsigned char value = alpha[i];
    if (value < min) min = value;
    if (value > max) max = value;
    if (value) nonzero++;
  }

  if (!info->alpha_source) {
    info->alpha_min = min;
    info->alpha_max = max;
  } else {
    if (min < info->alpha_min) info->alpha_min = min;
    if (max > info->alpha_max) info->alpha_max = max;
  }
  info->alpha_source = GL_TRUE;
  info->alpha_nonzero += nonzero;
  info->alpha_total += count;
#else
  (void)info;
  (void)operation;
  (void)format;
  (void)type;
  (void)width;
  (void)height;
  (void)pixels;
#endif
}

static void trace_texture_pixels(GLuint texture) {
  TextureInfo *info = texture_info_for(texture, 0);
  if (!info || !info->uploaded || info->sampled) return;
  info->sampled = GL_TRUE;
  if (info->alpha_source) {
    debugPrintf("gl: alpha texture %u %dx%d bytes=%zu range=%u..%u nonzero=%zu\n",
                texture, info->width, info->height, info->alpha_total,
                info->alpha_min, info->alpha_max, info->alpha_nonzero);
  } else if (info->format == GL_ALPHA && info->alpha_upload_seen) {
    debugPrintf("gl: alpha texture %u %dx%d source fmt=0x%x type=0x%x (not raw alpha bytes)\n",
                texture, info->width, info->height, info->alpha_upload_format,
                info->alpha_upload_type);
  } else if (info->format == GL_ALPHA) {
    debugPrintf("gl: alpha texture %u %dx%d has no CPU pixel upload\n",
                texture, info->width, info->height);
  } else {
    debugPrintf("gl: texture %u %dx%d fmt=0x%x uploaded\n",
                texture, info->width, info->height, info->format);
  }
}

static size_t vertex_scalar_size(GLenum type) {
  switch (type) {
    case GL_FLOAT: return sizeof(GLfloat);
    case GL_UNSIGNED_BYTE: case GL_BYTE: return sizeof(GLbyte);
    case GL_UNSIGNED_SHORT: case GL_SHORT: return sizeof(GLshort);
    default: return 0;
  }
}

static GLfloat vertex_scalar(const void *src, GLenum type, GLboolean normalized) {
  switch (type) {
    case GL_FLOAT: {
      GLfloat value; memcpy(&value, src, sizeof(value)); return value;
    }
    case GL_UNSIGNED_BYTE: {
      const GLubyte value = *(const GLubyte *)src;
      return normalized ? (GLfloat)value / 255.0f : (GLfloat)value;
    }
    case GL_BYTE: {
      const GLbyte value = *(const GLbyte *)src;
      return normalized ? (value < 0 ? (GLfloat)value / 128.0f : (GLfloat)value / 127.0f)
                        : (GLfloat)value;
    }
    case GL_UNSIGNED_SHORT: {
      GLushort value; memcpy(&value, src, sizeof(value));
      return normalized ? (GLfloat)value / 65535.0f : (GLfloat)value;
    }
    case GL_SHORT: {
      GLshort value; memcpy(&value, src, sizeof(value));
      return normalized ? (value < 0 ? (GLfloat)value / 32768.0f : (GLfloat)value / 32767.0f)
                        : (GLfloat)value;
    }
    default: return 0.0f;
  }
}

static size_t index_scalar_size(GLenum type) {
  switch (type) {
    case GL_UNSIGNED_BYTE: return sizeof(GLubyte);
    case GL_UNSIGNED_SHORT: return sizeof(GLushort);
    default: return 0;
  }
}

static unsigned vertex_index(const void *src, GLenum type) {
  switch (type) {
    case GL_UNSIGNED_BYTE: return *(const GLubyte *)src;
    case GL_UNSIGNED_SHORT: {
      GLushort value;
      memcpy(&value, src, sizeof(value));
      return value;
    }
    default: return 0;
  }
}

static int client_range_readable(uintptr_t start, size_t size) {
  MemoryInfo memory;
  u32 page_info;
  if (R_FAILED(svcQueryMemory(&memory, &page_info, start)) ||
      (memory.perm & Perm_R) == 0 || start < memory.addr)
    return 0;
  const uintptr_t offset = start - memory.addr;
  return offset <= memory.size && size <= memory.size - offset;
}

static void capture_color_alpha(ProgramDrawCount *entry, GLsizei vertex_count, GLint first) {
  const GLuint color_attr = 1;
  if (!entry || vertex_count <= 0 || first < 0 || !entry->attr_enabled[color_attr] ||
      entry->attr_buffer[color_attr] != 0 || color_attr >= 16)
    return;

  const VertexAttribInfo *attr = &g_vertex_attribs[color_attr];
  const size_t scalar = vertex_scalar_size(attr->type);
  if (!attr->pointer || attr->size < 4 || !scalar) return;
  const size_t stride = attr->stride > 0 ? (size_t)attr->stride : (size_t)attr->size * scalar;
  const size_t samples = vertex_count < 16 ? (size_t)vertex_count : 16;
  const uintptr_t start = (uintptr_t)attr->pointer + (size_t)first * stride;
  const size_t need = (samples - 1) * stride + 4 * scalar;

  if (!client_range_readable(start, need))
    return;

  GLfloat min = 1.0e30f, max = -1.0e30f;
  GLfloat rgb_min = 1.0e30f, rgb_max = -1.0e30f;
  for (size_t i = 0; i < samples; i++) {
    for (size_t channel = 0; channel < 3; channel++) {
      const void *component = (const void *)(start + i * stride + channel * scalar);
      const GLfloat value = vertex_scalar(component, attr->type, attr->normalized);
      if (value < rgb_min) rgb_min = value;
      if (value > rgb_max) rgb_max = value;
    }
    const void *alpha = (const void *)(start + i * stride + 3 * scalar);
    const GLfloat value = vertex_scalar(alpha, attr->type, attr->normalized);
    if (value < min) min = value;
    if (value > max) max = value;
  }
  entry->color_alpha_known = GL_TRUE;
  entry->color_alpha_min = min;
  entry->color_alpha_max = max;
  entry->color_alpha_type = attr->type;
  entry->color_alpha_normalized = attr->normalized;
  entry->color_rgb_known = GL_TRUE;
  entry->color_rgb_min = rgb_min;
  entry->color_rgb_max = rgb_max;
}

static void capture_position_bounds(ProgramDrawCount *entry, GLsizei vertex_count,
                                    GLint first, GLenum index_type, const void *indices) {
  const GLuint position_attr = 0;
  if (!entry || vertex_count <= 0 || first < 0 || !entry->matrix_set ||
      !entry->attr_enabled[position_attr] || entry->attr_buffer[position_attr] != 0 ||
      position_attr >= 16)
    return;

  const VertexAttribInfo *attr = &g_vertex_attribs[position_attr];
  const size_t scalar = vertex_scalar_size(attr->type);
  if (!attr->pointer || attr->size < 2 || !scalar) return;
  const size_t stride = attr->stride > 0 ? (size_t)attr->stride : (size_t)attr->size * scalar;
  const size_t samples = vertex_count < 24 ? (size_t)vertex_count : 24;
  const size_t index_size = indices ? index_scalar_size(index_type) : 0;
  if (indices && (!index_size || !client_range_readable((uintptr_t)indices, samples * index_size)))
    return;

  GLfloat min_x = 1.0e30f, max_x = -1.0e30f;
  GLfloat min_y = 1.0e30f, max_y = -1.0e30f;
  size_t transformed = 0;
  for (size_t i = 0; i < samples; i++) {
    const unsigned vertex = indices
        ? vertex_index((const char *)indices + i * index_size, index_type)
        : (unsigned)first + (unsigned)i;
    const uintptr_t base = (uintptr_t)attr->pointer + (size_t)vertex * stride;
    if (!client_range_readable(base, attr->size * scalar)) continue;
    const GLfloat x = vertex_scalar((const void *)base, attr->type, attr->normalized);
    const GLfloat y = vertex_scalar((const void *)(base + scalar), attr->type, attr->normalized);
    const GLfloat z = attr->size >= 3
        ? vertex_scalar((const void *)(base + 2 * scalar), attr->type, attr->normalized) : 0.0f;
    const GLfloat *m = entry->matrix_full;
    const GLfloat clip_x = m[0] * x + m[4] * y + m[8] * z + m[12];
    const GLfloat clip_y = m[1] * x + m[5] * y + m[9] * z + m[13];
    const GLfloat clip_w = m[3] * x + m[7] * y + m[11] * z + m[15];
    if (clip_w > -0.00001f && clip_w < 0.00001f) continue;
    const GLfloat screen_x = (clip_x / clip_w + 1.0f) * 0.5f * entry->viewport[2] + entry->viewport[0];
    const GLfloat screen_y = (clip_y / clip_w + 1.0f) * 0.5f * entry->viewport[3] + entry->viewport[1];
    if (screen_x < min_x) min_x = screen_x;
    if (screen_x > max_x) max_x = screen_x;
    if (screen_y < min_y) min_y = screen_y;
    if (screen_y > max_y) max_y = screen_y;
    transformed++;
  }
  if (transformed) {
    entry->position_bounds_known = GL_TRUE;
    entry->position_min_x = min_x;
    entry->position_max_x = max_x;
    entry->position_min_y = min_y;
    entry->position_max_y = max_y;
  }
}

/* The normal per-program report deliberately samples only one draw, which is
 * enough for state validation but not for the tutorial seed packet: its quad
 * is batched with the lawn and tutorial banner.  When the user requests a HUD
 * screenshot, inspect every draw in that *same* frame, including the UV area
 * selected from the atlas.  This stays diagnostic-only and is off otherwise. */
static int capture_texcoord_bounds(GLsizei vertex_count, GLint first, GLenum index_type,
                                   const void *indices, GLfloat *min_u, GLfloat *max_u,
                                   GLfloat *min_v, GLfloat *max_v) {
  const GLuint texcoord_attr = 2;
  if (vertex_count <= 0 || first < 0 || texcoord_attr >= 16) return 0;
  const VertexAttribInfo *attr = &g_vertex_attribs[texcoord_attr];
  const size_t scalar = vertex_scalar_size(attr->type);
  if (!attr->pointer || attr->size < 2 || !scalar) return 0;
  const size_t stride = attr->stride > 0 ? (size_t)attr->stride : (size_t)attr->size * scalar;
  const size_t samples = vertex_count < 4096 ? (size_t)vertex_count : 4096;
  const size_t index_size = indices ? index_scalar_size(index_type) : 0;
  if (indices && (!index_size || !client_range_readable((uintptr_t)indices, samples * index_size)))
    return 0;

  GLfloat lo_u = 1.0e30f, hi_u = -1.0e30f, lo_v = 1.0e30f, hi_v = -1.0e30f;
  size_t read = 0;
  for (size_t i = 0; i < samples; i++) {
    const unsigned vertex = indices
        ? vertex_index((const char *)indices + i * index_size, index_type)
        : (unsigned)first + (unsigned)i;
    const uintptr_t base = (uintptr_t)attr->pointer + (size_t)vertex * stride;
    if (!client_range_readable(base, 2 * scalar)) continue;
    const GLfloat u = vertex_scalar((const void *)base, attr->type, attr->normalized);
    const GLfloat v = vertex_scalar((const void *)(base + scalar), attr->type, attr->normalized);
    if (u < lo_u) lo_u = u;
    if (u > hi_u) hi_u = u;
    if (v < lo_v) lo_v = v;
    if (v > hi_v) hi_v = v;
    read++;
  }
  if (!read) return 0;
  *min_u = lo_u; *max_u = hi_u; *min_v = lo_v; *max_v = hi_v;
  return 1;
}

static void capture_program_render_state(ProgramDrawCount *entry, GLsizei vertex_count,
                                         GLint first, GLenum index_type, const void *indices);

static void log_single_draw_call(unsigned draw_idx, GLenum mode, GLsizei vertex_count,
                                 GLint first, GLenum index_type, const void *indices,
                                 GLint index_buffer) {
#if !PVZ2_ENABLE_GL_TRACE
  (void)draw_idx; (void)mode; (void)vertex_count; (void)first;
  (void)index_type; (void)indices; (void)index_buffer;
  return;
#endif
  const int capture_this_frame = (g_frame < 3) ||
                                 (s_capture_frames_remaining > 0);
  if (!capture_this_frame) return;

  ProgramDrawCount entry = { .program = g_draw_program };
  capture_program_render_state(&entry, vertex_count, first, index_type, indices);

  GLfloat min_u = 0, max_u = 0, min_v = 0, max_v = 0;
  const int uv_known = capture_texcoord_bounds(vertex_count, first, index_type, indices,
                                                &min_u, &max_u, &min_v, &max_v);

  const TextureInfo *t0 = texture_info_for(entry.tex0, 0);
  const TextureInfo *t1 = texture_info_for(entry.tex1, 0);

  debugPrintf("DRAW #%u:\n"
              "  program=%u count=%d mode=0x%x fbo=%u ebo=%d\n"
              "  tex0=%u (%dx%d fmt=0x%x) tex1=%u (%dx%d fmt=0x%x)\n",
              draw_idx, entry.program, vertex_count, mode, g_fbo_bound, index_buffer,
              entry.tex0, t0 ? t0->width : 0, t0 ? t0->height : 0, t0 ? t0->format : 0,
              entry.tex1, t1 ? t1->width : 0, t1 ? t1->height : 0, t1 ? t1->format : 0);

  if (entry.position_bounds_known)
    debugPrintf("  xy=%.1f..%.1f, %.1f..%.1f\n", entry.position_min_x, entry.position_max_x,
                entry.position_min_y, entry.position_max_y);
  else
    debugPrintf("  xy=unavailable\n");

  if (uv_known)
    debugPrintf("  uv=%.5f..%.5f, %.5f..%.5f\n", min_u, max_u, min_v, max_v);
  else
    debugPrintf("  uv=unavailable\n");

  if (entry.color_rgb_known || entry.color_alpha_known)
    debugPrintf("  color=RGB(%.3f..%.3f) A(%.3f..%.3f)\n",
                entry.color_rgb_min, entry.color_rgb_max,
                entry.color_alpha_min, entry.color_alpha_max);
  else
    debugPrintf("  color=unavailable\n");

  debugPrintf("  blend=%d (src=0x%x dst=0x%x eq=0x%x) scissor=%d\n",
              entry.blend, entry.blend_src, entry.blend_dst, entry.blend_equation, entry.scissor);

  if (entry.matrix_set)
    debugPrintf("  matrix=[%.4f, %.4f, %.4f, %.4f]\n",
                entry.matrix[0], entry.matrix[1], entry.matrix[2], entry.matrix[3]);
  else
    debugPrintf("  matrix=UNSET\n");
}

static void capture_program_render_state(ProgramDrawCount *entry, GLsizei vertex_count,
                                         GLint first, GLenum index_type, const void *indices) {
  if (!entry || entry->captured) return;
  entry->captured = GL_TRUE;
  entry->blend = glIsEnabled(GL_BLEND);
  entry->cull = glIsEnabled(GL_CULL_FACE);
  entry->stencil = glIsEnabled(GL_STENCIL_TEST);
  GLint blend_src = 0, blend_dst = 0, blend_equation = 0;
  glGetIntegerv(GL_BLEND_SRC_RGB, &blend_src);
  glGetIntegerv(GL_BLEND_DST_RGB, &blend_dst);
  glGetIntegerv(GL_BLEND_EQUATION_RGB, &blend_equation);
  entry->blend_src = (GLenum)blend_src;
  entry->blend_dst = (GLenum)blend_dst;
  entry->blend_equation = (GLenum)blend_equation;
  entry->tex0 = g_texture_2d[0];
  entry->tex1 = g_texture_2d[1];
  entry->scissor = g_scissor_enabled;
  entry->depth = glIsEnabled(GL_DEPTH_TEST);
  glGetBooleanv(GL_DEPTH_WRITEMASK, &entry->depth_mask);
  memcpy(entry->color_mask, g_color_mask, sizeof(g_color_mask));
  memcpy(entry->scissor_rect, g_scissor_rect, sizeof(g_scissor_rect));
  MatrixUniform *matrix = screen_matrix_for(entry->program, 0, 0);
  if (!matrix) {
    for (unsigned i = 0; i < g_screen_matrix_count; i++)
      if (g_screen_matrices[i].program == entry->program) { matrix = &g_screen_matrices[i]; break; }
  }
  if (matrix && matrix->set) {
    entry->matrix_set = GL_TRUE;
    memcpy(entry->matrix_full, matrix->value, sizeof(entry->matrix_full));
    entry->matrix[0] = matrix->value[0];
    entry->matrix[1] = matrix->value[5];
    entry->matrix[2] = matrix->value[12];
    entry->matrix[3] = matrix->value[13];
  }
  trace_texture_pixels(entry->tex0);
  if (entry->tex1 != entry->tex0) trace_texture_pixels(entry->tex1);
  for (GLuint index = 0; index < 4; index++) {
    glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &entry->attr_enabled[index]);
    glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_SIZE, &entry->attr_size[index]);
    glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &entry->attr_buffer[index]);
  }
  capture_color_alpha(entry, vertex_count, first);
  glGetIntegerv(GL_VIEWPORT, entry->viewport);
  capture_position_bounds(entry, vertex_count, first, index_type, indices);
  if (entry->program == 28 || entry->program == 33) {
    GLfloat min_u, max_u, min_v, max_v;
    if (capture_texcoord_bounds(vertex_count, first, index_type, indices,
                                &min_u, &max_u, &min_v, &max_v))
      debugPrintf("gl: HUD probe program %u uv=%.5f..%.5f,%.5f..%.5f\n",
                  entry->program, min_u, max_u, min_v, max_v);
  }
}

void glViewport_fake(GLint x, GLint y, GLsizei w, GLsizei h) {
  /* SAFETY NET: the engine was setting a 0x0 viewport (it queried the surface
   * size and got 0), which clips every triangle -> black screen despite 26 draw
   * calls a frame. A zero-area viewport is never legitimate here, so clamp it to
   * the real screen. eglQuerySurface_fake fixes the source; this guarantees the
   * viewport regardless. */
  if (w <= 0 || h <= 0) {
    static int warned = 0;
    if (!warned) {
      warned = 1;
      debugPrintf("gl: glViewport(%d,%d,%d,%d) DEGENERATE -> forcing %dx%d\n",
                  x, y, w, h, screen_width, screen_height);
    }
    x = 0; y = 0; w = screen_width; h = screen_height;
  }
  glViewport(x, y, w, h);
}

void glClearColor_fake(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
  /* The Switch window surface is composited. PVZ2 clears its Android default
   * framebuffer with alpha 0, which is fine when SurfaceFlinger ignores that
   * channel but can make subsequent translucent HUD batches disappear on the
   * Horizon compositor. Preserve alpha for every off-screen target; only make
   * the displayed framebuffer opaque. */
  if (g_fbo_bound == 0 && a == 0.0f) a = 1.0f;
#if PVZ2_ENABLE_GL_TRACE
  static GLfloat lr = -1, lg = -1, lb = -1, la = -1;
  if ((PVZ2_ENABLE_GL_TRACE || PVZ2_ENABLE_VERBOSE_RUNTIME_LOG) &&
      (r != lr || g != lg || b != lb || a != la)) {
    debugPrintf("gl: glClearColor(%.2f, %.2f, %.2f, %.2f)\n", r, g, b, a);
    lr = r; lg = g; lb = b; la = a;
  }
#endif
  glClearColor(r, g, b, a);
}

void glClear_fake(GLbitfield mask) {
  if (PVZ2_ENABLE_GL_TRACE) ++g_clears;
  glClear(mask);
}

void glBindFramebuffer_fake(GLenum target, GLuint fb) {
  g_fbo_bound = fb;
  glBindFramebuffer(target, fb);
}

void glActiveTexture_fake(GLenum texture) {
  if (texture >= GL_TEXTURE0 && texture < GL_TEXTURE0 + 8)
    g_active_texture_unit = texture - GL_TEXTURE0;
  glActiveTexture(texture);
}

void glTexParameteri_fake(GLenum target, GLenum pname, GLint param) {
  GLuint texture = (target == GL_TEXTURE_2D && g_active_texture_unit < 8)
      ? g_texture_2d[g_active_texture_unit] : 0;
  TextureInfo *info = (pname == GL_TEXTURE_MIN_FILTER || PVZ2_ENABLE_GL_TRACE)
      ? texture_info_for(texture, 0) : NULL;
  GLint effective = param;
  if (info && target == GL_TEXTURE_2D && pname == GL_TEXTURE_MIN_FILTER) {
#if PVZ2_ENABLE_GL_TRACE
    info->min_filter = param;
    info->min_filter_set = GL_TRUE;
#endif
    /* No glGenerateMipmap entry point is available in this port and our
     * upload tracker only observes level 0.  Sampling a mipmap filter on a
     * level-0-only texture makes the texture incomplete (all-zero samples).
     * Keep the game's requested filter unless it actually requires mipmaps. */
    if (info->uploaded && (param == GL_NEAREST_MIPMAP_NEAREST ||
                           param == GL_LINEAR_MIPMAP_NEAREST ||
                           param == GL_NEAREST_MIPMAP_LINEAR ||
                           param == GL_LINEAR_MIPMAP_LINEAR)) {
      effective = GL_LINEAR;
      debugPrintf("gl: texture %u incomplete mip filter 0x%x -> GL_LINEAR (%dx%d fmt=0x%x)\n",
                  texture, param, info->width, info->height, info->format);
    }
  } else if (info && target == GL_TEXTURE_2D && pname == GL_TEXTURE_MAG_FILTER) {
#if PVZ2_ENABLE_GL_TRACE
    info->mag_filter = param;
    info->mag_filter_set = GL_TRUE;
#endif
  }
  glTexParameteri(target, pname, effective);
}

void glBindTexture_fake(GLenum target, GLuint texture) {
  if (target == GL_TEXTURE_2D && g_active_texture_unit < 8)
    g_texture_2d[g_active_texture_unit] = texture;
  glBindTexture(target, texture);
}

void glDeleteTextures_fake(GLsizei n, const GLuint *textures) {
  if (textures && n > 0) {
    for (GLsizei i = 0; i < n; i++) {
      const GLuint texture = textures[i];
      if (texture && (size_t)texture < g_texture_info_capacity)
        memset(&g_texture_info[texture], 0, sizeof(g_texture_info[texture]));
      for (unsigned unit = 0; unit < sizeof(g_texture_2d) / sizeof(g_texture_2d[0]); unit++)
        if (g_texture_2d[unit] == texture) g_texture_2d[unit] = 0;
    }
  }
  glDeleteTextures(n, textures);
}

void glTexImage2D_fake(GLenum target, GLint level, GLint internalformat,
                       GLsizei width, GLsizei height, GLint border, GLenum format,
                       GLenum type, const void *pixels) {
  TextureInfo *info = target == GL_TEXTURE_2D && level == 0 && g_active_texture_unit < 8
      ? texture_info_for(g_texture_2d[g_active_texture_unit], 1) : NULL;
  const GLboolean alpha_mask = target == GL_TEXTURE_2D && internalformat == GL_ALPHA &&
                               format == GL_ALPHA && type == GL_UNSIGNED_BYTE;
  const GLboolean emulate_alpha = alpha_mask;
  if (info) {
    *info = (TextureInfo){ .width = width, .height = height,
                           .format = (GLenum)internalformat, .uploaded = GL_TRUE,
                           .alpha_mask_emulated = emulate_alpha };
    note_alpha_upload(info, "TexImage2D", format, type, width, height, pixels);
  }
  if (emulate_alpha) {
#if PVZ2_ENABLE_GL_TRACE
    debugPrintf("gl: GL_ALPHA texture %u -> GL_LUMINANCE_ALPHA\n",
                g_texture_2d[g_active_texture_unit]);
#endif
    unsigned char *la = alpha_to_luminance_alpha(pixels, width, height);
    if (pixels && !la) { glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels); }
    else {
      glTexImage2D(target, level, GL_LUMINANCE_ALPHA, width, height, border,
                   GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, la);
    }
  } else {
    glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels);
  }
}

void glTexSubImage2D_fake(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                          GLsizei width, GLsizei height, GLenum format, GLenum type,
                          const void *pixels) {
  TextureInfo *info = target == GL_TEXTURE_2D && level == 0 && g_active_texture_unit < 8
      ? texture_info_for(g_texture_2d[g_active_texture_unit], 0) : NULL;
  note_alpha_upload(info, "TexSubImage2D", format, type, width, height, pixels);
  if (info && info->alpha_mask_emulated && format == GL_ALPHA && type == GL_UNSIGNED_BYTE) {
    unsigned char *la = alpha_to_luminance_alpha(pixels, width, height);
    if (la) {
      glTexSubImage2D(target, level, xoffset, yoffset, width, height,
                      GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, la);
      return;
    }
  }
  glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels);
}

void glCompressedTexImage2D_fake(GLenum target, GLint level, GLenum internalformat,
                                 GLsizei width, GLsizei height, GLint border,
                                 GLsizei image_size, const void *data) {
  TextureInfo *info = target == GL_TEXTURE_2D && level == 0 && g_active_texture_unit < 8
      ? texture_info_for(g_texture_2d[g_active_texture_unit], 1) : NULL;
  if (info) *info = (TextureInfo){ .width = width, .height = height,
                                   .format = internalformat, .uploaded = GL_TRUE };
  glCompressedTexImage2D(target, level, internalformat, width, height, border, image_size, data);
}

void glBindBuffer_fake(GLenum target, GLuint buffer) {
  if (target == GL_ARRAY_BUFFER) g_array_buffer_bound = buffer;
  glBindBuffer(target, buffer);
}

void glBlendFunc_fake(GLenum sfactor, GLenum dfactor) {
  g_blend_src = sfactor;
  g_blend_dst = dfactor;
  g_blend_src_alpha = sfactor;
  g_blend_dst_alpha = dfactor;

  glBlendFunc(sfactor, dfactor);
}

void glEnable_fake(GLenum cap) {
  if (cap == GL_BLEND) g_blend_enabled = GL_TRUE;
  if (cap == GL_DEPTH_TEST) g_depth_enabled = GL_TRUE;
  if (cap == GL_CULL_FACE) g_cull_enabled = GL_TRUE;
  if (cap == GL_SCISSOR_TEST) g_scissor_enabled = GL_TRUE;

  glEnable(cap);
}

void glDisable_fake(GLenum cap) {
  if (cap == GL_BLEND) g_blend_enabled = GL_FALSE;
  if (cap == GL_DEPTH_TEST) g_depth_enabled = GL_FALSE;
  if (cap == GL_CULL_FACE) g_cull_enabled = GL_FALSE;
  if (cap == GL_SCISSOR_TEST) g_scissor_enabled = GL_FALSE;

  glDisable(cap);
}

void glEnableVertexAttribArray_fake(GLuint index) {
  if (index < sizeof(g_vertex_attribs) / sizeof(g_vertex_attribs[0]))
    g_vertex_attribs[index].enabled = GL_TRUE;
  glEnableVertexAttribArray(index);
}

void glDisableVertexAttribArray_fake(GLuint index) {
  if (index < sizeof(g_vertex_attribs) / sizeof(g_vertex_attribs[0]))
    g_vertex_attribs[index].enabled = GL_FALSE;
  glDisableVertexAttribArray(index);
}

void glColorMask_fake(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha) {
  if (PVZ2_ENABLE_GL_TRACE) {
    g_color_mask[0] = red;
    g_color_mask[1] = green;
    g_color_mask[2] = blue;
    g_color_mask[3] = alpha;
  }
  glColorMask(red, green, blue, alpha);
}

void glScissor_fake(GLint x, GLint y, GLsizei width, GLsizei height) {
  if (PVZ2_ENABLE_GL_TRACE) {
    g_scissor_rect[0] = x;
    g_scissor_rect[1] = y;
    g_scissor_rect[2] = width;
    g_scissor_rect[3] = height;
  }
  glScissor(x, y, width, height);
}

GLint glGetUniformLocation_fake(GLuint program, const GLchar *name) {
  const GLint location = glGetUniformLocation(program, name);
  if (PVZ2_ENABLE_GL_TRACE && name && !strcmp(name, "Params"))
    debugPrintf("gl: program %u uniform Params loc=%d\n", program, location);
  if (PVZ2_ENABLE_GL_TRACE && location >= 0 && name &&
      !strcmp(name, "screenMatrix"))
    screen_matrix_for(program, location, 1);
  return location;
}

void glUniform1i_fake(GLint location, GLint value) {
  /* PVZ2 already selects Tex0/Tex1 correctly.
   * Preserve the engine's sampler choice instead of forcing our diagnostic
   * expectation onto every uniform update. */
  glUniform1i(location, value);
}

void glUniform4f_fake(GLint location, GLfloat x, GLfloat y, GLfloat z, GLfloat w) {
  if (PVZ2_ENABLE_GL_TRACE)
    debugPrintf("gl: program %u uniform4f loc=%d = (%.3f, %.3f, %.3f, %.3f)\n",
                g_draw_program, location, x, y, z, w);
  glUniform4f(location, x, y, z, w);
}

void glUniform4fv_fake(GLint location, GLsizei count, const GLfloat *v) {
  if (PVZ2_ENABLE_GL_TRACE && v && count > 0)
    debugPrintf("gl: program %u uniform4fv loc=%d count=%d = (%.3f, %.3f, %.3f, %.3f)\n",
                g_draw_program, location, count, v[0], v[1], v[2], v[3]);
  glUniform4fv(location, count, v);
}

void glUniformMatrix4fv_fake(GLint location, GLsizei count, GLboolean transpose,
                             const GLfloat *value) {
  if (PVZ2_ENABLE_GL_TRACE) {
    MatrixUniform *matrix = screen_matrix_for(g_draw_program, location, 0);
    if (matrix && count > 0 && value) {
      memcpy(matrix->value, value, sizeof(matrix->value));
      matrix->set = GL_TRUE;
    }
  }
  glUniformMatrix4fv(location, count, transpose, value);
}

void glVertexAttribPointer_fake(GLuint index, GLint size, GLenum type,
                                GLboolean normalized, GLsizei stride,
                                const void *pointer) {
  if (index < sizeof(g_vertex_attribs) / sizeof(g_vertex_attribs[0])) {
    VertexAttribInfo *attribute = &g_vertex_attribs[index];
    attribute->size = size;
    attribute->type = type;
    attribute->normalized = normalized;
    attribute->stride = stride;
    attribute->pointer = pointer;
    attribute->buffer = g_array_buffer_bound;
  }

  glVertexAttribPointer(index, size, type, normalized, stride, pointer);
}

void glUseProgram_fake(GLuint program) {
  /* Do not write Tex0/Tex1/Params on behalf of the game. The prior
   * long run showed the native sampler assignments were already correct, and
   * these per-bind lookups/writes dominated the log and CPU cost. */
  g_draw_program = program;
  glUseProgram(program);
}

void glDrawArrays_fake(GLenum mode, GLint first, GLsizei count) {
  if (PVZ2_ENABLE_GL_TRACE) {
    log_single_draw_call(g_draws, mode, count, first, 0, NULL, 0);
    capture_program_render_state(note_program_draw(), count, first, 0, NULL);
    g_draws++;
  }
  glDrawArrays(mode, first, count);
}

void glDrawElements_fake(GLenum mode, GLsizei count, GLenum type, const void *idx) {
  if (PVZ2_ENABLE_GL_TRACE) {
    GLint index_buffer = 0;
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &index_buffer);
    log_single_draw_call(g_draws, mode, count, 0,
                         index_buffer == 0 ? type : 0,
                         index_buffer == 0 ? idx : NULL, index_buffer);
    capture_program_render_state(note_program_draw(), count, 0,
                                 index_buffer == 0 ? type : 0,
                                 index_buffer == 0 ? idx : NULL);
    g_draws++;
  }
  glDrawElements(mode, count, type, idx);
}

/* Full-frame framebuffer readback/BMP dumping is not supported.
 * It stalled the render thread and SD I/O path and was no longer appropriate
 * for normal gameplay builds. */

/* Called from eglSwapBuffers_fake once per presented frame. */
void gl_frame_report(void) {
#if PVZ2_ENABLE_GL_TRACE
  const int reported = (g_frame < 3 || s_capture_frames_remaining > 0);
  if (reported) {
    GLint viewport[4] = {0};
    GLint program = 0;
    GLboolean color_mask[4] = {0};
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
    debugPrintf("gl: frame %u -- %u draw call(s), %u clear(s), fbo=%u%s\n",
                g_frame, g_draws, g_clears, g_fbo_bound,
                g_draws == 0 ? "   *** NO DRAW CALLS -> engine is drawing nothing ***"
                             : (g_fbo_bound != 0 ? "   *** rendering into an FBO, not the screen ***" : ""));
    debugPrintf("gl: state viewport=%d,%d %dx%d program=%d blend=%d scissor=%d depth=%d "
                "mask=%d%d%d%d err=0x%x\n", viewport[0], viewport[1], viewport[2],
                viewport[3], program, glIsEnabled(GL_BLEND), glIsEnabled(GL_SCISSOR_TEST),
                glIsEnabled(GL_DEPTH_TEST), color_mask[0], color_mask[1], color_mask[2],
                color_mask[3], glGetError());
    debugPrintf("gl: draw programs");
    for (unsigned i = 0; i < g_program_draw_count; i++) {
      const ProgramDrawCount *entry = &g_program_draws[i];
      debugPrintf(" %u=%u", entry->program, entry->draws);
      if (entry->captured)
        debugPrintf("[blend=%u:%u/%u eq=%u cull=%u stencil=%u tex=%u,%u depth=%u/%u mask=%u%u%u%u scissor=%u:%d,%d %dx%d]", entry->blend,
                    entry->blend_src, entry->blend_dst, entry->blend_equation, entry->cull,
                    entry->stencil, entry->tex0, entry->tex1,
                    entry->depth, entry->depth_mask,
                    entry->color_mask[0], entry->color_mask[1], entry->color_mask[2],
                    entry->color_mask[3], entry->scissor, entry->scissor_rect[0],
                    entry->scissor_rect[1], entry->scissor_rect[2], entry->scissor_rect[3]);
      if (entry->matrix_set)
        debugPrintf("[matrix=%.4f,%.4f %.4f,%.4f]", entry->matrix[0], entry->matrix[1],
                    entry->matrix[2], entry->matrix[3]);
      else
        debugPrintf("[matrix=UNSET]");
      if (entry->color_alpha_known)
        debugPrintf("[vcolA=%.3f..%.3f type=0x%x norm=%u]", entry->color_alpha_min,
                    entry->color_alpha_max, entry->color_alpha_type,
                    entry->color_alpha_normalized);
      else
        debugPrintf("[vcolA=unavailable]");
      if (entry->color_rgb_known)
        debugPrintf("[vcolRGB=%.3f..%.3f]", entry->color_rgb_min, entry->color_rgb_max);
      if (entry->position_bounds_known)
        debugPrintf("[xy=%.0f..%.0f,%.0f..%.0f]", entry->position_min_x,
                    entry->position_max_x, entry->position_min_y, entry->position_max_y);
      else
        debugPrintf("[xy=unavailable]");
      debugPrintf("[attr=%d/%d/b%d,%d/%d/b%d,%d/%d/b%d,%d/%d/b%d]",
                  entry->attr_enabled[0], entry->attr_size[0], entry->attr_buffer[0],
                  entry->attr_enabled[1], entry->attr_size[1], entry->attr_buffer[1],
                  entry->attr_enabled[2], entry->attr_size[2], entry->attr_buffer[2],
                  entry->attr_enabled[3], entry->attr_size[3], entry->attr_buffer[3]);
    }
    debugPrintf("\n");
  }
#endif
  if (s_capture_frames_remaining > 0) {
    s_capture_frames_remaining--;
  }
  g_draws = 0;
  g_clears = 0;
  g_program_draw_count = 0;
  g_frame++;
}

/* eglQuerySurface: the engine asks EGL for the surface size and uses it for
 * glViewport. It ended up with 0x0 -- everything it drew was clipped away
 * (26 draw calls/frame into FBO 0, black screen). Log what mesa actually
 * reports, and if it hands back a zero/failed size, substitute the real
 * screen size so the viewport can never be degenerate. */
EGLBoolean eglQuerySurface_fake(EGLDisplay d, EGLSurface s, EGLint attr, EGLint *val) {
  EGLBoolean r = eglQuerySurface(d, s, attr, val);
  if (attr == EGL_WIDTH || attr == EGL_HEIGHT) {
    const int want = (attr == EGL_WIDTH) ? screen_width : screen_height;
    static int logged = 0;
    if (logged < 6) {
      logged++;
      debugPrintf("egl: eglQuerySurface(%s) -> %d (ret=%d, err=0x%x)%s\n",
                  attr == EGL_WIDTH ? "EGL_WIDTH" : "EGL_HEIGHT",
                  val ? *val : -1, (int)r, eglGetError(),
                  (!r || !val || *val <= 0) ? "   *** BAD -> substituting real size ***" : "");
    }
    if (val && (!r || *val <= 0)) {   /* mesa gave us nothing usable */
      *val = want;
      r = EGL_TRUE;
    }
  }
  return r;
}

/* ===================== On-screen cursor ==================================
 * The engine owns the GL context and presents via eglSwapBuffers, so we draw
 * the cursor from inside eglSwapBuffers_fake -- after the engine has rendered
 * its frame, just before it goes to the panel. That means it always sits on top.
 *
 * We use our own tiny shader + client-side vertex array, and save/restore every
 * piece of GL state we touch, so the engine's next frame is unaffected.
 * ======================================================================== */

static GLuint s_cur_prog = 0;
static GLint  s_loc_pos, s_loc_screen, s_loc_origin, s_loc_scale, s_loc_colour;
static int    s_cur_gl_failed = 0;

/* A classic arrow, in local units with the tip at (0,0), y down. Drawn as a
 * triangle fan from the tip (the shape is star-shaped about the tip). */
static const GLfloat s_arrow[] = {
   0.0f,  0.0f,
   0.0f, 16.0f,
   4.0f, 12.0f,
   7.0f, 18.0f,
  10.0f, 16.5f,
   7.0f, 10.5f,
  12.0f, 10.0f,
};
#define ARROW_VERTS (sizeof(s_arrow) / (2 * sizeof(GLfloat)))

static GLuint cursor_compile(GLenum type, const char *src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) { glDeleteShader(s); return 0; }
  return s;
}

static int cursor_init_gl(void) {
  if (s_cur_prog) return 1;
  if (s_cur_gl_failed) return 0;

  static const char *vs =
    "attribute vec2 aPos;\n"
    "uniform vec2 uScreen;\n"
    "uniform vec2 uOrigin;\n"
    "uniform float uScale;\n"
    "void main() {\n"
    "  vec2 p = uOrigin + aPos * uScale;\n"
    "  vec2 ndc = vec2((p.x / uScreen.x) * 2.0 - 1.0,\n"
    "                  1.0 - (p.y / uScreen.y) * 2.0);\n"
    "  gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "}\n";
  static const char *fs =
    "precision mediump float;\n"
    "uniform vec4 uColour;\n"
    "void main() { gl_FragColor = uColour; }\n";

  GLuint v = cursor_compile(GL_VERTEX_SHADER, vs);
  GLuint f = cursor_compile(GL_FRAGMENT_SHADER, fs);
  if (!v || !f) { s_cur_gl_failed = 1; debugPrintf("cursor: shader compile failed\n"); return 0; }

  GLuint p = glCreateProgram();
  glAttachShader(p, v);
  glAttachShader(p, f);
  glBindAttribLocation(p, 0, "aPos");
  glLinkProgram(p);
  glDeleteShader(v);
  glDeleteShader(f);

  GLint ok = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) { glDeleteProgram(p); s_cur_gl_failed = 1; debugPrintf("cursor: link failed\n"); return 0; }

  s_cur_prog   = p;
  s_loc_pos    = 0;
  s_loc_screen = glGetUniformLocation(p, "uScreen");
  s_loc_origin = glGetUniformLocation(p, "uOrigin");
  s_loc_scale  = glGetUniformLocation(p, "uScale");
  s_loc_colour = glGetUniformLocation(p, "uColour");
  debugPrintf("cursor: gl ready (prog=%u)\n", p);
  return 1;
}

void cursor_draw(void) {
  if (!cursor_is_visible()) return;
  if (!cursor_init_gl())    return;

  float cx, cy;
  cursor_get_pos(&cx, &cy);

  /* All engine GLES entry points below are imported through our wrappers, so
   * the mirror is authoritative.  Avoid ~17 synchronous driver state queries
   * per visible-cursor frame (over 1,000 queries/s at 60 Hz in docked mode). */
  const GLuint prev_prog = g_draw_program;
  const GLuint prev_buf = g_array_buffer_bound;
  const GLenum bs_rgb = g_blend_src, bd_rgb = g_blend_dst;
  const GLenum bs_a = g_blend_src_alpha, bd_a = g_blend_dst_alpha;
  const VertexAttribInfo attr0 = g_vertex_attribs[0];
  const GLboolean was_blend = g_blend_enabled;
  const GLboolean was_depth = g_depth_enabled;
  const GLboolean was_cull = g_cull_enabled;
  const GLboolean was_scissor = g_scissor_enabled;

  /* --- draw --- */
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_SCISSOR_TEST);          /* the engine may have clipped to a sub-rect */
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  glUseProgram(s_cur_prog);
  glBindBuffer(GL_ARRAY_BUFFER, 0);    /* client-side array (legal in GLES2) */
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, s_arrow);

  glUniform2f(s_loc_screen, (GLfloat)screen_width, (GLfloat)screen_height);
  /* Tegra/Mesa can rasterize a clip-edge primitive badly when the controller
   * cursor tip sits exactly on NDC x=-1.  The logical pointer must still reach
   * x=0 for PVZ2 hit-testing, but the overlay itself does not need to place a
   * vertex exactly on the clip plane.  Keep only the visual tip one pixel
   * inside the left/top edge; input coordinates remain unchanged. */
  GLfloat draw_cx = cx < 1.0f ? 1.0f : cx;
  GLfloat draw_cy = cy < 1.0f ? 1.0f : cy;
  static int logged_edge_guard;
  if (!logged_edge_guard && (draw_cx != cx || draw_cy != cy)) {
    logged_edge_guard = 1;
    debugPrintf("CURSOR EDGE: visual origin guarded %.1f,%.1f -> %.1f,%.1f; logical pointer unchanged\n",
                cx, cy, draw_cx, draw_cy);
  }
  glUniform2f(s_loc_origin, draw_cx, draw_cy);

  const GLfloat scale = 2.4f;          /* ~40px tall on a 1080p screen */

  /* black outline first (same shape, scaled up from the tip), then white fill */
  glUniform1f(s_loc_scale, scale * 1.22f);
  glUniform4f(s_loc_colour, 0.0f, 0.0f, 0.0f, 0.85f);
  glDrawArrays(GL_TRIANGLE_FAN, 0, (GLsizei)ARROW_VERTS);

  glUniform1f(s_loc_scale, scale);
  glUniform4f(s_loc_colour, 1.0f, 1.0f, 1.0f, 1.0f);
  glDrawArrays(GL_TRIANGLE_FAN, 0, (GLsizei)ARROW_VERTS);

  /* --- restore ---
   * Restoring only ENABLED was not sufficient: glVertexAttribPointer() also
   * changes attribute 0's buffer/pointer/format state.  PVZ2 can reuse that
   * state on the next frame, which made the cursor overlay capable of leaking
   * stray geometry (observed as a flickering one-pixel edge). */
  glBindBuffer(GL_ARRAY_BUFFER, attr0.buffer);
  glVertexAttribPointer(0, attr0.size ? attr0.size : 4,
                        attr0.type ? attr0.type : GL_FLOAT,
                        attr0.normalized, attr0.stride, attr0.pointer);
  if (attr0.enabled) glEnableVertexAttribArray(0);
  else glDisableVertexAttribArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prev_buf);
  glUseProgram((GLuint)prev_prog);
  glBlendFuncSeparate((GLenum)bs_rgb, (GLenum)bd_rgb, (GLenum)bs_a, (GLenum)bd_a);
  if (!was_blend)  glDisable(GL_BLEND);      else glEnable(GL_BLEND);
  if (was_depth)   glEnable(GL_DEPTH_TEST);
  if (was_cull)    glEnable(GL_CULL_FACE);
  if (was_scissor) glEnable(GL_SCISSOR_TEST);
}
