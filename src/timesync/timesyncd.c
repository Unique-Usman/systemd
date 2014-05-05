/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2014 Kay Sievers

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/timerfd.h>
#include <sys/timex.h>
#include <sys/socket.h>

#include "missing.h"
#include "util.h"
#include "sparse-endian.h"
#include "log.h"
#include "socket-util.h"
#include "list.h"
#include "ratelimit.h"
#include "strv.h"
#include "sd-event.h"
#include "sd-resolve.h"
#include "sd-daemon.h"

#define TIME_T_MAX (time_t)((1UL << ((sizeof(time_t) << 3) - 1)) - 1)

#ifndef ADJ_SETOFFSET
#define ADJ_SETOFFSET                   0x0100  /* add 'time' to current time */
#endif

/* expected accuracy of time synchronization; used to adjust the poll interval */
#define NTP_ACCURACY_SEC                0.2

/*
 * "A client MUST NOT under any conditions use a poll interval less
 * than 15 seconds."
 */
#define NTP_POLL_INTERVAL_MIN_SEC       32
#define NTP_POLL_INTERVAL_MAX_SEC       2048

/*
 * Maximum delta in seconds which the system clock is gradually adjusted
 * (slew) to approach the network time. Deltas larger that this are set by
 * letting the system time jump. The kernel's limit for adjtime is 0.5s.
 */
#define NTP_MAX_ADJUST                  0.4

/* NTP protocol, packet header */
#define NTP_LEAP_PLUSSEC                1
#define NTP_LEAP_MINUSSEC               2
#define NTP_LEAP_NOTINSYNC              3
#define NTP_MODE_CLIENT                 3
#define NTP_MODE_SERVER                 4
#define NTP_FIELD_LEAP(f)               (((f) >> 6) & 3)
#define NTP_FIELD_VERSION(f)            (((f) >> 3) & 7)
#define NTP_FIELD_MODE(f)               ((f) & 7)
#define NTP_FIELD(l, v, m)              (((l) << 6) | ((v) << 3) | (m))

/*
 * "NTP timestamps are represented as a 64-bit unsigned fixed-point number,
 * in seconds relative to 0h on 1 January 1900."
 */
#define OFFSET_1900_1970        2208988800UL

#define RETRY_USEC (30*USEC_PER_SEC)
#define RATELIMIT_INTERVAL_USEC (10*USEC_PER_SEC)
#define RATELIMIT_BURST 10

struct ntp_ts {
        be32_t sec;
        be32_t frac;
} _packed_;

struct ntp_ts_short {
        be16_t sec;
        be16_t frac;
} _packed_;

struct ntp_msg {
        uint8_t field;
        uint8_t stratum;
        int8_t poll;
        int8_t precision;
        struct ntp_ts_short root_delay;
        struct ntp_ts_short root_dispersion;
        char refid[4];
        struct ntp_ts reference_time;
        struct ntp_ts origin_time;
        struct ntp_ts recv_time;
        struct ntp_ts trans_time;
} _packed_;

typedef struct Manager Manager;
typedef struct ServerAddress ServerAddress;
typedef struct ServerName ServerName;

struct ServerAddress {
        union sockaddr_union sockaddr;
        socklen_t socklen;
        LIST_FIELDS(ServerAddress, addresses);
};

struct ServerName {
        char *string;
        LIST_HEAD(ServerAddress, addresses);
        LIST_FIELDS(ServerName, names);
};

struct Manager {
        sd_event *event;
        sd_resolve *resolve;

        LIST_HEAD(ServerName, servers);

        RateLimit ratelimit;

        /* peer */
        sd_resolve_query *resolve_query;
        sd_event_source *event_receive;
        ServerName *current_server_name;
        ServerAddress *current_server_address;
        int server_socket;
        uint64_t packet_count;

        /* last sent packet */
        struct timespec trans_time_mon;
        struct timespec trans_time;
        usec_t retry_interval;
        bool pending;

        /* poll timer */
        sd_event_source *event_timer;
        usec_t poll_interval_usec;
        bool poll_resync;

        /* history data */
        struct {
                double offset;
                double delay;
        } samples[8];
        unsigned int samples_idx;
        double samples_jitter;

        /* last change */
        bool jumped;
        int drift_ppm;

        /* watch for time changes */
        sd_event_source *event_clock_watch;
        int clock_watch_fd;

        /* Retry connections */
        sd_event_source *event_retry;

        /* Handle SIGINT/SIGTERM */
        sd_event_source *sigterm, *sigint;
};

static void manager_free(Manager *m);
DEFINE_TRIVIAL_CLEANUP_FUNC(Manager*, manager_free);
#define _cleanup_manager_free_ _cleanup_(manager_freep)

static int manager_arm_timer(Manager *m, usec_t next);
static int manager_clock_watch_setup(Manager *m);
static int manager_connect(Manager *m);
static void manager_disconnect(Manager *m);

static double ntp_ts_to_d(const struct ntp_ts *ts) {
        return be32toh(ts->sec) + ((double)be32toh(ts->frac) / UINT_MAX);
}

static double ts_to_d(const struct timespec *ts) {
        return ts->tv_sec + (1.0e-9 * ts->tv_nsec);
}

static double tv_to_d(const struct timeval *tv) {
        return tv->tv_sec + (1.0e-6 * tv->tv_usec);
}

static double square(double d) {
        return d * d;
}

static int manager_send_request(Manager *m) {
        _cleanup_free_ char *pretty = NULL;
        struct ntp_msg ntpmsg = {
                /*
                 * "The client initializes the NTP message header, sends the request
                 * to the server, and strips the time of day from the Transmit
                 * Timestamp field of the reply.  For this purpose, all the NTP
                 * header fields are set to 0, except the Mode, VN, and optional
                 * Transmit Timestamp fields."
                 */
                .field = NTP_FIELD(0, 4, NTP_MODE_CLIENT),
        };
        ssize_t len;
        int r;

        assert(m);
        assert(m->current_server_name);
        assert(m->current_server_address);

        /*
         * Set transmit timestamp, remember it; the server will send that back
         * as the origin timestamp and we have an indication that this is the
         * matching answer to our request.
         *
         * The actual value does not matter, We do not care about the correct
         * NTP UINT_MAX fraction; we just pass the plain nanosecond value.
         */
        assert_se(clock_gettime(CLOCK_MONOTONIC, &m->trans_time_mon) >= 0);
        assert_se(clock_gettime(CLOCK_REALTIME, &m->trans_time) >= 0);
        ntpmsg.trans_time.sec = htobe32(m->trans_time.tv_sec + OFFSET_1900_1970);
        ntpmsg.trans_time.frac = htobe32(m->trans_time.tv_nsec);

        r = sockaddr_pretty(&m->current_server_address->sockaddr.sa, m->current_server_address->socklen, true, &pretty);
        if (r < 0) {
                log_error("Failed to format sockaddr: %s", strerror(-r));
                return r;
        }

        len = sendto(m->server_socket, &ntpmsg, sizeof(ntpmsg), MSG_DONTWAIT, &m->current_server_address->sockaddr.sa, m->current_server_address->socklen);
        if (len == sizeof(ntpmsg)) {
                m->pending = true;
                log_debug("Sent NTP request to %s (%s)", pretty, m->current_server_name->string);
        } else {
                log_debug("Sending NTP request to %s (%s) failed: %m", pretty, m->current_server_name->string);
                return manager_connect(m);
        }

        /* re-arm timer with incresing timeout, in case the packets never arrive back */
        if (m->retry_interval > 0) {
                if (m->retry_interval < NTP_POLL_INTERVAL_MAX_SEC * USEC_PER_SEC)
                        m->retry_interval *= 2;
        } else
                m->retry_interval = NTP_POLL_INTERVAL_MIN_SEC * USEC_PER_SEC;

        r = manager_arm_timer(m, m->retry_interval);
        if (r < 0) {
                log_error("Failed to rearm timer: %s", strerror(-r));
                return r;
        }

        return 0;
}

static int manager_timer(sd_event_source *source, usec_t usec, void *userdata) {
        Manager *m = userdata;

        assert(m);

        return manager_send_request(m);
}

static int manager_arm_timer(Manager *m, usec_t next) {
        int r;

        assert(m);
        assert(m->event_receive);

        if (next == 0) {
                m->event_timer = sd_event_source_unref(m->event_timer);
                return 0;
        }

        if (m->event_timer) {
                r = sd_event_source_set_time(m->event_timer, now(CLOCK_MONOTONIC) + next);
                if (r < 0)
                        return r;

                return sd_event_source_set_enabled(m->event_timer, SD_EVENT_ONESHOT);
        }

        return sd_event_add_time(
                        m->event,
                        &m->event_timer,
                        CLOCK_MONOTONIC,
                        now(CLOCK_MONOTONIC) + next, 0,
                        manager_timer, m);
}

static int manager_clock_watch(sd_event_source *source, int fd, uint32_t revents, void *userdata) {
        Manager *m = userdata;

        assert(m);

        /* rearm timer */
        manager_clock_watch_setup(m);

        /* skip our own jumps */
        if (m->jumped) {
                m->jumped = false;
                return 0;
        }

        /* resync */
        log_info("System time changed. Resyncing.");
        m->poll_resync = true;
        return manager_send_request(m);
}

/* wake up when the system time changes underneath us */
static int manager_clock_watch_setup(Manager *m) {

        struct itimerspec its = {
                .it_value.tv_sec = TIME_T_MAX
        };

        int r;

        assert(m);

        m->event_clock_watch = sd_event_source_unref(m->event_clock_watch);
        m->clock_watch_fd = safe_close(m->clock_watch_fd);

        m->clock_watch_fd = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK|TFD_CLOEXEC);
        if (m->clock_watch_fd < 0) {
                log_error("Failed to create timerfd: %m");
                return -errno;
        }

        if (timerfd_settime(m->clock_watch_fd, TFD_TIMER_ABSTIME|TFD_TIMER_CANCEL_ON_SET, &its, NULL) < 0) {
                log_error("Failed to set up timerfd: %m");
                return -errno;
        }

        r = sd_event_add_io(m->event, &m->event_clock_watch, m->clock_watch_fd, EPOLLIN, manager_clock_watch, m);
        if (r < 0) {
                log_error("Failed to create clock watch event source: %s", strerror(-r));
                return r;
        }

        return 0;
}

static int manager_adjust_clock(Manager *m, double offset, int leap_sec) {
        struct timex tmx = {};
        int r;

        assert(m);

        /*
         * For small deltas, tell the kernel to gradually adjust the system
         * clock to the NTP time, larger deltas are just directly set.
         *
         * Clear STA_UNSYNC, it will enable the kernel's 11-minute mode, which
         * syncs the system time periodically to the hardware clock.
         */
        if (fabs(offset) < NTP_MAX_ADJUST) {
                tmx.modes = ADJ_STATUS | ADJ_NANO | ADJ_OFFSET | ADJ_TIMECONST | ADJ_MAXERROR | ADJ_ESTERROR;
                tmx.status = STA_PLL;
                tmx.offset = offset * NSEC_PER_SEC;
                tmx.constant = log2i(m->poll_interval_usec / USEC_PER_SEC) - 4;
                tmx.maxerror = 0;
                tmx.esterror = 0;
                log_debug("  adjust (slew): %+.3f sec\n", offset);
        } else {
                tmx.modes = ADJ_SETOFFSET | ADJ_NANO;

                /* ADJ_NANO uses nanoseconds in the microseconds field */
                tmx.time.tv_sec = (long)offset;
                tmx.time.tv_usec = (offset - tmx.time.tv_sec) * NSEC_PER_SEC;

                /* the kernel expects -0.3s as {-1, 7000.000.000} */
                if (tmx.time.tv_usec < 0) {
                        tmx.time.tv_sec  -= 1;
                        tmx.time.tv_usec += NSEC_PER_SEC;
                }

                m->jumped = true;
                log_debug("  adjust (jump): %+.3f sec\n", offset);
        }

        switch (leap_sec) {
        case 1:
                tmx.status |= STA_INS;
                break;
        case -1:
                tmx.status |= STA_DEL;
                break;
        }

        r = clock_adjtime(CLOCK_REALTIME, &tmx);
        if (r < 0)
                return r;

        m->drift_ppm = tmx.freq / 65536;

        log_debug("  status       : %04i %s\n"
                  "  time now     : %li.%03lli\n"
                  "  constant     : %li\n"
                  "  offset       : %+.3f sec\n"
                  "  freq offset  : %+li (%i ppm)\n",
                  tmx.status, tmx.status & STA_UNSYNC ? "" : "sync",
                  tmx.time.tv_sec, tmx.time.tv_usec / NSEC_PER_MSEC,
                  tmx.constant,
                  (double)tmx.offset / NSEC_PER_SEC,
                  tmx.freq, m->drift_ppm);

        return 0;
}

static bool manager_sample_spike_detection(Manager *m, double offset, double delay) {
        unsigned int i, idx_cur, idx_new, idx_min;
        double jitter;
        double j;

        assert(m);

        m->packet_count++;

        /* ignore initial sample */
        if (m->packet_count == 1)
                return false;

        /* store the current data in our samples array */
        idx_cur = m->samples_idx;
        idx_new = (idx_cur + 1) % ELEMENTSOF(m->samples);
        m->samples_idx = idx_new;
        m->samples[idx_new].offset = offset;
        m->samples[idx_new].delay = delay;

        /* calculate new jitter value from the RMS differences relative to the lowest delay sample */
        jitter = m->samples_jitter;
        for (idx_min = idx_cur, i = 0; i < ELEMENTSOF(m->samples); i++)
                if (m->samples[i].delay > 0 && m->samples[i].delay < m->samples[idx_min].delay)
                        idx_min = i;

        j = 0;
        for (i = 0; i < ELEMENTSOF(m->samples); i++)
                j += square(m->samples[i].offset - m->samples[idx_min].offset);
        m->samples_jitter = sqrt(j / (ELEMENTSOF(m->samples) - 1));

        /* ignore samples when resyncing */
        if (m->poll_resync)
                return false;

        /* always accept offset if we are farther off than the round-trip delay */
        if (fabs(offset) > delay)
                return false;

        /* we need a few samples before looking at them */
        if (m->packet_count < 4)
                return false;

        /* do not accept anything worse than the maximum possible error of the best sample */
        if (fabs(offset) > m->samples[idx_min].delay)
                return true;

        /* compare the difference between the current offset to the previous offset and jitter */
        return fabs(offset - m->samples[idx_cur].offset) > 3 * jitter;
}

static void manager_adjust_poll(Manager *m, double offset, bool spike) {
        assert(m);

        if (m->poll_resync) {
                m->poll_interval_usec = NTP_POLL_INTERVAL_MIN_SEC * USEC_PER_SEC;
                m->poll_resync = false;
                return;
        }

        /* set to minimal poll interval */
        if (!spike && fabs(offset) > NTP_ACCURACY_SEC) {
                m->poll_interval_usec = NTP_POLL_INTERVAL_MIN_SEC * USEC_PER_SEC;
                return;
        }

        /* increase polling interval */
        if (fabs(offset) < NTP_ACCURACY_SEC * 0.25) {
                if (m->poll_interval_usec < NTP_POLL_INTERVAL_MAX_SEC * USEC_PER_SEC)
                        m->poll_interval_usec *= 2;
                return;
        }

        /* decrease polling interval */
        if (spike || fabs(offset) > NTP_ACCURACY_SEC * 0.75) {
                if (m->poll_interval_usec > NTP_POLL_INTERVAL_MIN_SEC * USEC_PER_SEC)
                        m->poll_interval_usec /= 2;
                return;
        }
}

static bool sockaddr_equal(union sockaddr_union *a, union sockaddr_union *b) {
        assert(a);
        assert(b);

        if (a->sa.sa_family != b->sa.sa_family)
                return false;

        if (a->sa.sa_family == AF_INET)
                return a->in.sin_addr.s_addr == b->in.sin_addr.s_addr;

        if (a->sa.sa_family == AF_INET6)
                return memcmp(&a->in6.sin6_addr, &b->in6.sin6_addr, sizeof(a->in6.sin6_addr)) == 0;

        return false;
}

static int manager_receive_response(sd_event_source *source, int fd, uint32_t revents, void *userdata) {
        _cleanup_free_ char *pretty = NULL;
        Manager *m = userdata;
        struct ntp_msg ntpmsg;

        struct iovec iov = {
                .iov_base = &ntpmsg,
                .iov_len = sizeof(ntpmsg),
        };
        union {
                struct cmsghdr cmsghdr;
                uint8_t buf[CMSG_SPACE(sizeof(struct timeval))];
        } control;
        union sockaddr_union server_addr;
        struct msghdr msghdr = {
                .msg_iov = &iov,
                .msg_iovlen = 1,
                .msg_control = &control,
                .msg_controllen = sizeof(control),
                .msg_name = &server_addr,
                .msg_namelen = sizeof(server_addr),
        };
        struct cmsghdr *cmsg;
        struct timespec now_ts;
        struct timeval *recv_time;
        ssize_t len;
        double origin, receive, trans, dest;
        double delay, offset;
        bool spike;
        int leap_sec;
        int r;

        assert(source);
        assert(m);

        if (revents & (EPOLLHUP|EPOLLERR)) {
                log_warning("Server connection returned error.");
                return manager_connect(m);
        }

        len = recvmsg(fd, &msghdr, MSG_DONTWAIT);
        if (len < 0) {
                if (errno == EAGAIN)
                        return 0;

                log_warning("Error receiving message. Disconnecting.");
                return manager_connect(m);
        }

        if (iov.iov_len < sizeof(struct ntp_msg)) {
                log_warning("Invalid response from server. Disconnecting.");
                return manager_connect(m);
        }

        if (!m->current_server_name ||
            !m->current_server_address ||
            !sockaddr_equal(&server_addr, &m->current_server_address->sockaddr)) {
                log_debug("Response from unknown server.");
                return 0;
        }

        recv_time = NULL;
        for (cmsg = CMSG_FIRSTHDR(&msghdr); cmsg; cmsg = CMSG_NXTHDR(&msghdr, cmsg)) {
                if (cmsg->cmsg_level != SOL_SOCKET)
                        continue;

                switch (cmsg->cmsg_type) {
                case SCM_TIMESTAMP:
                        recv_time = (struct timeval *) CMSG_DATA(cmsg);
                        break;
                }
        }
        if (!recv_time) {
                log_error("Invalid packet timestamp.");
                return -EINVAL;
        }

        if (!m->pending) {
                log_debug("Unexpected reply. Ignoring.");
                return 0;
        }

        /* check our "time cookie" (we just stored nanoseconds in the fraction field) */
        if (be32toh(ntpmsg.origin_time.sec) != m->trans_time.tv_sec + OFFSET_1900_1970 ||
            be32toh(ntpmsg.origin_time.frac) != m->trans_time.tv_nsec) {
                log_debug("Invalid reply; not our transmit time. Ignoring.");
                return 0;
        }

        if (NTP_FIELD_LEAP(ntpmsg.field) == NTP_LEAP_NOTINSYNC) {
                log_debug("Server is not synchronized. Disconnecting.");
                return manager_connect(m);
        }

        if (NTP_FIELD_VERSION(ntpmsg.field) != 4) {
                log_debug("Response NTPv%d. Disconnecting.", NTP_FIELD_VERSION(ntpmsg.field));
                return manager_connect(m);
        }

        if (NTP_FIELD_MODE(ntpmsg.field) != NTP_MODE_SERVER) {
                log_debug("Unsupported mode %d. Disconnecting.", NTP_FIELD_MODE(ntpmsg.field));
                return manager_connect(m);
        }

        /* valid packet */
        m->pending = false;
        m->retry_interval = 0;

        /* announce leap seconds */
        if (NTP_FIELD_LEAP(ntpmsg.field) & NTP_LEAP_PLUSSEC)
                leap_sec = 1;
        else if (NTP_FIELD_LEAP(ntpmsg.field) & NTP_LEAP_MINUSSEC)
                leap_sec = -1;
        else
                leap_sec = 0;

        /*
         * "Timestamp Name          ID   When Generated
         *  ------------------------------------------------------------
         *  Originate Timestamp     T1   time request sent by client
         *  Receive Timestamp       T2   time request received by server
         *  Transmit Timestamp      T3   time reply sent by server
         *  Destination Timestamp   T4   time reply received by client
         *
         *  The round-trip delay, d, and system clock offset, t, are defined as:
         *  d = (T4 - T1) - (T3 - T2)     t = ((T2 - T1) + (T3 - T4)) / 2"
         */
        assert_se(clock_gettime(CLOCK_MONOTONIC, &now_ts) >= 0);
        origin = tv_to_d(recv_time) - (ts_to_d(&now_ts) - ts_to_d(&m->trans_time_mon)) + OFFSET_1900_1970;
        receive = ntp_ts_to_d(&ntpmsg.recv_time);
        trans = ntp_ts_to_d(&ntpmsg.trans_time);
        dest = tv_to_d(recv_time) + OFFSET_1900_1970;

        offset = ((receive - origin) + (trans - dest)) / 2;
        delay = (dest - origin) - (trans - receive);

        spike = manager_sample_spike_detection(m, offset, delay);

        manager_adjust_poll(m, offset, spike);

        log_debug("NTP response:\n"
                  "  leap         : %u\n"
                  "  version      : %u\n"
                  "  mode         : %u\n"
                  "  stratum      : %u\n"
                  "  precision    : %.6f sec (%d)\n"
                  "  reference    : %.4s\n"
                  "  origin       : %.3f\n"
                  "  receive      : %.3f\n"
                  "  transmit     : %.3f\n"
                  "  dest         : %.3f\n"
                  "  offset       : %+.3f sec\n"
                  "  delay        : %+.3f sec\n"
                  "  packet count : %"PRIu64"\n"
                  "  jitter       : %.3f%s\n"
                  "  poll interval: %llu\n",
                  NTP_FIELD_LEAP(ntpmsg.field),
                  NTP_FIELD_VERSION(ntpmsg.field),
                  NTP_FIELD_MODE(ntpmsg.field),
                  ntpmsg.stratum,
                  exp2(ntpmsg.precision), ntpmsg.precision,
                  ntpmsg.stratum == 1 ? ntpmsg.refid : "n/a",
                  origin - OFFSET_1900_1970,
                  receive - OFFSET_1900_1970,
                  trans - OFFSET_1900_1970,
                  dest - OFFSET_1900_1970,
                  offset, delay,
                  m->packet_count,
                  m->samples_jitter, spike ? " spike" : "",
                  m->poll_interval_usec / USEC_PER_SEC);

        if (!spike) {
                r = manager_adjust_clock(m, offset, leap_sec);
                if (r < 0)
                        log_error("Failed to call clock_adjtime(): %m");
        }

        r = sockaddr_pretty(&m->current_server_address->sockaddr.sa, m->current_server_address->socklen, true, &pretty);
        if (r < 0) {
                log_error("Failed to format socket address: %s", strerror(-r));
                return r;
        }

        log_info("%s (%s): interval/delta/delay/jitter/drift %llus/%+.3fs/%.3fs/%.3fs/%+ippm%s",
                 pretty, m->current_server_name->string, m->poll_interval_usec / USEC_PER_SEC, offset, delay, m->samples_jitter, m->drift_ppm,
                 spike ? " (ignored)" : "");

        r = manager_arm_timer(m, m->poll_interval_usec);
        if (r < 0) {
                log_error("Failed to rearm timer: %s", strerror(-r));
                return r;
        }

        return 0;
}

static int manager_listen_setup(Manager *m) {
        union sockaddr_union addr = {};
        static const int tos = IPTOS_LOWDELAY;
        static const int on = 1;
        int r;

        assert(m);

        assert(m->server_socket < 0);
        assert(!m->event_receive);
        assert(m->current_server_address);

        addr.sa.sa_family = m->current_server_address->sockaddr.sa.sa_family;

        m->server_socket = socket(addr.sa.sa_family, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (m->server_socket < 0)
                return -errno;

        r = bind(m->server_socket, &addr.sa, m->current_server_address->socklen);
        if (r < 0)
                return -errno;

        r = setsockopt(m->server_socket, SOL_SOCKET, SO_TIMESTAMP, &on, sizeof(on));
        if (r < 0)
                return -errno;

        setsockopt(m->server_socket, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));

        return sd_event_add_io(m->event, &m->event_receive, m->server_socket, EPOLLIN, manager_receive_response, m);
}

static int manager_begin(Manager *m) {
        _cleanup_free_ char *pretty = NULL;
        int r;

        assert(m);
        assert_return(m->current_server_name, -EHOSTUNREACH);
        assert_return(m->current_server_address, -EHOSTUNREACH);

        m->poll_interval_usec = NTP_POLL_INTERVAL_MIN_SEC * USEC_PER_SEC;

        r = sockaddr_pretty(&m->current_server_address->sockaddr.sa, m->current_server_address->socklen, true, &pretty);
        if (r < 0) {
                log_warning("Failed to decode address of %s: %s", m->current_server_name->string, strerror(-r));
                return r;
        }

        log_debug("Connecting to NTP server %s (%s).", pretty, m->current_server_name->string);
        sd_notifyf(false, "STATUS=Using Time Server %s (%s)", pretty, m->current_server_name->string);

        r = manager_listen_setup(m);
        if (r < 0) {
                log_warning("Failed to setup connection socket: %s", strerror(-r));
                return r;
        }

        r = manager_clock_watch_setup(m);
        if (r < 0)
                return r;

        return manager_send_request(m);
}

static void server_name_flush_addresses(ServerName *n) {
        ServerAddress *a;

        assert(n);

        while ((a = n->addresses)) {
                LIST_REMOVE(addresses, n->addresses, a);
                free(a);
        }
}

static void manager_flush_names(Manager *m) {
        ServerName *n;

        assert(m);

        while ((n = m->servers)) {
                LIST_REMOVE(names, m->servers, n);
                free(n->string);
                server_name_flush_addresses(n);
                free(n);
        }
}

static int manager_resolve_handler(sd_resolve_query *q, int ret, const struct addrinfo *ai, void *userdata) {
        Manager *m = userdata;
        ServerAddress *a, *last = NULL;

        assert(q);
        assert(m);
        assert(m->current_server_name);

        m->resolve_query = sd_resolve_query_unref(m->resolve_query);

        if (ret != 0) {
                log_error("Failed to resolve %s: %s", m->current_server_name->string, gai_strerror(ret));

                /* Try next host */
                return manager_connect(m);
        }

        server_name_flush_addresses(m->current_server_name);

        for (; ai; ai = ai->ai_next) {
                _cleanup_free_ char *pretty = NULL;

                assert(ai->ai_addr);
                assert(ai->ai_addrlen >= offsetof(struct sockaddr, sa_data));
                assert(ai->ai_addrlen <= sizeof(union sockaddr_union));

                if (!IN_SET(ai->ai_addr->sa_family, AF_INET, AF_INET6)) {
                        log_warning("Unsuitable address protocol for %s", m->current_server_name->string);
                        continue;
                }

                a = new0(ServerAddress, 1);
                if (!a)
                        return log_oom();

                memcpy(&a->sockaddr, ai->ai_addr, ai->ai_addrlen);
                a->socklen = ai->ai_addrlen;

                LIST_INSERT_AFTER(addresses, m->current_server_name->addresses, last, a);
                last = a;

                sockaddr_pretty(&a->sockaddr.sa, a->socklen, true, &pretty);
                log_debug("Found address %s for %s.", pretty, m->current_server_name->string);
        }

        if (!m->current_server_name->addresses) {
                log_error("Failed to find suitable address for host %s.", m->current_server_name->string);

                /* Try next host */
                return manager_connect(m);
        }

        m->current_server_address = m->current_server_name->addresses;

        return manager_begin(m);
}

static int manager_retry(sd_event_source *source, usec_t usec, void *userdata) {
        Manager *m = userdata;

        assert(m);

        return manager_connect(m);
}

static int manager_connect(Manager *m) {

        struct addrinfo hints = {
                .ai_flags = AI_NUMERICSERV|AI_ADDRCONFIG,
                .ai_socktype = SOCK_DGRAM,
        };
        int r;

        assert(m);

        manager_disconnect(m);

        m->event_retry = sd_event_source_unref(m->event_retry);
        if (!ratelimit_test(&m->ratelimit)) {
                log_debug("Slowing down attempts to contact servers.");

                r = sd_event_add_time(m->event, &m->event_retry, CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + RETRY_USEC, 0, manager_retry, m);
                if (r < 0) {
                        log_error("Failed to create retry timer: %s", strerror(-r));
                        return r;
                }

                return 0;
        }

        /* If we already are operating on some address, switch to the
         * next one. */
        if (m->current_server_address && m->current_server_address->addresses_next)
                m->current_server_address = m->current_server_address->addresses_next;
        else {
                /* Hmm, we are through all addresses, let's look for the next host instead */
                m->current_server_address = NULL;

                if (m->current_server_name && m->current_server_name->names_next)
                        m->current_server_name = m->current_server_name->names_next;
                else {
                        if (!m->servers) {
                                m->current_server_name = NULL;
                                log_debug("No server found.");
                                return 0;
                        }

                        m->current_server_name = m->servers;
                }

                r = sd_resolve_getaddrinfo(m->resolve, &m->resolve_query, m->current_server_name->string, "123", &hints, manager_resolve_handler, m);
                if (r < 0) {
                        log_error("Failed to create resolver: %s", strerror(-r));
                        return r;
                }

                return 1;
        }

        r = manager_begin(m);
        if (r < 0)
                return r;

        return 1;
}

static int manager_add_server(Manager *m, const char *server) {
        ServerName *n;

        assert(m);
        assert(server);

        n = new0(ServerName, 1);
        if (!n)
                return -ENOMEM;

        n->string = strdup(server);
        if (!n->string) {
                free(n);
                return -ENOMEM;
        }

        LIST_PREPEND(names, m->servers, n);
        return 0;
}

static void manager_disconnect(Manager *m) {
        assert(m);

        m->resolve_query = sd_resolve_query_unref(m->resolve_query);

        m->event_timer = sd_event_source_unref(m->event_timer);

        m->event_receive = sd_event_source_unref(m->event_receive);
        m->server_socket = safe_close(m->server_socket);

        m->event_clock_watch = sd_event_source_unref(m->event_clock_watch);
        m->clock_watch_fd = safe_close(m->clock_watch_fd);
}

static int manager_new(Manager **ret) {
        _cleanup_manager_free_ Manager *m = NULL;
        int r;

        m = new0(Manager, 1);
        if (!m)
                return -ENOMEM;

        m->server_socket = m->clock_watch_fd = -1;

        RATELIMIT_INIT(m->ratelimit, RATELIMIT_INTERVAL_USEC, RATELIMIT_BURST);

        r = sd_event_default(&m->event);
        if (r < 0)
                return r;

        sd_event_add_signal(m->event, &m->sigterm, SIGTERM, NULL,  NULL);
        sd_event_add_signal(m->event, &m->sigint, SIGINT, NULL, NULL);

        r = sd_resolve_default(&m->resolve);
        if (r < 0)
                return r;

        r = sd_resolve_attach_event(m->resolve, m->event, 0);
        if (r < 0)
                return 0;

        r = manager_clock_watch_setup(m);
        if (r < 0)
                return r;

        *ret = m;
        m = NULL;

        return 0;
}

static void manager_free(Manager *m) {
        if (!m)
                return;

        manager_disconnect(m);
        manager_flush_names(m);

        sd_event_source_unref(m->sigint);
        sd_event_source_unref(m->sigterm);

        sd_event_source_unref(m->event_retry);

        sd_resolve_unref(m->resolve);
        sd_event_unref(m->event);

        free(m);
}

int main(int argc, char *argv[]) {
        _cleanup_manager_free_ Manager *m = NULL;
        const char *x;
        int r;

        log_set_target(LOG_TARGET_AUTO);
        log_parse_environment();
        log_open();

        assert_se(sigprocmask_many(SIG_BLOCK, SIGTERM, SIGINT, -1) == 0);

        r = manager_new(&m);
        if (r < 0) {
                log_error("Failed to allocate manager: %s", strerror(-r));
                goto out;
        }

        sd_notify(false, "READY=1");

        FOREACH_STRING(x, "time1.google.com", "time2.google.com", "time3.google.com", "time4.google.com", "0.fedora.pool.ntp.org") {

                r = manager_add_server(m, x);
                if (r < 0) {
                        log_error("Failed to add server %s: %s", x, strerror(-r));
                        goto out;
                }
        }

        r = manager_connect(m);
        if (r < 0)
                goto out;

        r = sd_event_loop(m->event);
        if (r < 0) {
                log_error("Failed to run event loop: %s", strerror(-r));
                goto out;
        }

        sd_event_get_exit_code(m->event, &r);

out:
        return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
