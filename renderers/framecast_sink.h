/*
 * Sends each mirrored video frame, whole and with the client's timestamp, to a local TCP
 * listener (framecast), bypassing GStreamer.
 *
 * Each frame is one message: a 20-byte header followed by the Annex B frame.
 *   bytes 0-3   "FCV1"
 *   bytes 4-7   flags, big-endian: bit 0 set for H.265
 *   bytes 8-15  presentation time in nanoseconds on the client's clock, big-endian
 *   bytes 16-19 frame length in bytes, big-endian
 *
 * The first frame on a new connection carries the latest parameter sets, so the receiver can
 * decode from it.
 */

#ifndef FRAMECAST_SINK_H
#define FRAMECAST_SINK_H

#include <stdbool.h>
#include <stdint.h>
#include "../lib/logger.h"

#ifdef __cplusplus
extern "C" {
#endif

void framecast_sink_init(logger_t *logger, unsigned short port);
void framecast_sink_send(const unsigned char *data, int length, bool h265, uint64_t pts);
void framecast_sink_destroy(void);

#ifdef __cplusplus
}
#endif

#endif
