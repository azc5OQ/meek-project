#ifndef MEEK_COMPOSITOR_OUTPUT_X11_H
#define MEEK_COMPOSITOR_OUTPUT_X11_H

//
//output_x11.h - nested X11 window as compositor output.
//
//A TEMPORARY output backend for development. The "real" phone
//deployment will use DRM/KMS (takes over the screen directly).
//But that requires killing whatever display manager is already
//running, switching to a free tty, etc. -- ugly for day-to-day
//iteration. So: in dev we run meek-compositor nested inside an
//existing X11 session, and our output is a regular-looking window
//on your desktop.
//
//This module OWNS:
//  * an X11 Display* (our connection to the host X server)
//  * a Window (our output viewport)
//  * an EGL display + context + window surface (for rendering)
//  * a render-tick timer integrated into wl_event_loop
//
//It does NOT share the EGL context with egl_ctx.c. That context
//is on a GBM platform display (used for dmabuf import); ours is
//on an X11 platform display. Sharing across displays is UB per
//EGL spec, so we run two contexts and do eglMakeCurrent per
//operation. Slow in theory, fine in practice at 60Hz for a dev
//backend that's going away in C10 anyway.
//
//RENDER CONTENT (A4 scope):
//  - glClear to a solid color every frame + eglSwapBuffers. Proves
//    the output pipeline works.
//
//Later passes draw client surfaces as textured quads + the shell
//UI (C6 replaces everything here with "present meek-shell's single
//surface").
//

struct wl_display;

//
//Opens a host X connection, creates a window of the given size,
//sets up EGL + GLES3 context on it, and arms a render tick on the
//wl_event_loop (~60Hz). Returns 0 on success, nonzero on failure;
//on failure the compositor should keep running (log-only, skip
//output) because clients still need protocol service.
//
int  output_x11__init(struct wl_display* display, int width, int height);

//
//Tears down X connection + EGL + render timer. Safe to call
//multiple times.
//
void output_x11__shutdown(void);

#endif
