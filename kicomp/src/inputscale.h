/*
 * kicomp - X-INPUT-SCALE: confining the pointer to the logical desktop.
 *
 * When an output is scaled (comp.h: rect vs physical), the compositor
 * draws a logical desktop that is *smaller* than the monitor's scanout and
 * magnifies it to fill the panel. Windows, clicks and RandR geometry all
 * live in that logical box; the pixels outside it exist only as the
 * magnified image. The pointer must not be able to wander out there.
 *
 * That confinement is the one part of this that cannot be done client
 * side: cursor motion runs through the server's input pipeline on every
 * event, and a compositor reacting afterwards with a warp would be
 * visibly late. X-INPUT-SCALE is the fork's extension for exactly this
 * and nothing else -- one rectangle per CRTC, no coordinate remapping (see
 * the fork's doc/x11-per-output-scaling-extension.md).
 *
 * Which is why the extension gates the whole feature: without it, a scaled
 * output would be a desktop with a dead margin the pointer can enter and
 * nothing is drawn into. So when the server doesn't have it, kicomp scales
 * nothing at all, whatever the config or the DPI property say -- the
 * capability decides, never a guess about which server this is (section
 * 17/30).
 *
 * No client binding exists for this protocol yet, so the requests are
 * written on the wire directly, in the same shape xispanel's inputscale.c
 * and the server tree's own xis-smoke-test.c use.
 */
#ifndef KICOMP_INPUTSCALE_H
#define KICOMP_INPUTSCALE_H

#include "comp.h"

/* Probes the extension and sets comp.caps.input_scale. Called once, before
 * the outputs are built, because whether outputs may be scaled at all
 * depends on the answer. */
void inputscale_init(void);

/* Confines each scaled output's CRTC to its logical box, and releases the
 * confinement on every output that isn't scaled. Called after each
 * outputs_refresh(), so a hotplug or a mode change re-asserts it. */
void inputscale_apply(void);

/* Releases every confinement, without forgetting the scales -- what
 * outputs_refresh() calls *before* re-reading RandR.
 *
 * The reason is a feature of the server, not of this file: while a CRTC is
 * confined, the fork answers XRRGetCrtcInfo and XRRGetMonitors with the
 * confined box instead of the true scanout box, deliberately, so that
 * every ordinary client (the WM included) lays out inside the logical
 * desktop without knowing this extension exists. Which means a compositor
 * that re-read RandR while its own confinement was active would see its
 * logical box as the *physical* one and divide it by the scale again --
 * shrinking the desktop a little more on every hotplug, DPI change or
 * mode set. Letting go first is what makes the two features compose. */
void inputscale_release_all(void);

/* Releases every confinement this compositor set. Closing the connection
 * would do it too -- the server drops a confinement with the client that
 * set it -- but shutting down cleanly says so explicitly. */
void inputscale_shutdown(void);

#endif /* KICOMP_INPUTSCALE_H */
