/*
 * Flexible event queue for dispatching events
 *
 * Copyright (c) 2016 Christopher Haster
 * Distributed under the MIT license
 */
#include "equeue.h"

#include <stdlib.h>
#include <string.h>


int equeue_create(equeue_t *q, size_t size) {
    void *buffer = malloc(size);
    if (!buffer) {
        return -1;
    }

    int err = equeue_create_inplace(q, size, buffer);
    q->allocated = buffer;
    return err;
}

int equeue_create_inplace(equeue_t *q, size_t size, void *buffer) {
    q->buffer = buffer;
    q->allocated = 0;

    q->npw2 = 0;
    for (unsigned s = size; s; s >>= 1) {
        q->npw2++;
    }

    q->chunks = 0;
    q->slab.size = size;
    q->slab.data = buffer;

    q->queue = 0;
    q->breaks = 0;

    int err;
    err = equeue_sema_create(&q->eventsema);
    if (err < 0) {
        return err;
    }

    err = equeue_mutex_create(&q->queuelock);
    if (err < 0) {
        return err;
    }

    err = equeue_mutex_create(&q->memlock);
    if (err < 0) {
        return err;
    }

    return 0;
}

void equeue_destroy(equeue_t *q) {
    // call destructors on pending events
    for (struct equeue_event *es = q->queue; es; es = es->next) {
        for (struct equeue_event *e = q->queue; e; e = e->sibling) {
            if (e->dtor) {
                e->dtor(e + 1);
            }
        }
    }

    equeue_mutex_destroy(&q->memlock);
    equeue_mutex_destroy(&q->queuelock);
    equeue_sema_destroy(&q->eventsema);
    free(q->allocated);
}

// equeue chunk allocation functions
static struct equeue_event *equeue_mem_alloc(equeue_t *q, size_t size) {
    size += sizeof(struct equeue_event);
    size = (size + sizeof(void*)-1) & ~(sizeof(void*)-1);

    equeue_mutex_lock(&q->memlock);

    for (struct equeue_event **p = &q->chunks; *p; p = &(*p)->next) {
        if ((*p)->size >= size) {
            struct equeue_event *e = *p;
            if (e->sibling) {
                *p = e->sibling;
                (*p)->next = e->next;
            } else {
                *p = e->next;
            }

            equeue_mutex_unlock(&q->memlock);
            return e;
        }
    }

    if (q->slab.size >= size) {
        struct equeue_event *e = (struct equeue_event *)q->slab.data;
        q->slab.data += size;
        q->slab.size -= size;
        e->size = size;
        e->id = 1;

        equeue_mutex_unlock(&q->memlock);
        return e;
    }

    equeue_mutex_unlock(&q->memlock);
    return 0;
}

static void equeue_mem_dealloc(equeue_t *q, struct equeue_event *e) {
    equeue_mutex_lock(&q->memlock);

    struct equeue_event **p = &q->chunks;
    while (*p && (*p)->size < e->size) {
        p = &(*p)->next;
    }

    if (*p && (*p)->size == e->size) {
        e->sibling = *p;
        e->next = (*p)->next;
    } else {
        e->sibling = 0;
        e->next = *p;
    }
    *p = e;

    equeue_mutex_unlock(&q->memlock);
}

// equeue allocation functions
void *equeue_alloc(equeue_t *q, size_t size) {
    struct equeue_event *e = equeue_mem_alloc(q, size);
    if (!e) {
        return 0;
    }

    e->target = 0;
    e->period = -1;
    e->dtor = 0;

    return e + 1;
}

void equeue_dealloc(equeue_t *q, void *p) {
    struct equeue_event *e = (struct equeue_event*)p - 1;

    if (e->dtor) {
        e->dtor(e+1);
    }

    equeue_mem_dealloc(q, e);
}

// equeue scheduling functions
static inline int equeue_tickdiff(unsigned a, unsigned b) {
    return (int)(a - b);
}

static void equeue_enqueue(equeue_t *q, struct equeue_event *e, unsigned ms) {
    e->target = equeue_tick() + ms;

    struct equeue_event **p = &q->queue;
    while (*p && equeue_tickdiff((*p)->target, e->target) < 0) {
        p = &(*p)->next;
    }

    if (*p && (*p)->target == e->target) {
        if (*p) {
            (*p)->ref = &e->sibling;
        }
        e->sibling = *p;

        if ((*p)->next) {
            (*p)->next->ref = &e->next;
        }
        e->next = (*p)->next;
    } else {
        if (*p) {
            (*p)->ref = &e->next;
        }
        e->next = *p;

        e->sibling = 0;
    }

    e->ref = p;
    *p = e;
}

static void equeue_unqueue(equeue_t *q, struct equeue_event *e) {
    if (e->sibling) {
        if (e->next) {
            e->next->ref = &e->sibling->next;
        }
        e->sibling->next = e->next;

        e->sibling->ref = e->ref;
        *e->ref = e->sibling;
    } else {
        if (e->next) {
            e->next->ref = e->ref;
        }
        *e->ref = e->next;
    }
}

static struct equeue_event *equeue_dequeue(equeue_t *q, int *deadline) {
    unsigned target = equeue_tick();
    struct equeue_event *head = 0;
    struct equeue_event **tail = &head;

    while (q->queue) {
        int diff = equeue_tickdiff(q->queue->target, target);
        if (diff > 0) {
            *deadline = diff;
            break;
        }

        struct equeue_event *es = q->queue;
        q->queue = es->next;

        struct equeue_event *prev = 0;
        for (struct equeue_event *e = es; e; e = e->sibling) {
            e->next = prev;
            prev = e;
        }

        *tail = prev;
        tail = &es->next;
    }

    if (q->queue) {
        q->queue->ref = &q->queue;
    }

    return head;
}

static inline int equeue_incid(equeue_t *q, int id) {
    if ((id+1) >> (8*sizeof(int)-1 - q->npw2)) {
        return 1;
    }

    return id+1;
}

int equeue_post(equeue_t *q, void (*cb)(void*), void *p) {
    struct equeue_event *e = (struct equeue_event*)p - 1;
    int id = (e->id << q->npw2) | ((unsigned char *)e - q->buffer);
    e->cb = cb;

    if (e->target < 0) {
        equeue_dealloc(q, e+1);
        return id;
    }

    equeue_mutex_lock(&q->queuelock);
    equeue_enqueue(q, e, e->target);
    equeue_mutex_unlock(&q->queuelock);

    equeue_sema_signal(&q->eventsema);
    return id;
}

void equeue_cancel(equeue_t *q, int id) {
    struct equeue_event *e = (struct equeue_event *)
            &q->buffer[id & ((1 << q->npw2)-1)];

    equeue_mutex_lock(&q->queuelock);
    if (e->id == -id >> q->npw2) {
        e->cb = 0;
        e->period = -1;
    }

    if (e->id != id >> q->npw2) {
        equeue_mutex_unlock(&q->queuelock);
        return;
    }

    equeue_unqueue(q, e);
    e->id = equeue_incid(q, e->id);
    equeue_mutex_unlock(&q->queuelock);

    equeue_dealloc(q, e+1);
}

void equeue_break(equeue_t *q) {
    equeue_mutex_lock(&q->queuelock);
    q->breaks++;
    equeue_mutex_unlock(&q->queuelock);
    equeue_sema_signal(&q->eventsema);
}

void equeue_dispatch(equeue_t *q, int ms) {
    unsigned timeout = equeue_tick() + ms;

    while (1) {
        // collect all the available events and next deadline
        struct equeue_event *es = 0;
        int deadline = -1;
        if (q->queue) {
            equeue_mutex_lock(&q->queuelock);
            es = equeue_dequeue(q, &deadline);

            // mark events as in-flight
            for (struct equeue_event *e = es; e; e = e->next) {
                e->id = -e->id;
            }
            equeue_mutex_unlock(&q->queuelock);
        }

        // dispatch events
        while (es) {
            struct equeue_event *e = es;
            es = e->next;

            // actually dispatch the callbacks
            void (*cb)(void *) = e->cb;
            if (cb) {
                cb(e + 1);
            }

            // undirty the id and either dealloc or reenqueue periodic events
            if (e->period >= 0) {
                equeue_mutex_lock(&q->queuelock);
                e->id = -e->id;
                equeue_enqueue(q, e, e->period);
                equeue_mutex_unlock(&q->queuelock);

                equeue_sema_signal(&q->eventsema);
            } else {
                e->id = equeue_incid(q, -e->id);
                equeue_dealloc(q, e+1);
            }
        }

        // check if we should stop dispatching soon
        if (ms >= 0) {
            int diff = equeue_tickdiff(timeout, equeue_tick());
            if (diff <= 0) {
                return;
            }

            if (deadline < 0 || diff < deadline) {
                deadline = diff;
            }
        }

        // wait for events
        equeue_sema_wait(&q->eventsema, deadline);

        // check if we were notified to break out of dispatch
        if (q->breaks) {
            equeue_mutex_lock(&q->queuelock);
            if (q->breaks > 0) {
                q->breaks--;
                equeue_mutex_unlock(&q->queuelock);
                return;
            }
            equeue_mutex_unlock(&q->queuelock);
        }
    }
}

// event functions
void equeue_event_delay(void *p, int ms) {
    struct equeue_event *e = (struct equeue_event*)p - 1;
    e->target = ms;
}

void equeue_event_period(void *p, int ms) {
    struct equeue_event *e = (struct equeue_event*)p - 1;
    e->period = ms;
}

void equeue_event_dtor(void *p, void (*dtor)(void *)) {
    struct equeue_event *e = (struct equeue_event*)p - 1;
    e->dtor = dtor;
}

// simple callbacks 
struct ecallback {
    void (*cb)(void*);
    void *data;
};

static void ecallback_dispatch(void *p) {
    struct ecallback *e = (struct ecallback*)p;
    e->cb(e->data);
}

int equeue_call(equeue_t *q, void (*cb)(void*), void *data) {
    struct ecallback *e = equeue_alloc(q, sizeof(struct ecallback));
    if (!e) {
        return 0;
    }

    e->cb = cb;
    e->data = data;
    return equeue_post(q, ecallback_dispatch, e);
}

int equeue_call_in(equeue_t *q, int ms, void (*cb)(void*), void *data) {
    struct ecallback *e = equeue_alloc(q, sizeof(struct ecallback));
    if (!e) {
        return 0;
    }

    equeue_event_delay(e, ms);
    e->cb = cb;
    e->data = data;
    return equeue_post(q, ecallback_dispatch, e);
}

int equeue_call_every(equeue_t *q, int ms, void (*cb)(void*), void *data) {
    struct ecallback *e = equeue_alloc(q, sizeof(struct ecallback));
    if (!e) {
        return 0;
    }

    equeue_event_delay(e, ms);
    equeue_event_period(e, ms);
    e->cb = cb;
    e->data = data;
    return equeue_post(q, ecallback_dispatch, e);
}
