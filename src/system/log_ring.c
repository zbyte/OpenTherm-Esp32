/**
 * @file log_ring.c
 * @brief Keeps the most recent log text in RAM for GET /log.
 */

#include "log_ring.h"

#include <string.h>

#include "tal_api.h"
#include "tkl_output.h"

#define RING_SIZE (12 * 1024)

static char         s_ring[RING_SIZE];
static size_t       s_head; /* next write position */
static bool         s_wrapped;
static uint32_t     s_total; /* bytes ever written */
static MUTEX_HANDLE s_lock;

void log_ring_init(void)
{
    tal_mutex_create_init(&s_lock);
}

void log_ring_output(const char *str)
{
    tkl_log_output(str); /* USB console, as before */
    if (!s_lock) {
        return;
    }
    size_t len = strlen(str);
    tal_mutex_lock(s_lock);
    s_total += len;
    while (len) {
        size_t n = RING_SIZE - s_head < len ? RING_SIZE - s_head : len;
        memcpy(s_ring + s_head, str, n);
        str += n;
        len -= n;
        s_head += n;
        if (s_head == RING_SIZE) {
            s_head    = 0;
            s_wrapped = true;
        }
    }
    tal_mutex_unlock(s_lock);
}

size_t log_ring_read(char *buf, size_t cap)
{
    if (!s_lock) {
        return 0;
    }
    tal_mutex_lock(s_lock);
    size_t out = 0;
    if (s_wrapped) {
        size_t n = RING_SIZE - s_head < cap ? RING_SIZE - s_head : cap;
        memcpy(buf, s_ring + s_head, n);
        out = n;
    }
    size_t n = s_head < cap - out ? s_head : cap - out;
    memcpy(buf + out, s_ring, n);
    out += n;
    tal_mutex_unlock(s_lock);
    return out;
}

uint32_t log_ring_mark(void)
{
    return s_total;
}

size_t log_ring_since(uint32_t mark, char *buf, size_t cap)
{
    if (!s_lock || !cap) {
        return 0;
    }
    tal_mutex_lock(s_lock);
    size_t n = s_total - mark;
    if (n > cap) {
        n = cap;
    }
    if (n > RING_SIZE) {
        n = RING_SIZE;
    }
    /* the last n bytes end just before s_head */
    size_t start = (s_head + RING_SIZE - n) % RING_SIZE;
    size_t first = RING_SIZE - start < n ? RING_SIZE - start : n;
    memcpy(buf, s_ring + start, first);
    memcpy(buf + first, s_ring, n - first);
    tal_mutex_unlock(s_lock);
    return n;
}
