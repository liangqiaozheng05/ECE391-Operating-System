/*! @file uio.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‍‌‍‍‍‌‌‍‌​⁠⁠‌
    @brief Uniform I/O interface
    @copyright Copyright (c) 2024-2025 University of Illinois

*/

#ifdef UIO_DEBUG
#define DEBUG
#endif

#ifdef UIO_TRACE
#define TRACE
#endif

#include "uio.h"

#include <stddef.h>  // for NULL and offsetof

#include "error.h"
#include "heap.h"
#include "memory.h"
#include "misc.h"
#include "string.h"
#include "thread.h"
#include "uioimpl.h"

struct pipe {
    char *buf;
    unsigned long size;
    unsigned long head;
    unsigned long tail;
    unsigned long count;

    struct condition can_read;
    struct condition can_write;

    int reader_open;
    int writer_open;
};

struct pipe_end {
    struct uio uio;
    struct pipe *pipe;
    int is_reader;
};

static void pipe_read_close(struct uio *uio);
static void pipe_write_close(struct uio *uio);
static long pipe_read(struct uio *uio, void *buf, unsigned long bufsz);
static long pipe_write(struct uio *uio, const void *buf, unsigned long bufsz);
static int  pipe_cntl(struct uio *uio, int op, void *arg);

static const struct uio_intf pipe_read_intf = {
    .close = pipe_read_close,
    .read  = pipe_read,
    .write = NULL,
    .cntl  = pipe_cntl,
};

static const struct uio_intf pipe_write_intf = {
    .close = pipe_write_close,
    .read  = NULL,
    .write = pipe_write,
    .cntl  = pipe_cntl,
};

static struct pipe_end *pipe_from_uio(struct uio *uio) {
    return (struct pipe_end *)((char *)uio - offsetof(struct pipe_end, uio));
}


void create_pipe(struct uio **wptr, struct uio **rptr) {
    if (wptr == NULL || rptr == NULL) {
        return;
    }

    *wptr = NULL;
    *rptr = NULL;
    struct pipe *p = kmalloc(sizeof(struct pipe));
    if (p == NULL) {
        return;
    }
    void *buf = alloc_phys_page();
    if (buf == NULL) {
        kfree(p);
        return;
    }

    p->buf   = (char *)buf;
    p->size  = PAGE_SIZE;
    p->head  = 0;
    p->tail  = 0;
    p->count = 0;

    p->reader_open = 1;
    p->writer_open = 1;

    condition_init(&p->can_read, "pipe_can_read");
    condition_init(&p->can_write, "pipe_can_write");
    struct pipe_end *r_end = kmalloc(sizeof(struct pipe_end));
    struct pipe_end *w_end = kmalloc(sizeof(struct pipe_end));
    if (r_end == NULL || w_end == NULL) {
        if (r_end) kfree(r_end);
        if (w_end) kfree(w_end);
        free_phys_page(buf);
        kfree(p);
        return;
    }

    uio_init1(&r_end->uio, &pipe_read_intf);
    r_end->pipe      = p;
    r_end->is_reader = 1;
    uio_init1(&w_end->uio, &pipe_write_intf);
    w_end->pipe      = p;
    w_end->is_reader = 0;

    *wptr = &w_end->uio;
    *rptr = &r_end->uio;
}

static void pipe_read_close(struct uio *uio) {
    struct pipe_end *pend = pipe_from_uio(uio);
    struct pipe *p = pend->pipe;

    p->reader_open = 0;
    condition_broadcast(&p->can_write);
    if (!p->writer_open) {
        if (p->buf) {
            free_phys_page(p->buf);
        }
        kfree(p);
    }
    kfree(pend);
}

static void pipe_write_close(struct uio *uio) {
    struct pipe_end *pend = pipe_from_uio(uio);
    struct pipe *p = pend->pipe;

    p->writer_open = 0;
    condition_broadcast(&p->can_read);

    if (!p->reader_open) {
        if (p->buf) {
            free_phys_page(p->buf);
        }
        kfree(p);
    }

    kfree(pend);
}
static long pipe_read(struct uio *uio, void *buf, unsigned long bufsz) {
    if (buf == NULL || bufsz == 0) {
        return 0;
    }

    struct pipe_end *pend = pipe_from_uio(uio);
    struct pipe *p = pend->pipe;
    if (!pend->is_reader) {
        return -EINVAL;
    }

    long total_read = 0;
    while (total_read == 0) {
        while (p->count == 0) {
            if (!p->writer_open) {
                return 0;
            }
            condition_wait(&p->can_read);
        }
        unsigned long can_read = p->count;
        if (can_read > bufsz) {
            can_read = bufsz;
        }
        unsigned long first = p->size - p->head;
        if (first > can_read) {
            first = can_read;
        }

        memcpy(buf, p->buf + p->head, first);

        unsigned long second = can_read - first;
        if (second > 0) {
            memcpy((char *)buf + first, p->buf, second);
        }

        p->head = (p->head + can_read) % p->size;
        p->count -= can_read;
        total_read += (long)can_read;
        condition_broadcast(&p->can_write);
    }

    return total_read;
}

static long pipe_write(struct uio *uio, const void *buf, unsigned long bufsz) {
    if (buf == NULL || bufsz == 0) {
        return 0;
    }

    struct pipe_end *pend = pipe_from_uio(uio);
    struct pipe *p = pend->pipe;
    if (pend->is_reader) {
        return -EINVAL;
    }

    long written = 0;

    while ((unsigned long)written < bufsz) {
        if (!p->reader_open) {
            return (written > 0) ? written : -EPIPE;
        }

        while (p->count == p->size && p->reader_open) {
            condition_wait(&p->can_write);
        }

        if (!p->reader_open && p->count == p->size) {
            return (written > 0) ? written : -EPIPE;
        }

        unsigned long space = p->size - p->count;
        if (space == 0) {
            continue;
        }

        unsigned long to_write = bufsz - (unsigned long)written;
        if (to_write > space) {
            to_write = space;
        }

        unsigned long first = p->size - p->tail;
        if (first > to_write) {
            first = to_write;
        }
        memcpy(p->buf + p->tail, (const char *)buf + written, first);

        unsigned long second = to_write - first;
        if (second > 0) {
            memcpy(p->buf, (const char *)buf + written + first, second);
        }

        p->tail = (p->tail + to_write) % p->size;
        p->count += to_write;
        written += (long)to_write;
        condition_broadcast(&p->can_read);
    }

    return written;
}

static int pipe_cntl(struct uio *uio, int op, void *arg) {
    (void)uio;
    (void)op;
    (void)arg;
    return -ENOTSUP;
}

static void nulluio_close(struct uio* uio);

static long nulluio_read(struct uio* uio, void* buf, unsigned long bufsz);

static long nulluio_write(struct uio* uio, const void* buf, unsigned long buflen);

// INTERNAL GLOBAL VARIABLES AND CONSTANTS
//

void uio_close(struct uio* uio) {
    debug("uio_close: refcnt=%d, has_close=%d", uio->refcnt, (uio->intf->close != NULL));

    // Decrement reference count if it's greater than 0
    if (uio->refcnt > 0) {
        uio->refcnt--;
        debug("uio_close: decremented refcnt to %d", uio->refcnt);
    }

    // Only call the actual close method when refcnt reaches 0
    if (uio->refcnt == 0 && uio->intf->close != NULL) {
        debug("uio_close: calling close method");
        uio->intf->close(uio);
    } else if (uio->refcnt > 0) {
        debug("uio_close: NOT calling close (refcnt=%d still has references)", uio->refcnt);
    }
}

long uio_read(struct uio* uio, void* buf, unsigned long bufsz) {
    if (uio->intf->read != NULL) {
        if (0 <= (long)bufsz)
            return uio->intf->read(uio, buf, bufsz);
        else
            return -EINVAL;
    } else
        return -ENOTSUP;
}

long uio_write(struct uio* uio, const void* buf, unsigned long buflen) {
    if (uio->intf->write != NULL) {
        if (0 <= (long)buflen)
            return uio->intf->write(uio, buf, buflen);
        else
            return -EINVAL;
    } else
        return -ENOTSUP;
}

int uio_cntl(struct uio* uio, int op, void* arg) {
    if (uio->intf->cntl != NULL)
        return uio->intf->cntl(uio, op, arg);
    else
        return -ENOTSUP;
}

unsigned long uio_refcnt(const struct uio* uio) {
    assert(uio != NULL);
    return uio->refcnt;
}

int uio_addref(struct uio* uio) { return ++uio->refcnt; }

struct uio* create_null_uio(void) {
    static const struct uio_intf nulluio_intf = {
        .close = &nulluio_close, .read = &nulluio_read, .write = &nulluio_write, .cntl = NULL};

    static struct uio nulluio = {.intf = &nulluio_intf, .refcnt = 0};

    uio_addref(&nulluio);
    return &nulluio;
}

static void nulluio_close(struct uio* uio) {
    (void)uio;
}

static long nulluio_read(struct uio* uio, void* buf, unsigned long bufsz) {
    (void)uio;
    (void)buf;
    (void)bufsz;
    return 0;
}

static long nulluio_write(struct uio* uio, const void* buf, unsigned long buflen) {
    (void)uio;
    (void)buf;
    return (long)buflen;
}
