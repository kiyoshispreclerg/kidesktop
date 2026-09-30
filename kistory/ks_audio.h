/* ks_audio - kistoryd's audio source: which apps played or recorded sound,
 * and when. Follows PulseAudio/PipeWire-pulse streams through
 * shared/xis_pactl_subscribe (one long-lived `pactl subscribe` child) and
 * `pactl list` on each stream event.
 *
 * A period starts when a stream is running and not corked (paused), and
 * ends when it is corked or removed; each period of at least 2 s becomes
 * one line, like focus periods:
 *
 *   audio_play / audio_rec   app: application.name   exe: process binary
 *                            subject: HH:MM:SS-HH:MM:SS   detail: media.name pid=N
 *
 * Level meters ("Peak detect" source outputs) are ignored; media.name is
 * dropped for apps with hidden titles (it often is the song or video). */
#ifndef KS_AUDIO_H
#define KS_AUDIO_H

int  ks_audio_init(void);
int  ks_audio_fd(void);      /* -1 while pactl isn't running (respawn backoff) */
void ks_audio_handle(void);  /* call when ks_audio_fd() is readable */
void ks_audio_flush(void);   /* ends every running period (shutdown) */

#endif /* KS_AUDIO_H */
