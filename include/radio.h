/*
 * radio.h: internet radio on the panel. An Icecast / SHOUTcast MP3 stream
 * (HTTP or HTTPS) is received on core 0 into a ring in PSRAM, decoded there
 * (minimp3) into mono 22.05 kHz PCM in a second ring, and pulled from that
 * ring by the sound engine on core 1 (rainsynth.h, kind RS_RADIO), so the
 * speaker's tone stage, the sleep timer and the dawn chorus all apply as for
 * rain. A few seconds of decoded sound ride out a dashboard download or a
 * Wi-Fi hiccup; a dead stream reconnects by itself.
 *
 * Presets (name + URL) live in flash blob slot 6; the first boot seeds a
 * handful of SomaFM channels. SomaFM's stream servers refuse "browser-like"
 * and tool identities but accept a plainly named device; the request carries
 * the firmware's own User-Agent, and a 403 is retried once with none.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define RADIO_PRESETS   7
#define RADIO_NAME_MAX  32
#define RADIO_URL_MAX   92

typedef struct { char name[RADIO_NAME_MAX]; char url[RADIO_URL_MAX]; } radio_preset_t;

typedef enum { RADIO_OFF, RADIO_DNS, RADIO_CONNECTING, RADIO_HEADERS, RADIO_BUFFERING, RADIO_PLAYING, RADIO_STALLED, RADIO_FAILED } radio_state_t;

/* pcm: room for the decoded ring (RADIO_PCM_BYTES); work: the compressed ring,
 * the decoder and its scratch (RADIO_WORK_BYTES). Both in PSRAM. */
#define RADIO_PCM_BYTES   (22050u * 2u * 8u)          /* 8 s of mono 22.05 kHz */
#define RADIO_WORK_BYTES  (256u * 1024u + 32u * 1024u)
void radio_init(uint8_t *pcm, uint8_t *work);

void radio_play(const char *url, const char *name);   /* start, or switch to, a stream */
void radio_stop(void);
void radio_tick(void);                                /* core 0, often: decode, time out, reconnect */
int  radio_pull(int16_t *dst, int n);                 /* core 1: n mono samples; zeros while there is nothing to play */

radio_state_t radio_state(void);
const char   *radio_state_name(void);
const char   *radio_station(void);                    /* the stream's icy-name, else the preset's name */
const char   *radio_title(void);                      /* what is playing (ICY metadata), "" if unknown */
int           radio_kbps(void);
int           radio_buffer_ms(void);                  /* decoded sound in hand */
uint32_t      radio_underruns(void);
const char   *radio_error(void);                      /* why it failed, "" if it hasn't */

/* presets */
int   radio_preset_count(void);
const radio_preset_t *radio_preset(int i);
bool  radio_preset_set(int i, const char *name, const char *url);   /* saved to flash; "" clears the slot */
void  radio_presets_load(void);                       /* boot: the blob, else the seeds */
