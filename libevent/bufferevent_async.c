/*
 * Copyright (c) 2009 Niels Provos and Nick Mathewson
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "event-config.h"

#ifdef _EVENT_HAVE_SYS_TIME_H
#include <sys/time.h>
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#ifdef _EVENT_HAVE_STDARG_H
#include <stdarg.h>
#endif
#ifdef _EVENT_HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifdef WIN32
#include <winsock2.h>
#endif

#include "event2/util.h"
#include "event2/bufferevent.h"
#include "event2/buffer.h"
#include "event2/bufferevent_struct.h"
#include "event2/event.h"
#include "log-internal.h"
#include "mm-internal.h"
#include "bufferevent-internal.h"
#include "util-internal.h"
#include "iocp-internal.h"

/* prototypes */
static int be_async_enable(struct bufferevent *, short);
static int be_async_disable(struct bufferevent *, short);
static void be_async_destruct(struct bufferevent *);
static void be_async_adj_timeouts(struct bufferevent *);
static int be_async_flush(struct bufferevent *, short, enum bufferevent_flush_mode);

const struct bufferevent_ops bufferevent_ops_async = {
	"socket",
	0,
	be_async_enable,
	be_async_disable,
	be_async_destruct,
	be_async_adj_timeouts,
        be_async_flush,
};


struct bufferevent_async {
	struct bufferevent_private bev;
	unsigned read_in_progress : 1;
	unsigned write_in_progress : 1;
};

static inline struct bufferevent_async *
upcast(struct bufferevent *bev)
{
	struct bufferevent_async *bev_a;
	if (bev->be_ops != &bufferevent_ops_async)
		return NULL;
	bev_a = EVUTIL_UPCAST(bev, struct bufferevent_async, bev.bev);
	assert(bev_a->bev.bev.be_ops == &bufferevent_ops_async);
	return bev_a;
}

static void
bev_async_consider_writing(struct bufferevent_async *b)
{
	/* Don't write if there's a write in progress, or we do not
	 * want to write. */
	if (b->write_in_progress || !(b->bev.bev.enabled&EV_WRITE))
		return;
	/* Don't write if there's nothing to write */
	if (!evbuffer_get_length(b->bev.bev.output))
		return;

	/* XXXX doesn't respect low-water mark very well. */
	if (evbuffer_launch_write(b->bev.bev.output, -1)) {
		assert(0);/* XXX act sensibly. */
	} else {
		b->write_in_progress = 1;
	}
}

static void
bev_async_consider_reading(struct bufferevent_async *b)
{
	size_t cur_size;
	size_t read_high;
	/* Don't read if there is a read in progress, or we do not
	 * want to read. */
	if (b->read_in_progress || !(b->bev.bev.enabled&EV_READ))
		return;

	/* Don't read if we're full */
	cur_size = evbuffer_get_length(b->bev.bev.input);
	read_high = b->bev.bev.wm_read.high;
	if (cur_size >= read_high)
		return;

	if (evbuffer_launch_read(b->bev.bev.input, read_high-cur_size)) {
		assert(0);
	} else {
		b->read_in_progress = 1;
	}
}

static void
be_async_outbuf_callback(struct evbuffer *buf,
    const struct evbuffer_cb_info *cbinfo,
    void *arg)
{
	struct bufferevent *bev = arg;
	struct bufferevent_async *bev_async = upcast(bev);
	/* If we successfully wrote from the outbuf, or we added data to the
	 * outbuf and were not writing before, we may want to write now. */

	BEV_LOCK(bev);
	if (cbinfo->n_deleted) {
		/* XXXX can't detect 0-length write completion */
		bev_async->write_in_progress = 0;
	}

	if (cbinfo->n_added || cbinfo->n_deleted)
		bev_async_consider_writing(bev_async);

	if (cbinfo->n_deleted &&
	    bev->writecb != NULL &&
	    evbuffer_get_length(bev->output) <= bev->wm_write.low)
		_bufferevent_run_writecb(bev);

	BEV_UNLOCK(bev);
}

static void
be_async_inbuf_callback(struct evbuffer *buf,
    const struct evbuffer_cb_info *cbinfo,
    void *arg)
{
	struct bufferevent *bev = arg;
	struct bufferevent_async *bev_async = upcast(bev);

	/* If we successfully read into the inbuf, or we drained data from
	 * the inbuf and were not reading before, we may want to read now */

	BEV_UNLOCK(bev);
	if (cbinfo->n_added) {
		/* XXXX can't detect 0-length read completion */
		bev_async->read_in_progress = 0;
	}

	if (cbinfo->n_added || cbinfo->n_deleted)
		bev_async_consider_reading(bev_async);

	if (cbinfo->n_added &&
	    evbuffer_get_length(bev->input) >= bev->wm_read.low &&
            bev->readcb != NULL)
		_bufferevent_run_readcb(bev);

	BEV_UNLOCK(bev);
}

static int
be_async_enable(struct bufferevent *buf, short what)
{
	struct bufferevent_async *bev_async = upcast(buf);

	/* If we newly enable reading or writing, and we aren't reading or
	   writing already, consider launching a new read or write. */

	if (what & EV_READ)
		bev_async_consider_reading(bev_async);
	if (what & EV_WRITE)
		bev_async_consider_writing(bev_async);
	return 0;
}

static int
be_async_disable(struct bufferevent *bev, short what)
{
	/* XXXX If we disable reading or writing, we may want to consider
	 * canceling any in-progress read or write operation, though it might
	 * not work. */
	return 0;
}

static void
be_async_destruct(struct bufferevent *bev)
{
}
static void
be_async_adj_timeouts(struct bufferevent *bev)
{
}
static int
be_async_flush(struct bufferevent *bev, short what,
    enum bufferevent_flush_mode mode)
{
	return 0;
}
