/*
 * Sends mirrored video frames to framecast over a local TCP connection. See framecast_sink.h
 * for the message format.
 */

#include "framecast_sink.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#define HEADER_SIZE 20

static logger_t *logger = NULL;
static unsigned short port = 0;
static int fd = -1;
static time_t last_attempt = 0;
/* Parameter sets from the latest frame that carried them, with their start codes. */
static unsigned char *parameter_sets = NULL;
static size_t parameter_sets_length = 0;
/* Whether the current connection has been sent parameter sets yet. */
static bool connection_has_parameter_sets = false;

void framecast_sink_init(logger_t *render_logger, unsigned short listen_port) {
    logger = render_logger;
    port = listen_port;
}

static void disconnect(void) {
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}

/* Connects at most once a second, so a missing listener costs nothing per frame. */
static bool ensure_connected(void) {
    if (fd >= 0) {
        return true;
    }
    time_t now = time(NULL);
    if (now == last_attempt) {
        return false;
    }
    last_attempt = now;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        return false;
    }
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr *) &address, sizeof(address)) < 0) {
        close(s);
        return false;
    }
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    /* A receiver that stops reading must not stall the mirroring connection for long. */
    struct timeval timeout = { 0, 250000 };
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    fd = s;
    connection_has_parameter_sets = false;
    logger_log(logger, LOGGER_INFO, "framecast: connected to 127.0.0.1:%u", port);
    return true;
}

static bool is_parameter_set(const unsigned char *nal, bool h265) {
    if (h265) {
        int type = (nal[0] >> 1) & 0x3f;
        return type >= 32 && type <= 34; /* VPS, SPS, PPS */
    }
    int type = nal[0] & 0x1f;
    return type == 7 || type == 8; /* SPS, PPS */
}

/* Keeps the parameter sets of a frame that carries any, so a later connection can start, and
 * says whether this frame carried them. */
static bool remember_parameter_sets(const unsigned char *data, int length, bool h265) {
    unsigned char *found = NULL;
    size_t found_length = 0;
    int start = -1;
    for (int i = 0; i <= length; i++) {
        bool boundary = i == length || (i + 4 <= length && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1);
        if (!boundary) {
            continue;
        }
        if (start >= 0 && start + 4 < i && is_parameter_set(data + start + 4, h265)) {
            unsigned char *grown = realloc(found, found_length + (i - start));
            if (!grown) {
                free(found);
                return false;
            }
            found = grown;
            memcpy(found + found_length, data + start, i - start);
            found_length += i - start;
        }
        start = i;
        if (i < length) {
            i += 3;
        }
    }
    if (!found) {
        return false;
    }
    free(parameter_sets);
    parameter_sets = found;
    parameter_sets_length = found_length;
    return true;
}

static bool write_all(struct iovec *parts, int count) {
    while (count > 0) {
        ssize_t n = writev(fd, parts, count);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        while (count > 0 && (size_t) n >= parts->iov_len) {
            n -= parts->iov_len;
            parts++;
            count--;
        }
        if (count > 0) {
            parts->iov_base = (char *) parts->iov_base + n;
            parts->iov_len -= n;
        }
    }
    return true;
}

static void put32(unsigned char *p, uint32_t v) {
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

void framecast_sink_send(const unsigned char *data, int length, bool h265, uint64_t pts) {
    if (!port || length <= 0) {
        return;
    }
    bool carries = remember_parameter_sets(data, length, h265);
    if (!ensure_connected()) {
        return;
    }
    /* Prefix the remembered parameter sets when this connection has not had any yet. */
    bool prefix = !connection_has_parameter_sets && !carries && parameter_sets;
    size_t total = length + (prefix ? parameter_sets_length : 0);
    unsigned char header[HEADER_SIZE];
    memcpy(header, "FCV1", 4);
    put32(header + 4, h265 ? 1 : 0);
    put32(header + 8, (uint32_t) (pts >> 32));
    put32(header + 12, (uint32_t) pts);
    put32(header + 16, (uint32_t) total);
    struct iovec parts[3];
    int count = 0;
    parts[count].iov_base = header;
    parts[count++].iov_len = HEADER_SIZE;
    if (prefix) {
        parts[count].iov_base = parameter_sets;
        parts[count++].iov_len = parameter_sets_length;
    }
    parts[count].iov_base = (void *) data;
    parts[count++].iov_len = length;
    if (!write_all(parts, count)) {
        logger_log(logger, LOGGER_INFO, "framecast: connection lost (%s)", strerror(errno));
        /* A partial write leaves the stream out of step, so start a new connection. */
        disconnect();
        return;
    }
    if (carries || prefix) {
        connection_has_parameter_sets = true;
    }
}

void framecast_sink_destroy(void) {
    disconnect();
    free(parameter_sets);
    parameter_sets = NULL;
    parameter_sets_length = 0;
}
