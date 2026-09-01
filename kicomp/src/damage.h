/*
 * kicomp - turning X Damage into the core's own damage region.
 *
 * Two things are deliberate here.
 *
 * Nothing is asked of the server when a DamageNotify arrives. The Damage
 * object accumulates on its own until it is subtracted, and the report
 * level is NON_EMPTY, so one event stands for "there is damage waiting"
 * however much arrives afterwards. Collecting once per frame therefore
 * costs the same whether a window damaged itself once or five hundred
 * times between two frames -- which is what a video player does.
 *
 * And the collection is batched: every waiting window's subtract goes out
 * first, then every reply is read, so a frame costs *one* round trip no
 * matter how many windows changed. Doing it per window instead would put
 * a full server round trip between each one, which is exactly the shape
 * of stall that makes a compositor feel slow while doing very little.
 */
#ifndef KICOMP_DAMAGE_H
#define KICOMP_DAMAGE_H

#include "comp.h"

/* A DamageNotify for this window. Records that there is something to
 * collect and returns; the region itself is fetched at frame time. */
void damage_window_reported(CompWindow *w);

/* Collects everything waiting into the outputs' damage regions. Called
 * once per iteration of the main loop, before painting. */
void damage_collect(void);

#endif /* KICOMP_DAMAGE_H */
