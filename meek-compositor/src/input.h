#ifndef MEEK_COMPOSITOR_INPUT_H
#define MEEK_COMPOSITOR_INPUT_H

//
//input.h - libinput capture + dispatch to wl_seat resources.
//
//WHAT
//----
//Opens libinput on seat0 via the udev backend, pulls input events
//out of /dev/input/event*, and forwards them to all currently-bound
//wl_seat clients as standard wl_pointer / wl_keyboard / wl_touch
//events.
//
//SCOPE (C7.1 -- first pass)
//--------------------------
//Touch only. Poco F1 is a phone; pointer + keyboard can come later.
//
//FOCUS MODEL
//-----------
//Single full-screen client (the shell). All touch events target
//the shell's only wl_surface. No hit-testing. No per-surface
//focus. Multi-client focus lands when meek-shell owns the scene
//and has to route via meek_shell_v1 (see session/configs.md for
//how that's supposed to work under config 2).
//
//PERMISSIONS
//-----------
//libinput needs read+write access to /dev/input/event*. On pmOS
//that usually requires: (a) user is in `input` group, OR (b) a
//seat-management daemon (elogind + libseat) grants per-device
//ACLs. If open_restricted fails we log the path and the error and
//continue (compositor stays up, just with no input). Touch simply
//won't work; everything else does.
//
//EVENT LOOP
//----------
//libinput exposes a single fd (`libinput_get_fd`). We register
//that fd with wl_event_loop via wl_event_loop_add_fd and drain
//via libinput_dispatch + libinput_get_event when it fires. No
//separate thread.
//

struct wl_display;

//
//Set up libinput on seat0 and wire its fd into `display`'s event
//loop. Returns 0 on success; -1 on catastrophic failure (udev_new
//or libinput context creation failed). Per-device open failures
//are logged + ignored (compositor runs without those devices).
//
//Must be called AFTER globals__register_all so wl_seat is bindable
//by the time events start flowing.
//
int  input__init(struct wl_display* display);

//
//Stop dispatching, release libinput + udev. Call from main's
//shutdown path. Safe to call multiple times and safe to call if
//input__init failed / wasn't called.
//
void input__shutdown(void);

#endif
