/* platform.h -- Switch platform layer for the PvZ2 port: heap split, GLES2/EGL
 * context, and input (handheld multitouch + docked virtual cursor), unified so
 * main.c stays a clean lifecycle driver.
 * MIT license -- see LICENSE. */
#ifndef __PLATFORM_H__
#define __PLATFORM_H__

#include <stddef.h>
#include <stdint.h>

/* ---- SO load zone (passed to so_load as the RW image buffer) ------------- */
void  *heap_so_base(void);      /* aligned buffer of heap_so_limit() bytes    */
size_t heap_so_limit(void);     /* == SO_ZONE_MB (config.h)                   */

/* ---- GLES2 / EGL --------------------------------------------------------- */
void egl_init_context(void);    /* create context+surface at current res     */
void egl_swap_buffers(void);
void egl_exit_context(void);

/* ---- per-frame input ----------------------------------------------------- */
void padUpdate_all(void);       /* padUpdate + sample touch/cursor once/frame */
int  should_quit(void);         /* true when the user asks to exit            */
int  handle_dock_change(int *w, int *h);  /* 1 if dock state (res) changed    */
int  is_switch_handheld(void);
int  back_edge_pressed(void);   /* B / + edge -> Android Back down            */
int  back_edge_released(void);

/* Unified pointer events for this frame. Handheld = touchscreen fingers;
 * docked = a single stick-driven cursor with A as the "finger". */
enum { PTR_DOWN = 1, PTR_MOVE = 2, PTR_UP = 3 };
typedef struct { int id; float x, y, previous_x, previous_y; int phase; } PtrEvent;
int  platform_poll_pointers(PtrEvent *out, int max);
int  get_registered_touch_objects_count(void);
int  get_mapped_owners_count(void);
void trace_touch_probe_event(int phase, int id, float x, float y);
void trigger_touch_draw_capture(int frames);
/* Lightweight GL-trace requests; no framebuffer readback is performed. */
void request_hud_framebuffer_capture(void);
void request_debug_framebuffer_capture(const char *basename, int trace_frames);

/* EGL tracing wrappers (see platform.c): the engine owns EGL; these log each
 * step + its error code so a silent failure (NO_SURFACE / failed MakeCurrent)
 * shows up in the log instead of just a black screen. */
#include <EGL/egl.h>
EGLDisplay eglGetDisplay_fake(EGLNativeDisplayType dpy);
EGLBoolean eglInitialize_fake(EGLDisplay d, EGLint *maj, EGLint *min);
EGLBoolean eglChooseConfig_fake(EGLDisplay d, const EGLint *attrib, EGLConfig *cfgs, EGLint n, EGLint *num);
EGLContext eglCreateContext_fake(EGLDisplay d, EGLConfig c, EGLContext share, const EGLint *attrib);
EGLSurface eglCreateWindowSurface_fake(EGLDisplay d, EGLConfig c, EGLNativeWindowType win, const EGLint *attrib);
EGLBoolean eglMakeCurrent_fake(EGLDisplay d, EGLSurface draw, EGLSurface read, EGLContext ctx);
EGLBoolean eglSwapBuffers_fake(EGLDisplay d, EGLSurface s);

/* GL frame tracing (platform.c): shows viewport, clear colour, draw-call count
 * and the bound FBO, so a black screen with a working EGL surface can be traced
 * to "no draws" / "degenerate viewport" / "rendering into an offscreen FBO". */
#include <GLES2/gl2.h>
void glViewport_fake(GLint x, GLint y, GLsizei w, GLsizei h);
void glClearColor_fake(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void glClear_fake(GLbitfield mask);
void glBindFramebuffer_fake(GLenum target, GLuint fb);
void glActiveTexture_fake(GLenum texture);
void glGenTextures_fake(GLsizei n, GLuint *textures);
void glBindTexture_fake(GLenum target, GLuint texture);
void glDeleteTextures_fake(GLsizei n, const GLuint *textures);
GLboolean glIsTexture_fake(GLuint texture);
GLboolean glIsShader_fake(GLuint shader);
void gl_surface_validation_begin(uintptr_t game_base);
void gl_surface_validation_end(void);
void glBindBuffer_fake(GLenum target, GLuint buffer);
void glBlendFunc_fake(GLenum sfactor, GLenum dfactor);
void glEnableVertexAttribArray_fake(GLuint index);
void glDisableVertexAttribArray_fake(GLuint index);
void glEnable_fake(GLenum cap);
void glDisable_fake(GLenum cap);
void glColorMask_fake(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha);
void glScissor_fake(GLint x, GLint y, GLsizei width, GLsizei height);
GLint glGetUniformLocation_fake(GLuint program, const GLchar *name);
void glUniform1i_fake(GLint location, GLint value);
void glUniform4f_fake(GLint location, GLfloat x, GLfloat y, GLfloat z, GLfloat w);
void glUniform4fv_fake(GLint location, GLsizei count, const GLfloat *v);
void glUniformMatrix4fv_fake(GLint location, GLsizei count, GLboolean transpose,
                             const GLfloat *value);
void glVertexAttribPointer_fake(GLuint index, GLint size, GLenum type,
                                GLboolean normalized, GLsizei stride,
                                const void *pointer);
void glTexImage2D_fake(GLenum target, GLint level, GLint internalformat,
                       GLsizei width, GLsizei height, GLint border, GLenum format,
                       GLenum type, const void *pixels);
void glTexParameteri_fake(GLenum target, GLenum pname, GLint param);
void glTexSubImage2D_fake(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                          GLsizei width, GLsizei height, GLenum format, GLenum type,
                          const void *pixels);
void glCompressedTexImage2D_fake(GLenum target, GLint level, GLenum internalformat,
                                 GLsizei width, GLsizei height, GLint border,
                                 GLsizei image_size, const void *data);
void glUseProgram_fake(GLuint program);
void glDrawArrays_fake(GLenum mode, GLint first, GLsizei count);
void glDrawElements_fake(GLenum mode, GLsizei count, GLenum type, const void *idx);

EGLBoolean eglQuerySurface_fake(EGLDisplay d, EGLSurface s, EGLint attr, EGLint *val);

/* On-screen cursor: '+' shows it, '-' hides it, 'A' taps. Available in both
 * handheld and docked; in handheld the touchscreen stays live alongside it. */
int  cursor_is_visible(void);
void cursor_get_pos(float *x, float *y);
void cursor_draw(void);        /* called from eglSwapBuffers_fake */

#endif
