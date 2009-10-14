/* $Id: evdns.c 6979 2006-08-04 18:31:13Z nickm $ */

/* The original version of this module was written by Adam Langley; for
 * a history of modifications, check out the subversion logs.
 *
 * When editing this module, try to keep it re-mergeable by Adam.  Don't
 * reformat the whitespace, add Tor dependencies, or so on.
 *
 * TODO:
 *   - Support IPv6 and PTR records.
 *   - Replace all externally visible magic numbers with #defined constants.
 *   - Write doccumentation for APIs of all external functions.
 */

/* Async DNS Library
 * Adam Langley <agl@imperialviolet.org>
 * http://www.imperialviolet.org/eventdns.html
 * Public Domain code
 *
 * This software is Public Domain. To view a copy of the public domain dedication,
 * visit http://creativecommons.org/licenses/publicdomain/ or send a letter to
 * Creative Commons, 559 Nathan Abbott Way, Stanford, California 94305, USA.
 *
 * I ask and expect, but do not require, that all derivative works contain an
 * attribution similar to:
 *	Parts developed by Adam Langley <agl@imperialviolet.org>
 *
 * You may wish to replace the word "Parts" with something else depending on
 * the amount of original code.
 *
 * (Derivative works does not include programs which link against, run or include
 * the source verbatim in their source distributions)
 *
 * Version: 0.1b
 */

#include <sys/types.h>
#ifdef HAVE_CONFIG_H
#include "event-config.h"
#endif

#ifdef DNS_USE_FTIME_FOR_ID
#include <sys/timeb.h>
#endif

#ifndef _EVENT_DNS_USE_CPU_CLOCK_FOR_ID
#ifndef _EVENT_DNS_USE_GETTIMEOFDAY_FOR_ID
#ifndef DNS_USE_OPENSSL_FOR_ID
#ifndef DNS_USE_FTIME_FOR_ID
#error Must configure at least one id generation method.
#error Please see the documentation.
#endif
#endif
#endif
#endif

/* #define _POSIX_C_SOURCE 200507 */
#define _GNU_SOURCE
/* for strtok_r */
#define _REENTRANT

#ifdef _EVENT_DNS_USE_CPU_CLOCK_FOR_ID
#ifdef DNS_USE_OPENSSL_FOR_ID
#error Multiple id options selected
#endif
#ifdef _EVENT_DNS_USE_GETTIMEOFDAY_FOR_ID
#error Multiple id options selected
#endif
#include <time.h>
#endif

#ifdef DNS_USE_OPENSSL_FOR_ID
#ifdef _EVENT_DNS_USE_GETTIMEOFDAY_FOR_ID
#error Multiple id options selected
#endif
#include <openssl/rand.h>
#endif

#ifndef _FORTIFY_SOURCE
#define _FORTIFY_SOURCE 3
#endif

#include <string.h>
#include <fcntl.h>
#ifdef _EVENT_HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#ifdef _EVENT_HAVE_STDINT_H
#include <stdint.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#ifdef _EVENT_HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <limits.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdarg.h>
#ifdef WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <event2/dns.h>
#include <event2/dns_struct.h>
#include <event2/dns_compat.h>
#include <event2/util.h>
#include <event2/event.h>
#include <event2/event_struct.h>
#include <event2/thread.h>

#include "defer-internal.h"
#include "log-internal.h"
#include "mm-internal.h"
#include "strlcpy-internal.h"
#include "ipv6-internal.h"
#include "util-internal.h"
#include "evthread-internal.h"
#ifdef WIN32
#include <ctype.h>
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <io.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

#ifdef _EVENT_HAVE_NETINET_IN6_H
#include <netinet/in6.h>
#endif

#define EVDNS_LOG_DEBUG 0
#define EVDNS_LOG_WARN 1

#ifndef HOST_NAME_MAX
#define HOST_NAME_MAX 255
#endif

#include <stdio.h>

#undef MIN
#define MIN(a,b) ((a)<(b)?(a):(b))

#ifdef __USE_ISOC99B
/* libevent doesn't work without this */
typedef unsigned int uint;
#endif

#define u64 ev_uint64_t
#define u32 ev_uint32_t
#define u16 ev_uint16_t
#define u8  ev_uint8_t

#ifdef WIN32
#define open _open
#define read _read
#define close _close
#endif

#define MAX_ADDRS 32  /* maximum number of addresses from a single packet */
/* which we bother recording */

#define TYPE_A	       EVDNS_TYPE_A
#define TYPE_CNAME     5
#define TYPE_PTR       EVDNS_TYPE_PTR
#define TYPE_AAAA      EVDNS_TYPE_AAAA

#define CLASS_INET     EVDNS_CLASS_INET

struct evdns_request {
	u8 *request;  /* the dns packet data */
	unsigned int request_len;
	int reissue_count;
	int tx_count;  /* the number of times that this packet has been sent */
	unsigned int request_type; /* TYPE_PTR or TYPE_A */
	void *user_pointer;  /* the pointer given to us for this request */
	evdns_callback_type user_callback;
	struct nameserver *ns;	/* the server which we last sent it */

	/* elements used by the searching code */
	int search_index;
	struct search_state *search_state;
	char *search_origname;	/* needs to be free()ed */
	int search_flags;

	/* these objects are kept in a circular list */
	struct evdns_request *next, *prev;

	struct event timeout_event;

	u16 trans_id;  /* the transaction id */
	char request_appended;	/* true if the request pointer is data which follows this struct */
	char transmit_me;  /* needs to be transmitted */

	struct evdns_base *base;
};

struct reply {
	unsigned int type;
	unsigned int have_answer;
	union {
		struct {
			u32 addrcount;
			u32 addresses[MAX_ADDRS];
		} a;
		struct {
			u32 addrcount;
			struct in6_addr addresses[MAX_ADDRS];
		} aaaa;
		struct {
			char name[HOST_NAME_MAX];
		} ptr;
	} data;
};

struct nameserver {
	evutil_socket_t socket;	 /* a connected UDP socket */
	struct sockaddr_storage address;
	ev_socklen_t addrlen;
	int failed_times;  /* number of times which we have given this server a chance */
	int timedout;  /* number of times in a row a request has timed out */
	struct event event;
	/* these objects are kept in a circular list */
	struct nameserver *next, *prev;
	struct event timeout_event;  /* used to keep the timeout for */
				     /* when we next probe this server. */
				     /* Valid if state == 0 */
	/* Outstanding probe request for this nameserver, if any */
	struct evdns_request *probe_request;
	char state;  /* zero if we think that this server is down */
	char choked;  /* true if we have an EAGAIN from this server's socket */
	char write_waiting;  /* true if we are waiting for EV_WRITE events */
	struct evdns_base *base;
};


/* Represents a local port where we're listening for DNS requests. Right now, */
/* only UDP is supported. */
struct evdns_server_port {
	evutil_socket_t socket; /* socket we use to read queries and write replies. */
	int refcnt; /* reference count. */
	char choked; /* Are we currently blocked from writing? */
	char closing; /* Are we trying to close this port, pending writes? */
	evdns_request_callback_fn_type user_callback; /* Fn to handle requests */
	void *user_data; /* Opaque pointer passed to user_callback */
	struct event event; /* Read/write event */
	/* circular list of replies that we want to write. */
	struct server_request *pending_replies;
	struct event_base *event_base;

#ifndef _EVENT_DISABLE_THREAD_SUPPORT
	void *lock;
	int lock_count;
#endif
};

/* Represents part of a reply being built.	(That is, a single RR.) */
struct server_reply_item {
	struct server_reply_item *next; /* next item in sequence. */
	char *name; /* name part of the RR */
	u16 type; /* The RR type */
	u16 class; /* The RR class (usually CLASS_INET) */
	u32 ttl; /* The RR TTL */
	char is_name; /* True iff data is a label */
	u16 datalen; /* Length of data; -1 if data is a label */
	void *data; /* The contents of the RR */
};

/* Represents a request that we've received as a DNS server, and holds */
/* the components of the reply as we're constructing it. */
struct server_request {
	/* Pointers to the next and previous entries on the list of replies */
	/* that we're waiting to write.	 Only set if we have tried to respond */
	/* and gotten EAGAIN. */
	struct server_request *next_pending;
	struct server_request *prev_pending;

	u16 trans_id; /* Transaction id. */
	struct evdns_server_port *port; /* Which port received this request on? */
	struct sockaddr_storage addr; /* Where to send the response */
	ev_socklen_t addrlen; /* length of addr */

	int n_answer; /* how many answer RRs have been set? */
	int n_authority; /* how many authority RRs have been set? */
	int n_additional; /* how many additional RRs have been set? */

	struct server_reply_item *answer; /* linked list of answer RRs */
	struct server_reply_item *authority; /* linked list of authority RRs */
	struct server_reply_item *additional; /* linked list of additional RRs */

	/* Constructed response.  Only set once we're ready to send a reply. */
	/* Once this is set, the RR fields are cleared, and no more should be set. */
	char *response;
	size_t response_len;

	/* Caller-visible fields: flags, questions. */
	struct evdns_server_request base;
};

struct evdns_base {
	/* An array of n_req_heads circular lists for inflight requests.
	 * Each inflight request req is in req_heads[req->trans_id % n_req_heads].
	 */
	struct evdns_request **req_heads;
	/* A circular list of requests that we're waiting to send, but haven't
	 * sent yet because there are too many requests inflight */
	struct evdns_request *req_waiting_head;
	/* A circular list of nameservers. */
	struct nameserver *server_head;
	int n_req_heads;

	struct event_base *event_base;

	/* The number of good nameservers that we have */
	int global_good_nameservers;

	/* inflight requests are contained in the req_head list */
	/* and are actually going out across the network */
	int global_requests_inflight;
	/* requests which aren't inflight are in the waiting list */
	/* and are counted here */
	int global_requests_waiting;

	int global_max_requests_inflight;

	struct timeval global_timeout;	/* 5 seconds */
	int global_max_reissues;  /* a reissue occurs when we get some errors from the server */
	int global_max_retransmits;  /* number of times we'll retransmit a request which timed out */
	/* number of timeouts in a row before we consider this server to be down */
	int global_max_nameserver_timeout;
	/* true iff we will use the 0x20 hack to prevent poisoning attacks. */
	int global_randomize_case;

	/** Port to bind to for outgoing DNS packets. */
	struct sockaddr_storage global_outgoing_address;
	/** ev_socklen_t for global_outgoing_address. 0 if it isn't set. */
	ev_socklen_t global_outgoing_addrlen;

	struct search_state *global_search_state;

#ifndef _EVENT_DISABLE_THREAD_SUPPORT
	void *lock;
	int lock_count;
#endif
};

static struct evdns_base *current_base = NULL;

/* Given a pointer to an evdns_server_request, get the corresponding */
/* server_request. */
#define TO_SERVER_REQUEST(base_ptr)					\
	((struct server_request*)					\
	  (((char*)(base_ptr) - evutil_offsetof(struct server_request, base))))

#define REQ_HEAD(base, id) ((base)->req_heads[id % (base)->n_req_heads])

/* These are the timeout values for nameservers. If we find a nameserver is down */
/* we try to probe it at intervals as given below. Values are in seconds. */
static const struct timeval global_nameserver_timeouts[] = {{10, 0}, {60, 0}, {300, 0}, {900, 0}, {3600, 0}};
static const int global_nameserver_timeouts_length = sizeof(global_nameserver_timeouts)/sizeof(struct timeval);

static struct nameserver *nameserver_pick(struct evdns_base *base);
static void evdns_request_insert(struct evdns_request *req, struct evdns_request **head);
static void evdns_request_remove(struct evdns_request *req, struct evdns_request **head);
static void nameserver_ready_callback(evutil_socket_t fd, short events, void *arg);
static int evdns_transmit(struct evdns_base *base);
static int evdns_request_transmit(struct evdns_request *req);
static void nameserver_send_probe(struct nameserver *const ns);
static void search_request_finished(struct evdns_request *const);
static int search_try_next(struct evdns_request *const req);
static struct evdns_request *search_request_new(struct evdns_base *base, int type, const char *const name, int flags, evdns_callback_type user_callback, void *user_arg);
static void evdns_requests_pump_waiting_queue(struct evdns_base *base);
static u16 transaction_id_pick(struct evdns_base *base);
static struct evdns_request *request_new(struct evdns_base *base, int type, const char *name, int flags, evdns_callback_type callback, void *ptr);
static void request_submit(struct evdns_request *const req);

static int server_request_free(struct server_request *req);
static void server_request_free_answers(struct server_request *req);
static void server_port_free(struct evdns_server_port *port);
static void server_port_ready_callback(evutil_socket_t fd, short events, void *arg);
static int evdns_base_resolv_conf_parse_impl(struct evdns_base *base, int flags, const char *const filename);
static int evdns_base_set_option_impl(struct evdns_base *base,
    const char *option, const char *val, int flags);
static void evdns_base_free_and_unlock(struct evdns_base *base, int fail_requests);

static int strtoint(const char *const str);

#ifdef _EVENT_DISABLE_THREAD_SUPPORT
#define EVDNS_LOCK(base)  _EVUTIL_NIL_STMT
#define EVDNS_UNLOCK(base) _EVUTIL_NIL_STMT
#define ASSERT_LOCKED(base) _EVUTIL_NIL_STMT
#else
#define EVDNS_LOCK(base)						\
	do {								\
		if ((base)->lock) {					\
			EVLOCK_LOCK((base)->lock, EVTHREAD_WRITE);	\
		}							\
		++(base)->lock_count;					\
	} while (0)
#define EVDNS_UNLOCK(base)						\
	do {								\
		assert((base)->lock_count > 0);				\
		--(base)->lock_count;					\
		if ((base)->lock) {					\
			EVLOCK_UNLOCK((base)->lock, EVTHREAD_WRITE);	\
		}							\
	} while (0)
#define ASSERT_LOCKED(base) assert((base)->lock_count > 0)
#endif

#define CLOSE_SOCKET(s) EVUTIL_CLOSESOCKET(s)

static const char *
debug_ntoa(u32 address)
{
	static char buf[32];
	u32 a = ntohl(address);
	evutil_snprintf(buf, sizeof(buf), "%d.%d.%d.%d",
					(int)(u8)((a>>24)&0xff),
					(int)(u8)((a>>16)&0xff),
					(int)(u8)((a>>8 )&0xff),
					(int)(u8)((a	)&0xff));
	return buf;
}

static const char *
debug_ntop(const struct sockaddr *sa)
{
	if (sa->sa_family == AF_INET) {
		struct sockaddr_in *sin = (struct sockaddr_in *) sa;
		return debug_ntoa(sin->sin_addr.s_addr);
	}
#ifdef AF_INET6
	if (sa->sa_family == AF_INET6) {
		static char buf[128];
		struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *) sa;
		const char *result;
		result = evutil_inet_ntop(AF_INET6, &sin6->sin6_addr, buf, sizeof(buf));
		return result ? result : "unknown";
	}
#endif
	return "<unknown>";
}

static evdns_debug_log_fn_type evdns_log_fn = NULL;

void
evdns_set_log_fn(evdns_debug_log_fn_type fn)
{
  evdns_log_fn = fn;
}

#ifdef __GNUC__
#define EVDNS_LOG_CHECK	 __attribute__ ((format(printf, 2, 3)))
#else
#define EVDNS_LOG_CHECK
#endif

static void _evdns_log(int warn, const char *fmt, ...) EVDNS_LOG_CHECK;
static void
_evdns_log(int warn, const char *fmt, ...)
{
  va_list args;
  static char buf[512];
  if (!evdns_log_fn)
    return;
  va_start(args,fmt);
  evutil_vsnprintf(buf, sizeof(buf), fmt, args);
  evdns_log_fn(warn, buf);
  va_end(args);
}

#define log _evdns_log

/* This walks the list of inflight requests to find the */
/* one with a matching transaction id. Returns NULL on */
/* failure */
static struct evdns_request *
request_find_from_trans_id(struct evdns_base *base, u16 trans_id) {
	struct evdns_request *req = REQ_HEAD(base, trans_id);
	struct evdns_request *const started_at = req;

	ASSERT_LOCKED(base);

	if (req) {
		do {
			if (req->trans_id == trans_id) return req;
			req = req->next;
		} while (req != started_at);
	}

	return NULL;
}

/* a libevent callback function which is called when a nameserver */
/* has gone down and we want to test if it has came back to life yet */
static void
nameserver_prod_callback(evutil_socket_t fd, short events, void *arg) {
	struct nameserver *const ns = (struct nameserver *) arg;
	(void)fd;
	(void)events;

	EVDNS_LOCK(ns->base);
	nameserver_send_probe(ns);
	EVDNS_UNLOCK(ns->base);
}

/* a libevent callback which is called when a nameserver probe (to see if */
/* it has come back to life) times out. We increment the count of failed_times */
/* and wait longer to send the next probe packet. */
static void
nameserver_probe_failed(struct nameserver *const ns) {
	const struct timeval * timeout;

	ASSERT_LOCKED(ns->base);
	(void) evtimer_del(&ns->timeout_event);
	if (ns->state == 1) {
		/* This can happen if the nameserver acts in a way which makes us mark */
		/* it as bad and then starts sending good replies. */
		return;
	}

	timeout =
		&global_nameserver_timeouts[MIN(ns->failed_times,
										global_nameserver_timeouts_length - 1)];
	ns->failed_times++;

	if (evtimer_add(&ns->timeout_event, (struct timeval *) timeout) < 0) {
	  log(EVDNS_LOG_WARN,
	      "Error from libevent when adding timer event for %s",
	      debug_ntop((struct sockaddr *)&ns->address));
	  /* ???? Do more? */
	}
}

/* called when a nameserver has been deemed to have failed. For example, too */
/* many packets have timed out etc */
static void
nameserver_failed(struct nameserver *const ns, const char *msg) {
	struct evdns_request *req, *started_at;
	struct evdns_base *base = ns->base;
	int i;

	ASSERT_LOCKED(base);
	/* if this nameserver has already been marked as failed */
	/* then don't do anything */
	if (!ns->state) return;

	log(EVDNS_LOG_WARN, "Nameserver %s has failed: %s",
		debug_ntop((struct sockaddr*)&ns->address), msg);
	base->global_good_nameservers--;
	assert(base->global_good_nameservers >= 0);
	if (base->global_good_nameservers == 0) {
		log(EVDNS_LOG_WARN, "All nameservers have failed");
	}

	ns->state = 0;
	ns->failed_times = 1;

	if (evtimer_add(&ns->timeout_event, (struct timeval *) &global_nameserver_timeouts[0]) < 0) {
		log(EVDNS_LOG_WARN,
		    "Error from libevent when adding timer event for %s",
		    debug_ntop((struct sockaddr*)&ns->address));
		/* ???? Do more? */
	}

	/* walk the list of inflight requests to see if any can be reassigned to */
	/* a different server. Requests in the waiting queue don't have a */
	/* nameserver assigned yet */

	/* if we don't have *any* good nameservers then there's no point */
	/* trying to reassign requests to one */
	if (!base->global_good_nameservers) return;

	for (i = 0; i < base->n_req_heads; ++i) {
		req = started_at = base->req_heads[i];
		if (req) {
			do {
				if (req->tx_count == 0 && req->ns == ns) {
					/* still waiting to go out, can be moved */
					/* to another server */
					req->ns = nameserver_pick(base);
				}
				req = req->next;
			} while (req != started_at);
		}
	}
}

static void
nameserver_up(struct nameserver *const ns) {
	EVDNS_LOCK(ns->base);
	if (ns->state) return;
	log(EVDNS_LOG_WARN, "Nameserver %s is back up",
	    debug_ntop((struct sockaddr *)&ns->address));
	evtimer_del(&ns->timeout_event);
	if (ns->probe_request) {
		evdns_cancel_request(ns->base, ns->probe_request);
		ns->probe_request = NULL;
	}
	ns->state = 1;
	ns->failed_times = 0;
	ns->timedout = 0;
	ns->base->global_good_nameservers++;
}

static void
request_trans_id_set(struct evdns_request *const req, const u16 trans_id) {
	req->trans_id = trans_id;
	*((u16 *) req->request) = htons(trans_id);
}

/* Called to remove a request from a list and dealloc it. */
/* head is a pointer to the head of the list it should be */
/* removed from or NULL if the request isn't in a list. */
static void
request_finished(struct evdns_request *const req, struct evdns_request **head) {
	struct evdns_base *base = req->base;
	int was_inflight = (head != &base->req_waiting_head);
	EVDNS_LOCK(base);
	if (head)
		evdns_request_remove(req, head);

	log(EVDNS_LOG_DEBUG, "Removing timeout for request %lx",
	    (unsigned long) req);
	search_request_finished(req);
	if (was_inflight) {
		evtimer_del(&req->timeout_event);
		base->global_requests_inflight--;
	} else {
		base->global_requests_waiting--;
	}

	if (!req->request_appended) {
		/* need to free the request data on it's own */
		mm_free(req->request);
	} else {
		/* the request data is appended onto the header */
		/* so everything gets free()ed when we: */
	}

	mm_free(req);

	evdns_requests_pump_waiting_queue(base);
	EVDNS_UNLOCK(base);
}

/* This is called when a server returns a funny error code. */
/* We try the request again with another server. */
/* */
/* return: */
/*   0 ok */
/*   1 failed/reissue is pointless */
static int
request_reissue(struct evdns_request *req) {
	const struct nameserver *const last_ns = req->ns;
	ASSERT_LOCKED(req->base);
	/* the last nameserver should have been marked as failing */
	/* by the caller of this function, therefore pick will try */
	/* not to return it */
	req->ns = nameserver_pick(req->base);
	if (req->ns == last_ns) {
		/* ... but pick did return it */
		/* not a lot of point in trying again with the */
		/* same server */
		return 1;
	}

	req->reissue_count++;
	req->tx_count = 0;
	req->transmit_me = 1;

	return 0;
}

/* this function looks for space on the inflight queue and promotes */
/* requests from the waiting queue if it can. */
static void
evdns_requests_pump_waiting_queue(struct evdns_base *base) {
	ASSERT_LOCKED(base);
	while (base->global_requests_inflight < base->global_max_requests_inflight &&
		   base->global_requests_waiting) {
		struct evdns_request *req;
		/* move a request from the waiting queue to the inflight queue */
		assert(base->req_waiting_head);
		req = base->req_waiting_head;
		evdns_request_remove(req, &base->req_waiting_head);

		base->global_requests_waiting--;
		base->global_requests_inflight++;

		req->ns = nameserver_pick(base);
		request_trans_id_set(req, transaction_id_pick(base));

		evdns_request_insert(req, &REQ_HEAD(base, req->trans_id));
		evdns_request_transmit(req);
		evdns_transmit(base);
	}
}

/* TODO(nickm) document */
struct deferred_reply_callback {
	struct deferred_cb deferred;
	u8 request_type;
	u8 have_reply;
	u32 ttl;
	u32 err;
	evdns_callback_type user_callback;
	struct reply reply;
};

static void
reply_run_callback(struct deferred_cb *d, void *user_pointer)
{
	struct deferred_reply_callback *cb =
	    EVUTIL_UPCAST(d, struct deferred_reply_callback, deferred);

	switch (cb->request_type) {
	case TYPE_A:
		if (cb->have_reply)
			cb->user_callback(DNS_ERR_NONE, DNS_IPv4_A,
			    cb->reply.data.a.addrcount, cb->ttl,
			    cb->reply.data.a.addresses,
			    user_pointer);
		else
			cb->user_callback(cb->err, 0, 0, 0, NULL, user_pointer);
		break;
	case TYPE_PTR:
		if (cb->have_reply) {
			char *name = cb->reply.data.ptr.name;
			cb->user_callback(DNS_ERR_NONE, DNS_PTR, 1, cb->ttl,
			    &name, user_pointer);
		} else {
			cb->user_callback(cb->err, 0, 0, 0, NULL, user_pointer);
		}
		break;
	case TYPE_AAAA:
		if (cb->have_reply)
			cb->user_callback(DNS_ERR_NONE, DNS_IPv6_AAAA,
			    cb->reply.data.aaaa.addrcount, cb->ttl,
			    cb->reply.data.aaaa.addresses,
			    user_pointer);
		else
			cb->user_callback(cb->err, 0, 0, 0, NULL, user_pointer);
		break;
	default:
		assert(0);
	}

	mm_free(cb);
}

static void
reply_schedule_callback(struct evdns_request *const req, u32 ttl, u32 err, struct reply *reply)
{
	struct deferred_reply_callback *d = mm_calloc(1, sizeof(*d));

	ASSERT_LOCKED(req->base);

	d->request_type = req->request_type;
	d->user_callback = req->user_callback;
	d->ttl = ttl;
	d->err = err;
	if (reply) {
		d->have_reply = 1;
		memcpy(&d->reply, reply, sizeof(struct reply));
	}

	event_deferred_cb_init(&d->deferred, reply_run_callback,
	    req->user_pointer);
	event_deferred_cb_schedule(
		event_base_get_deferred_cb_queue(req->base->event_base),
		&d->deferred);
}

/* this processes a parsed reply packet */
static void
reply_handle(struct evdns_request *const req, u16 flags, u32 ttl, struct reply *reply) {
	int error;
	static const int error_codes[] = {
		DNS_ERR_FORMAT, DNS_ERR_SERVERFAILED, DNS_ERR_NOTEXIST,
		DNS_ERR_NOTIMPL, DNS_ERR_REFUSED
	};

	ASSERT_LOCKED(req->base);

	if (flags & 0x020f || !reply || !reply->have_answer) {
		/* there was an error */
		if (flags & 0x0200) {
			error = DNS_ERR_TRUNCATED;
		} else {
			u16 error_code = (flags & 0x000f) - 1;
			if (error_code > 4) {
				error = DNS_ERR_UNKNOWN;
			} else {
				error = error_codes[error_code];
			}
		}

		switch(error) {
		case DNS_ERR_NOTIMPL:
		case DNS_ERR_REFUSED:
			/* we regard these errors as marking a bad nameserver */
			if (req->reissue_count < req->base->global_max_reissues) {
				char msg[64];
				evutil_snprintf(msg, sizeof(msg), "Bad response %d (%s)",
					 error, evdns_err_to_string(error));
				nameserver_failed(req->ns, msg);
				if (!request_reissue(req)) return;
			}
			break;
		case DNS_ERR_SERVERFAILED:
			/* rcode 2 (servfailed) sometimes means "we
			 * are broken" and sometimes (with some binds)
			 * means "that request was very confusing."
			 * Treat this as a timeout, not a failure.
			 */
			log(EVDNS_LOG_DEBUG, "Got a SERVERFAILED from nameserver %s; "
				"will allow the request to time out.",
				debug_ntop((struct sockaddr *)&req->ns->address));
			break;
		default:
			/* we got a good reply from the nameserver */
			nameserver_up(req->ns);
		}

		if (req->search_state && req->request_type != TYPE_PTR) {
			/* if we have a list of domains to search in,
			 * try the next one */
			if (!search_try_next(req)) {
				/* a new request was issued so this
				 * request is finished and */
				/* the user callback will be made when
				 * that request (or a */
				/* child of it) finishes. */
				request_finished(req, &REQ_HEAD(req->base, req->trans_id));
				return;
			}
		}

		/* all else failed. Pass the failure up */
		reply_schedule_callback(req, 0, error, NULL);
		request_finished(req, &REQ_HEAD(req->base, req->trans_id));
	} else {
		/* all ok, tell the user */
		reply_schedule_callback(req, ttl, 0, reply);
		if (req == req->ns->probe_request)
			req->ns->probe_request = NULL; /* Avoid double-free */
		nameserver_up(req->ns);
		request_finished(req, &REQ_HEAD(req->base, req->trans_id));
	}
}

static int
name_parse(u8 *packet, int length, int *idx, char *name_out, int name_out_len) {
	int name_end = -1;
	int j = *idx;
	int ptr_count = 0;
#define GET32(x) do { if (j + 4 > length) goto err; memcpy(&_t32, packet + j, 4); j += 4; x = ntohl(_t32); } while(0)
#define GET16(x) do { if (j + 2 > length) goto err; memcpy(&_t, packet + j, 2); j += 2; x = ntohs(_t); } while(0)
#define GET8(x) do { if (j >= length) goto err; x = packet[j++]; } while(0)

	char *cp = name_out;
	const char *const end = name_out + name_out_len;

	/* Normally, names are a series of length prefixed strings terminated */
	/* with a length of 0 (the lengths are u8's < 63). */
	/* However, the length can start with a pair of 1 bits and that */
	/* means that the next 14 bits are a pointer within the current */
	/* packet. */

	for(;;) {
		u8 label_len;
		if (j >= length) return -1;
		GET8(label_len);
		if (!label_len) break;
		if (label_len & 0xc0) {
			u8 ptr_low;
			GET8(ptr_low);
			if (name_end < 0) name_end = j;
			j = (((int)label_len & 0x3f) << 8) + ptr_low;
			/* Make sure that the target offset is in-bounds. */
			if (j < 0 || j >= length) return -1;
			/* If we've jumped more times than there are characters in the
			 * message, we must have a loop. */
			if (++ptr_count > length) return -1;
			continue;
		}
		if (label_len > 63) return -1;
		if (cp != name_out) {
			if (cp + 1 >= end) return -1;
			*cp++ = '.';
		}
		if (cp + label_len >= end) return -1;
		memcpy(cp, packet + j, label_len);
		cp += label_len;
		j += label_len;
	}
	if (cp >= end) return -1;
	*cp = '\0';
	if (name_end < 0)
		*idx = j;
	else
		*idx = name_end;
	return 0;
 err:
	return -1;
}

/* parses a raw request from a nameserver */
static int
reply_parse(struct evdns_base *base, u8 *packet, int length) {
	int j = 0, k = 0;  /* index into packet */
	u16 _t;	 /* used by the macros */
	u32 _t32;  /* used by the macros */
	char tmp_name[256], cmp_name[256]; /* used by the macros */
	int name_matches = 0;

	u16 trans_id, questions, answers, authority, additional, datalength;
	u16 flags = 0;
	u32 ttl, ttl_r = 0xffffffff;
	struct reply reply;
	struct evdns_request *req = NULL;
	unsigned int i;

	ASSERT_LOCKED(base);

	GET16(trans_id);
	GET16(flags);
	GET16(questions);
	GET16(answers);
	GET16(authority);
	GET16(additional);
	(void) authority; /* suppress "unused variable" warnings. */
	(void) additional; /* suppress "unused variable" warnings. */

	req = request_find_from_trans_id(base, trans_id);
	if (!req) return -1;
	assert(req->base == base);

	memset(&reply, 0, sizeof(reply));

	/* If it's not an answer, it doesn't correspond to any request. */
	if (!(flags & 0x8000)) return -1;  /* must be an answer */
	if (flags & 0x020f) {
		/* there was an error */
		goto err;
	}
	/* if (!answers) return; */  /* must have an answer of some form */

	/* This macro skips a name in the DNS reply. */
#define SKIP_NAME						\
	do { tmp_name[0] = '\0';				\
		if (name_parse(packet, length, &j, tmp_name,	\
			sizeof(tmp_name))<0)			\
			goto err;				\
	} while(0)
#define TEST_NAME							\
	do { tmp_name[0] = '\0';					\
		cmp_name[0] = '\0';					\
		k = j;							\
		if (name_parse(packet, length, &j, tmp_name,		\
			sizeof(tmp_name))<0)				\
			goto err;					\
		if (name_parse(req->request, req->request_len, &k,	\
			cmp_name, sizeof(cmp_name))<0)			\
			goto err;					\
		if (base->global_randomize_case) {			\
			if (strcmp(tmp_name, cmp_name) == 0)		\
				name_matches = 1;			\
		} else {						\
			if (evutil_ascii_strcasecmp(tmp_name, cmp_name) == 0) \
				name_matches = 1;			\
		}							\
	} while(0)

	reply.type = req->request_type;

	/* skip over each question in the reply */
	for (i = 0; i < questions; ++i) {
		/* the question looks like
		 *   <label:name><u16:type><u16:class>
		 */
		TEST_NAME;
		j += 4;
		if (j >= length) goto err;
	}

	if (!name_matches)
		goto err;

	/* now we have the answer section which looks like
	 * <label:name><u16:type><u16:class><u32:ttl><u16:len><data...>
	 */

	for (i = 0; i < answers; ++i) {
		u16 type, class;

		SKIP_NAME;
		GET16(type);
		GET16(class);
		GET32(ttl);
		GET16(datalength);

		if (type == TYPE_A && class == CLASS_INET) {
			int addrcount, addrtocopy;
			if (req->request_type != TYPE_A) {
				j += datalength; continue;
			}
			if ((datalength & 3) != 0) /* not an even number of As. */
			    goto err;
			addrcount = datalength >> 2;
			addrtocopy = MIN(MAX_ADDRS - reply.data.a.addrcount, (unsigned)addrcount);

			ttl_r = MIN(ttl_r, ttl);
			/* we only bother with the first four addresses. */
			if (j + 4*addrtocopy > length) goto err;
			memcpy(&reply.data.a.addresses[reply.data.a.addrcount],
				   packet + j, 4*addrtocopy);
			j += 4*addrtocopy;
			reply.data.a.addrcount += addrtocopy;
			reply.have_answer = 1;
			if (reply.data.a.addrcount == MAX_ADDRS) break;
		} else if (type == TYPE_PTR && class == CLASS_INET) {
			if (req->request_type != TYPE_PTR) {
				j += datalength; continue;
			}
			if (name_parse(packet, length, &j, reply.data.ptr.name,
						   sizeof(reply.data.ptr.name))<0)
				goto err;
			ttl_r = MIN(ttl_r, ttl);
			reply.have_answer = 1;
			break;
		} else if (type == TYPE_AAAA && class == CLASS_INET) {
			int addrcount, addrtocopy;
			if (req->request_type != TYPE_AAAA) {
				j += datalength; continue;
			}
			if ((datalength & 15) != 0) /* not an even number of AAAAs. */
				goto err;
			addrcount = datalength >> 4;  /* each address is 16 bytes long */
			addrtocopy = MIN(MAX_ADDRS - reply.data.aaaa.addrcount, (unsigned)addrcount);
			ttl_r = MIN(ttl_r, ttl);

			/* we only bother with the first four addresses. */
			if (j + 16*addrtocopy > length) goto err;
			memcpy(&reply.data.aaaa.addresses[reply.data.aaaa.addrcount],
				   packet + j, 16*addrtocopy);
			reply.data.aaaa.addrcount += addrtocopy;
			j += 16*addrtocopy;
			reply.have_answer = 1;
			if (reply.data.aaaa.addrcount == MAX_ADDRS) break;
		} else {
			/* skip over any other type of resource */
			j += datalength;
		}
	}

	reply_handle(req, flags, ttl_r, &reply);
	return 0;
 err:
	if (req)
		reply_handle(req, flags, 0, NULL);
	return -1;
}

/* Parse a raw request (packet,length) sent to a nameserver port (port) from */
/* a DNS client (addr,addrlen), and if it's well-formed, call the corresponding */
/* callback. */
static int
request_parse(u8 *packet, int length, struct evdns_server_port *port, struct sockaddr *addr, ev_socklen_t addrlen)
{
	int j = 0;	/* index into packet */
	u16 _t;	 /* used by the macros */
	char tmp_name[256]; /* used by the macros */

	int i;
	u16 trans_id, flags, questions, answers, authority, additional;
	struct server_request *server_req = NULL;

	ASSERT_LOCKED(port);

	/* Get the header fields */
	GET16(trans_id);
	GET16(flags);
	GET16(questions);
	GET16(answers);
	GET16(authority);
	GET16(additional);

	if (flags & 0x8000) return -1; /* Must not be an answer. */
	flags &= 0x0110; /* Only RD and CD get preserved. */

	server_req = mm_malloc(sizeof(struct server_request));
	if (server_req == NULL) return -1;
	memset(server_req, 0, sizeof(struct server_request));

	server_req->trans_id = trans_id;
	memcpy(&server_req->addr, addr, addrlen);
	server_req->addrlen = addrlen;

	server_req->base.flags = flags;
	server_req->base.nquestions = 0;
	server_req->base.questions = mm_malloc(sizeof(struct evdns_server_question *) * questions);
	if (server_req->base.questions == NULL)
		goto err;

	for (i = 0; i < questions; ++i) {
		u16 type, class;
		struct evdns_server_question *q;
		int namelen;
		if (name_parse(packet, length, &j, tmp_name, sizeof(tmp_name))<0)
			goto err;
		GET16(type);
		GET16(class);
		namelen = strlen(tmp_name);
		q = mm_malloc(sizeof(struct evdns_server_question) + namelen);
		if (!q)
			goto err;
		q->type = type;
		q->dns_question_class = class;
		memcpy(q->name, tmp_name, namelen+1);
		server_req->base.questions[server_req->base.nquestions++] = q;
	}

	/* Ignore answers, authority, and additional. */

	server_req->port = port;
	port->refcnt++;

	/* Only standard queries are supported. */
	if (flags & 0x7800) {
		evdns_server_request_respond(&(server_req->base), DNS_ERR_NOTIMPL);
		return -1;
	}

	port->user_callback(&(server_req->base), port->user_data);

	return 0;
err:
	if (server_req) {
		if (server_req->base.questions) {
			for (i = 0; i < server_req->base.nquestions; ++i)
				mm_free(server_req->base.questions[i]);
			mm_free(server_req->base.questions);
		}
		mm_free(server_req);
	}
	return -1;

#undef SKIP_NAME
#undef GET32
#undef GET16
#undef GET8
}

static u16
default_transaction_id_fn(void)
{
	u16 trans_id;
#ifdef _EVENT_DNS_USE_CPU_CLOCK_FOR_ID
	struct timespec ts;
	static int clkid = -1;
	if (clkid == -1) {
		clkid = CLOCK_REALTIME;
#ifdef CLOCK_MONOTONIC
		if (clock_gettime(CLOCK_MONOTONIC, &ts) != -1)
			clkid = CLOCK_MONOTONIC;
#endif
	}
	if (clock_gettime(clkid, &ts) == -1)
		event_err(1, "clock_gettime");
	trans_id = ts.tv_nsec & 0xffff;
#endif

#ifdef DNS_USE_FTIME_FOR_ID
	struct _timeb tb;
	_ftime(&tb);
	trans_id = tb.millitm & 0xffff;
#endif

#ifdef _EVENT_DNS_USE_GETTIMEOFDAY_FOR_ID
	struct timeval tv;
	evutil_gettimeofday(&tv, NULL);
	trans_id = tv.tv_usec & 0xffff;
#endif

#ifdef DNS_USE_OPENSSL_FOR_ID
	if (RAND_pseudo_bytes((u8 *) &trans_id, 2) == -1) {
		/* in the case that the RAND call fails we used to back */
		/* down to using gettimeofday. */
		/*
		  struct timeval tv;
		  gettimeofday(&tv, NULL);
		  trans_id = tv.tv_usec & 0xffff;
		*/
		abort();
	}
#endif
	return trans_id;
}

static ev_uint16_t (*trans_id_function)(void) = default_transaction_id_fn;

static void
default_random_bytes_fn(char *buf, size_t n)
{
	unsigned i;
	for (i = 0; i < n; i += 2) {
		u16 tid = trans_id_function();
		buf[i] = (tid >> 8) & 0xff;
		if (i+1<n)
			buf[i+1] = tid & 0xff;
	}
}

static void (*rand_bytes_function)(char *buf, size_t n) =
	default_random_bytes_fn;

static u16
trans_id_from_random_bytes_fn(void)
{
	u16 tid;
	rand_bytes_function((char*) &tid, sizeof(tid));
	return tid;
}

void
evdns_set_transaction_id_fn(ev_uint16_t (*fn)(void))
{
	if (fn)
		trans_id_function = fn;
	else
		trans_id_function = default_transaction_id_fn;
	rand_bytes_function = default_random_bytes_fn;
}

void
evdns_set_random_bytes_fn(void (*fn)(char *, size_t))
{
	rand_bytes_function = fn;
	trans_id_function = trans_id_from_random_bytes_fn;
}

/* Try to choose a strong transaction id which isn't already in flight */
static u16
transaction_id_pick(struct evdns_base *base) {
	ASSERT_LOCKED(base);
	for (;;) {
		u16 trans_id = trans_id_function();

		if (trans_id == 0xffff) continue;
		/* now check to see if that id is already inflight */
		if (request_find_from_trans_id(base, trans_id) == NULL)
			return trans_id;
	}
}

/* choose a namesever to use. This function will try to ignore */
/* nameservers which we think are down and load balance across the rest */
/* by updating the server_head global each time. */
static struct nameserver *
nameserver_pick(struct evdns_base *base) {
	struct nameserver *started_at = base->server_head, *picked;
	ASSERT_LOCKED(base);
	if (!base->server_head) return NULL;

	/* if we don't have any good nameservers then there's no */
	/* point in trying to find one. */
	if (!base->global_good_nameservers) {
		base->server_head = base->server_head->next;
		return base->server_head;
	}

	/* remember that nameservers are in a circular list */
	for (;;) {
		if (base->server_head->state) {
			/* we think this server is currently good */
			picked = base->server_head;
			base->server_head = base->server_head->next;
			return picked;
		}

		base->server_head = base->server_head->next;
		if (base->server_head == started_at) {
			/* all the nameservers seem to be down */
			/* so we just return this one and hope for the */
			/* best */
			assert(base->global_good_nameservers == 0);
			picked = base->server_head;
			base->server_head = base->server_head->next;
			return picked;
		}
	}
}

/* this is called when a namesever socket is ready for reading */
static void
nameserver_read(struct nameserver *ns) {
	struct sockaddr_storage ss;
	ev_socklen_t addrlen = sizeof(ss);
	u8 packet[1500];
	ASSERT_LOCKED(ns->base);

	for (;;) {
		const int r = recvfrom(ns->socket, packet, sizeof(packet), 0,
		    (struct sockaddr*)&ss, &addrlen);
		if (r < 0) {
			int err = evutil_socket_geterror(ns->socket);
			if (EVUTIL_ERR_RW_RETRIABLE(err))
				return;
			nameserver_failed(ns,
			    evutil_socket_error_to_string(err));
			return;
		}
		if (evutil_sockaddr_cmp((struct sockaddr*)&ss,
			(struct sockaddr*)&ns->address, 0)) {
			log(EVDNS_LOG_WARN, "Address mismatch on received "
			    "DNS packet.  Apparent source was %s",
			    debug_ntop((struct sockaddr*)&ss));
			return;
		}

		ns->timedout = 0;
		reply_parse(ns->base, packet, r);
	}
}

/* Read a packet from a DNS client on a server port s, parse it, and */
/* act accordingly. */
static void
server_port_read(struct evdns_server_port *s) {
	u8 packet[1500];
	struct sockaddr_storage addr;
	ev_socklen_t addrlen;
	int r;
	ASSERT_LOCKED(s);

	for (;;) {
		addrlen = sizeof(struct sockaddr_storage);
		r = recvfrom(s->socket, packet, sizeof(packet), 0,
					 (struct sockaddr*) &addr, &addrlen);
		if (r < 0) {
			int err = evutil_socket_geterror(s->socket);
			if (EVUTIL_ERR_RW_RETRIABLE(err))
				return;
			log(EVDNS_LOG_WARN, "Error %s (%d) while reading request.",
				evutil_socket_error_to_string(err), err);
			return;
		}
		request_parse(packet, r, s, (struct sockaddr*) &addr, addrlen);
	}
}

/* Try to write all pending replies on a given DNS server port. */
static void
server_port_flush(struct evdns_server_port *port)
{
	ASSERT_LOCKED(port);
	while (port->pending_replies) {
		struct server_request *req = port->pending_replies;
		int r = sendto(port->socket, req->response, req->response_len, 0,
			   (struct sockaddr*) &req->addr, req->addrlen);
		if (r < 0) {
			int err = evutil_socket_geterror(port->socket);
			if (EVUTIL_ERR_RW_RETRIABLE(err))
				return;
			log(EVDNS_LOG_WARN, "Error %s (%d) while writing response to port; dropping", evutil_socket_error_to_string(err), err);
		}
		if (server_request_free(req)) {
			/* we released the last reference to req->port. */
			return;
		}
	}

	/* We have no more pending requests; stop listening for 'writeable' events. */
	(void) event_del(&port->event);
	event_assign(&port->event, port->event_base,
				 port->socket, EV_READ | EV_PERSIST,
				 server_port_ready_callback, port);

	if (event_add(&port->event, NULL) < 0) {
		log(EVDNS_LOG_WARN, "Error from libevent when adding event for DNS server.");
		/* ???? Do more? */
	}
}

/* set if we are waiting for the ability to write to this server. */
/* if waiting is true then we ask libevent for EV_WRITE events, otherwise */
/* we stop these events. */
static void
nameserver_write_waiting(struct nameserver *ns, char waiting) {
	ASSERT_LOCKED(ns->base);
	if (ns->write_waiting == waiting) return;

	ns->write_waiting = waiting;
	(void) event_del(&ns->event);
	event_assign(&ns->event, ns->base->event_base,
	    ns->socket, EV_READ | (waiting ? EV_WRITE : 0) | EV_PERSIST,
	    nameserver_ready_callback, ns);
	if (event_add(&ns->event, NULL) < 0) {
	  log(EVDNS_LOG_WARN, "Error from libevent when adding event for %s",
	      debug_ntop((struct sockaddr *)&ns->address));
	  /* ???? Do more? */
	}
}

/* a callback function. Called by libevent when the kernel says that */
/* a nameserver socket is ready for writing or reading */
static void
nameserver_ready_callback(evutil_socket_t fd, short events, void *arg) {
	struct nameserver *ns = (struct nameserver *) arg;
	(void)fd;

	EVDNS_LOCK(ns->base);
	if (events & EV_WRITE) {
		ns->choked = 0;
		if (!evdns_transmit(ns->base)) {
			nameserver_write_waiting(ns, 0);
		}
	}
	if (events & EV_READ) {
		nameserver_read(ns);
	}
	EVDNS_UNLOCK(ns->base);
}

/* a callback function. Called by libevent when the kernel says that */
/* a server socket is ready for writing or reading. */
static void
server_port_ready_callback(evutil_socket_t fd, short events, void *arg) {
	struct evdns_server_port *port = (struct evdns_server_port *) arg;
	(void) fd;

	EVDNS_LOCK(port);
	if (events & EV_WRITE) {
		port->choked = 0;
		server_port_flush(port);
	}
	if (events & EV_READ) {
		server_port_read(port);
	}
	EVDNS_UNLOCK(port);
}

/* This is an inefficient representation; only use it via the dnslabel_table_*
 * functions, so that is can be safely replaced with something smarter later. */
#define MAX_LABELS 128
/* Structures used to implement name compression */
struct dnslabel_entry { char *v; off_t pos; };
struct dnslabel_table {
	int n_labels; /* number of current entries */
	/* map from name to position in message */
	struct dnslabel_entry labels[MAX_LABELS];
};

/* Initialize dnslabel_table. */
static void
dnslabel_table_init(struct dnslabel_table *table)
{
	table->n_labels = 0;
}

/* Free all storage held by table, but not the table itself. */
static void
dnslabel_clear(struct dnslabel_table *table)
{
	int i;
	for (i = 0; i < table->n_labels; ++i)
		mm_free(table->labels[i].v);
	table->n_labels = 0;
}

/* return the position of the label in the current message, or -1 if the label */
/* hasn't been used yet. */
static int
dnslabel_table_get_pos(const struct dnslabel_table *table, const char *label)
{
	int i;
	for (i = 0; i < table->n_labels; ++i) {
		if (!strcmp(label, table->labels[i].v))
			return table->labels[i].pos;
	}
	return -1;
}

/* remember that we've used the label at position pos */
static int
dnslabel_table_add(struct dnslabel_table *table, const char *label, off_t pos)
{
	char *v;
	int p;
	if (table->n_labels == MAX_LABELS)
		return (-1);
	v = mm_strdup(label);
	if (v == NULL)
		return (-1);
	p = table->n_labels++;
	table->labels[p].v = v;
	table->labels[p].pos = pos;

	return (0);
}

/* Converts a string to a length-prefixed set of DNS labels, starting */
/* at buf[j]. name and buf must not overlap. name_len should be the length */
/* of name.	 table is optional, and is used for compression. */
/* */
/* Input: abc.def */
/* Output: <3>abc<3>def<0> */
/* */
/* Returns the first index after the encoded name, or negative on error. */
/*	 -1	 label was > 63 bytes */
/*	 -2	 name too long to fit in buffer. */
/* */
static off_t
dnsname_to_labels(u8 *const buf, size_t buf_len, off_t j,
				  const char *name, const int name_len,
				  struct dnslabel_table *table) {
	const char *end = name + name_len;
	int ref = 0;
	u16 _t;

#define APPEND16(x) do {						\
		if (j + 2 > (off_t)buf_len)				\
			goto overflow;					\
		_t = htons(x);						\
		memcpy(buf + j, &_t, 2);				\
		j += 2;							\
	} while (0)
#define APPEND32(x) do {						\
		if (j + 4 > (off_t)buf_len)				\
			goto overflow;					\
		_t32 = htonl(x);					\
		memcpy(buf + j, &_t32, 4);				\
		j += 4;							\
	} while (0)

	if (name_len > 255) return -2;

	for (;;) {
		const char *const start = name;
		if (table && (ref = dnslabel_table_get_pos(table, name)) >= 0) {
			APPEND16(ref | 0xc000);
			return j;
		}
		name = strchr(name, '.');
		if (!name) {
			const unsigned int label_len = end - start;
			if (label_len > 63) return -1;
			if ((size_t)(j+label_len+1) > buf_len) return -2;
			if (table) dnslabel_table_add(table, start, j);
			buf[j++] = label_len;

			memcpy(buf + j, start, end - start);
			j += end - start;
			break;
		} else {
			/* append length of the label. */
			const unsigned int label_len = name - start;
			if (label_len > 63) return -1;
			if ((size_t)(j+label_len+1) > buf_len) return -2;
			if (table) dnslabel_table_add(table, start, j);
			buf[j++] = label_len;

			memcpy(buf + j, start, name - start);
			j += name - start;
			/* hop over the '.' */
			name++;
		}
	}

	/* the labels must be terminated by a 0. */
	/* It's possible that the name ended in a . */
	/* in which case the zero is already there */
	if (!j || buf[j-1]) buf[j++] = 0;
	return j;
 overflow:
	return (-2);
}

/* Finds the length of a dns request for a DNS name of the given */
/* length. The actual request may be smaller than the value returned */
/* here */
static size_t
evdns_request_len(const size_t name_len) {
	return 96 + /* length of the DNS standard header */
		name_len + 2 +
		4;  /* space for the resource type */
}

/* build a dns request packet into buf. buf should be at least as long */
/* as evdns_request_len told you it should be. */
/* */
/* Returns the amount of space used. Negative on error. */
static int
evdns_request_data_build(const char *const name, const int name_len,
    const u16 trans_id, const u16 type, const u16 class,
    u8 *const buf, size_t buf_len) {
	off_t j = 0;  /* current offset into buf */
	u16 _t;	 /* used by the macros */

	APPEND16(trans_id);
	APPEND16(0x0100);  /* standard query, recusion needed */
	APPEND16(1);  /* one question */
	APPEND16(0);  /* no answers */
	APPEND16(0);  /* no authority */
	APPEND16(0);  /* no additional */

	j = dnsname_to_labels(buf, buf_len, j, name, name_len, NULL);
	if (j < 0) {
		return (int)j;
	}

	APPEND16(type);
	APPEND16(class);

	return (int)j;
 overflow:
	return (-1);
}

/* exported function */
struct evdns_server_port *
evdns_add_server_port_with_base(struct event_base *base, evutil_socket_t socket, int is_tcp, evdns_request_callback_fn_type cb, void *user_data)
{
	struct evdns_server_port *port;
	if (!(port = mm_malloc(sizeof(struct evdns_server_port))))
		return NULL;
	memset(port, 0, sizeof(struct evdns_server_port));

	assert(!is_tcp); /* TCP sockets not yet implemented */
	port->socket = socket;
	port->refcnt = 1;
	port->choked = 0;
	port->closing = 0;
	port->user_callback = cb;
	port->user_data = user_data;
	port->pending_replies = NULL;
	port->event_base = base;

	event_assign(&port->event, port->event_base,
				 port->socket, EV_READ | EV_PERSIST,
				 server_port_ready_callback, port);
	if (event_add(&port->event, NULL) < 0) {
		mm_free(port);
		return NULL;
	}
	EVTHREAD_ALLOC_LOCK(port->lock);
	return port;
}

struct evdns_server_port *
evdns_add_server_port(evutil_socket_t socket, int is_tcp, evdns_request_callback_fn_type cb, void *user_data)
{
	return evdns_add_server_port_with_base(NULL, socket, is_tcp, cb, user_data);
}

/* exported function */
void
evdns_close_server_port(struct evdns_server_port *port)
{
	EVDNS_LOCK(port);
	if (--port->refcnt == 0) {
		EVDNS_UNLOCK(port);
		server_port_free(port);
	} else {
		port->closing = 1;
	}
}

/* exported function */
int
evdns_server_request_add_reply(struct evdns_server_request *_req, int section, const char *name, int type, int class, int ttl, int datalen, int is_name, const char *data)
{
	struct server_request *req = TO_SERVER_REQUEST(_req);
	struct server_reply_item **itemp, *item;
	int *countp;
	int result = -1;

	EVDNS_LOCK(req->port);
	if (req->response) /* have we already answered? */
		goto done;

	switch (section) {
	case EVDNS_ANSWER_SECTION:
		itemp = &req->answer;
		countp = &req->n_answer;
		break;
	case EVDNS_AUTHORITY_SECTION:
		itemp = &req->authority;
		countp = &req->n_authority;
		break;
	case EVDNS_ADDITIONAL_SECTION:
		itemp = &req->additional;
		countp = &req->n_additional;
		break;
	default:
		goto done;
	}
	while (*itemp) {
		itemp = &((*itemp)->next);
	}
	item = mm_malloc(sizeof(struct server_reply_item));
	if (!item)
		goto done;
	item->next = NULL;
	if (!(item->name = mm_strdup(name))) {
		mm_free(item);
		goto done;
	}
	item->type = type;
	item->dns_question_class = class;
	item->ttl = ttl;
	item->is_name = is_name != 0;
	item->datalen = 0;
	item->data = NULL;
	if (data) {
		if (item->is_name) {
			if (!(item->data = mm_strdup(data))) {
				mm_free(item->name);
				mm_free(item);
				goto done;
			}
			item->datalen = (u16)-1;
		} else {
			if (!(item->data = mm_malloc(datalen))) {
				mm_free(item->name);
				mm_free(item);
				goto done;
			}
			item->datalen = datalen;
			memcpy(item->data, data, datalen);
		}
	}

	*itemp = item;
	++(*countp);
	result = 0;
done:
	EVDNS_UNLOCK(req->port);
	return result;
}

/* exported function */
int
evdns_server_request_add_a_reply(struct evdns_server_request *req, const char *name, int n, void *addrs, int ttl)
{
	return evdns_server_request_add_reply(
		  req, EVDNS_ANSWER_SECTION, name, TYPE_A, CLASS_INET,
		  ttl, n*4, 0, addrs);
}

/* exported function */
int
evdns_server_request_add_aaaa_reply(struct evdns_server_request *req, const char *name, int n, void *addrs, int ttl)
{
	return evdns_server_request_add_reply(
		  req, EVDNS_ANSWER_SECTION, name, TYPE_AAAA, CLASS_INET,
		  ttl, n*16, 0, addrs);
}

/* exported function */
int
evdns_server_request_add_ptr_reply(struct evdns_server_request *req, struct in_addr *in, const char *inaddr_name, const char *hostname, int ttl)
{
	u32 a;
	char buf[32];
	assert(in || inaddr_name);
	assert(!(in && inaddr_name));
	if (in) {
		a = ntohl(in->s_addr);
		evutil_snprintf(buf, sizeof(buf), "%d.%d.%d.%d.in-addr.arpa",
				(int)(u8)((a	)&0xff),
				(int)(u8)((a>>8 )&0xff),
				(int)(u8)((a>>16)&0xff),
				(int)(u8)((a>>24)&0xff));
		inaddr_name = buf;
	}
	return evdns_server_request_add_reply(
		  req, EVDNS_ANSWER_SECTION, inaddr_name, TYPE_PTR, CLASS_INET,
		  ttl, -1, 1, hostname);
}

/* exported function */
int
evdns_server_request_add_cname_reply(struct evdns_server_request *req, const char *name, const char *cname, int ttl)
{
	return evdns_server_request_add_reply(
		  req, EVDNS_ANSWER_SECTION, name, TYPE_CNAME, CLASS_INET,
		  ttl, -1, 1, cname);
}

/* exported function */
void
evdns_server_request_set_flags(struct evdns_server_request *exreq, int flags)
{
	struct server_request *req = TO_SERVER_REQUEST(exreq);
	req->base.flags &= ~(EVDNS_FLAGS_AA|EVDNS_FLAGS_RD);
	req->base.flags |= flags;
}

static int
evdns_server_request_format_response(struct server_request *req, int err)
{
	unsigned char buf[1500];
	size_t buf_len = sizeof(buf);
	off_t j = 0, r;
	u16 _t;
	u32 _t32;
	int i;
	u16 flags;
	struct dnslabel_table table;

	if (err < 0 || err > 15) return -1;

	/* Set response bit and error code; copy OPCODE and RD fields from
	 * question; copy RA and AA if set by caller. */
	flags = req->base.flags;
	flags |= (0x8000 | err);

	dnslabel_table_init(&table);
	APPEND16(req->trans_id);
	APPEND16(flags);
	APPEND16(req->base.nquestions);
	APPEND16(req->n_answer);
	APPEND16(req->n_authority);
	APPEND16(req->n_additional);

	/* Add questions. */
	for (i=0; i < req->base.nquestions; ++i) {
		const char *s = req->base.questions[i]->name;
		j = dnsname_to_labels(buf, buf_len, j, s, strlen(s), &table);
		if (j < 0) {
			dnslabel_clear(&table);
			return (int) j;
		}
		APPEND16(req->base.questions[i]->type);
		APPEND16(req->base.questions[i]->dns_question_class);
	}

	/* Add answer, authority, and additional sections. */
	for (i=0; i<3; ++i) {
		struct server_reply_item *item;
		if (i==0)
			item = req->answer;
		else if (i==1)
			item = req->authority;
		else
			item = req->additional;
		while (item) {
			r = dnsname_to_labels(buf, buf_len, j, item->name, strlen(item->name), &table);
			if (r < 0)
				goto overflow;
			j = r;

			APPEND16(item->type);
			APPEND16(item->dns_question_class);
			APPEND32(item->ttl);
			if (item->is_name) {
				off_t len_idx = j, name_start;
				j += 2;
				name_start = j;
				r = dnsname_to_labels(buf, buf_len, j, item->data, strlen(item->data), &table);
				if (r < 0)
					goto overflow;
				j = r;
				_t = htons( (short) (j-name_start) );
				memcpy(buf+len_idx, &_t, 2);
			} else {
				APPEND16(item->datalen);
				if (j+item->datalen > (off_t)buf_len)
					goto overflow;
				memcpy(buf+j, item->data, item->datalen);
				j += item->datalen;
			}
			item = item->next;
		}
	}

	if (j > 512) {
overflow:
		j = 512;
		buf[2] |= 0x02; /* set the truncated bit. */
	}

	req->response_len = j;

	if (!(req->response = mm_malloc(req->response_len))) {
		server_request_free_answers(req);
		dnslabel_clear(&table);
		return (-1);
	}
	memcpy(req->response, buf, req->response_len);
	server_request_free_answers(req);
	dnslabel_clear(&table);
	return (0);
}

/* exported function */
int
evdns_server_request_respond(struct evdns_server_request *_req, int err)
{
	struct server_request *req = TO_SERVER_REQUEST(_req);
	struct evdns_server_port *port = req->port;
	int r = -1;

	EVDNS_LOCK(port);
	if (!req->response) {
		if ((r = evdns_server_request_format_response(req, err))<0)
			goto done;
	}

	r = sendto(port->socket, req->response, req->response_len, 0,
			   (struct sockaddr*) &req->addr, req->addrlen);
	if (r<0) {
		int sock_err = evutil_socket_geterror(port->socket);
		if (EVUTIL_ERR_RW_RETRIABLE(sock_err))
			goto done;

		if (port->pending_replies) {
			req->prev_pending = port->pending_replies->prev_pending;
			req->next_pending = port->pending_replies;
			req->prev_pending->next_pending =
				req->next_pending->prev_pending = req;
		} else {
			req->prev_pending = req->next_pending = req;
			port->pending_replies = req;
			port->choked = 1;

			(void) event_del(&port->event);
			event_assign(&port->event, port->event_base, port->socket, (port->closing?0:EV_READ) | EV_WRITE | EV_PERSIST, server_port_ready_callback, port);

			if (event_add(&port->event, NULL) < 0) {
				log(EVDNS_LOG_WARN, "Error from libevent when adding event for DNS server");
			}

		}

		r = 1;
		goto done;
	}
	if (server_request_free(req)) {
		r = 0;
		goto done;
	}

	if (port->pending_replies)
		server_port_flush(port);

	r = 0;
done:
	EVDNS_UNLOCK(port);
	return r;
}

/* Free all storage held by RRs in req. */
static void
server_request_free_answers(struct server_request *req)
{
	struct server_reply_item *victim, *next, **list;
	int i;
	for (i = 0; i < 3; ++i) {
		if (i==0)
			list = &req->answer;
		else if (i==1)
			list = &req->authority;
		else
			list = &req->additional;

		victim = *list;
		while (victim) {
			next = victim->next;
			mm_free(victim->name);
			if (victim->data)
				mm_free(victim->data);
			mm_free(victim);
			victim = next;
		}
		*list = NULL;
	}
}

/* Free all storage held by req, and remove links to it. */
/* return true iff we just wound up freeing the server_port. */
static int
server_request_free(struct server_request *req)
{
	int i, rc=1, lock=0;
	if (req->base.questions) {
		for (i = 0; i < req->base.nquestions; ++i)
			mm_free(req->base.questions[i]);
		mm_free(req->base.questions);
	}

	if (req->port) {
		EVDNS_LOCK(req->port);
		lock=1;
		if (req->port->pending_replies == req) {
			if (req->next_pending)
				req->port->pending_replies = req->next_pending;
			else
				req->port->pending_replies = NULL;
		}
		rc = --req->port->refcnt;
	}

	if (req->response) {
		mm_free(req->response);
	}

	server_request_free_answers(req);

	if (req->next_pending && req->next_pending != req) {
		req->next_pending->prev_pending = req->prev_pending;
		req->prev_pending->next_pending = req->next_pending;
	}

	if (rc == 0) {
		EVDNS_UNLOCK(req->port); /* ????? nickm */
		server_port_free(req->port);
		mm_free(req);
		return (1);
	}
	if (lock)
		EVDNS_UNLOCK(req->port);
	mm_free(req);
	return (0);
}

/* Free all storage held by an evdns_server_port.  Only called when  */
static void
server_port_free(struct evdns_server_port *port)
{
	assert(port);
	assert(!port->refcnt);
	assert(!port->pending_replies);
	if (port->socket > 0) {
		CLOSE_SOCKET(port->socket);
		port->socket = -1;
	}
	(void) event_del(&port->event);
	EVTHREAD_FREE_LOCK(port->lock);
	mm_free(port);
}

/* exported function */
int
evdns_server_request_drop(struct evdns_server_request *_req)
{
	struct server_request *req = TO_SERVER_REQUEST(_req);
	server_request_free(req);
	return 0;
}

/* exported function */
int
evdns_server_request_get_requesting_addr(struct evdns_server_request *_req, struct sockaddr *sa, int addr_len)
{
	struct server_request *req = TO_SERVER_REQUEST(_req);
	if (addr_len < (int)req->addrlen)
		return -1;
	memcpy(sa, &(req->addr), req->addrlen);
	return req->addrlen;
}

#undef APPEND16
#undef APPEND32

/* this is a libevent callback function which is called when a request */
/* has timed out. */
static void
evdns_request_timeout_callback(evutil_socket_t fd, short events, void *arg) {
	struct evdns_request *const req = (struct evdns_request *) arg;
	struct evdns_base *base = req->base;
	(void) fd;
	(void) events;

	log(EVDNS_LOG_DEBUG, "Request %lx timed out", (unsigned long) arg);
	EVDNS_LOCK(base);

	req->ns->timedout++;
	if (req->ns->timedout > req->base->global_max_nameserver_timeout) {
		req->ns->timedout = 0;
		nameserver_failed(req->ns, "request timed out.");
	}

	if (req->tx_count >= req->base->global_max_retransmits) {
		/* this request has failed */
		reply_schedule_callback(req, 0, DNS_ERR_TIMEOUT, NULL);
		request_finished(req, &REQ_HEAD(req->base, req->trans_id));
	} else {
		/* retransmit it */
		(void) evtimer_del(&req->timeout_event);
		evdns_request_transmit(req);
	}
	EVDNS_UNLOCK(base);
}

/* try to send a request to a given server. */
/* */
/* return: */
/*   0 ok */
/*   1 temporary failure */
/*   2 other failure */
static int
evdns_request_transmit_to(struct evdns_request *req, struct nameserver *server) {
	int r;
	ASSERT_LOCKED(req->base);
	r = sendto(server->socket, req->request, req->request_len, 0,
                (struct sockaddr *)&server->address, server->addrlen);
	if (r < 0) {
		int err = evutil_socket_geterror(server->socket);
		if (EVUTIL_ERR_RW_RETRIABLE(err))
			return 1;
		nameserver_failed(req->ns, evutil_socket_error_to_string(err));
		return 2;
	} else if (r != (int)req->request_len) {
		return 1;  /* short write */
	} else {
		return 0;
	}
}

/* try to send a request, updating the fields of the request */
/* as needed */
/* */
/* return: */
/*   0 ok */
/*   1 failed */
static int
evdns_request_transmit(struct evdns_request *req) {
	int retcode = 0, r;

	ASSERT_LOCKED(req->base);
	/* if we fail to send this packet then this flag marks it */
	/* for evdns_transmit */
	req->transmit_me = 1;
	if (req->trans_id == 0xffff) abort();

	if (req->ns->choked) {
		/* don't bother trying to write to a socket */
		/* which we have had EAGAIN from */
		return 1;
	}

	r = evdns_request_transmit_to(req, req->ns);
	switch (r) {
	case 1:
		/* temp failure */
		req->ns->choked = 1;
		nameserver_write_waiting(req->ns, 1);
		return 1;
	case 2:
		/* failed to transmit the request entirely. */
		retcode = 1;
		/* fall through: we'll set a timeout, which will time out,
		 * and make us retransmit the request anyway. */
	default:
		/* all ok */
		log(EVDNS_LOG_DEBUG,
		    "Setting timeout for request %lx", (unsigned long) req);
		if (evtimer_add(&req->timeout_event, &req->base->global_timeout) < 0) {
			log(EVDNS_LOG_WARN,
		      "Error from libevent when adding timer for request %lx",
				(unsigned long) req);
			/* ???? Do more? */
		}
		req->tx_count++;
		req->transmit_me = 0;
		return retcode;
	}
}

static void
nameserver_probe_callback(int result, char type, int count, int ttl, void *addresses, void *arg) {
	struct nameserver *const ns = (struct nameserver *) arg;
	(void) type;
	(void) count;
	(void) ttl;
	(void) addresses;

	EVDNS_LOCK(ns->base);
	ns->probe_request = NULL;
	if (result == DNS_ERR_CANCEL) {
		/* We canceled this request because the nameserver came up
		 * for some other reason.  Do not change our opinion about
		 * the nameserver. */
	} else if (result == DNS_ERR_NONE || result == DNS_ERR_NOTEXIST) {
		/* this is a good reply */
		nameserver_up(ns);
	} else {
		nameserver_probe_failed(ns);
	}
	EVDNS_UNLOCK(ns->base);
}

static void
nameserver_send_probe(struct nameserver *const ns) {
	struct evdns_request *req;
	/* here we need to send a probe to a given nameserver */
	/* in the hope that it is up now. */

	ASSERT_LOCKED(ns->base);
	log(EVDNS_LOG_DEBUG, "Sending probe to %s",
		debug_ntop((struct sockaddr *)&ns->address));

	req = request_new(ns->base, TYPE_A, "google.com", DNS_QUERY_NO_SEARCH, nameserver_probe_callback, ns);
	if (!req) return;
	ns->probe_request = req;
	/* we force this into the inflight queue no matter what */
	request_trans_id_set(req, transaction_id_pick(ns->base));
	req->ns = ns;
	request_submit(req);
}

/* returns: */
/*   0 didn't try to transmit anything */
/*   1 tried to transmit something */
static int
evdns_transmit(struct evdns_base *base) {
	char did_try_to_transmit = 0;
	int i;

	ASSERT_LOCKED(base);
	for (i = 0; i < base->n_req_heads; ++i) {
		if (base->req_heads[i]) {
			struct evdns_request *const started_at = base->req_heads[i], *req = started_at;
			/* first transmit all the requests which are currently waiting */
			do {
				if (req->transmit_me) {
					did_try_to_transmit = 1;
					evdns_request_transmit(req);
				}

				req = req->next;
			} while (req != started_at);
		}
	}

	return did_try_to_transmit;
}

/* exported function */
int
evdns_base_count_nameservers(struct evdns_base *base)
{
	const struct nameserver *server;
	int n = 0;

	EVDNS_LOCK(base);
	server = base->server_head;
	if (!server)
		goto done;
	do {
		++n;
		server = server->next;
	} while (server != base->server_head);
done:
	EVDNS_UNLOCK(base);
	return n;
}

int
evdns_count_nameservers(void)
{
	return evdns_base_count_nameservers(current_base);
}

/* exported function */
int
evdns_base_clear_nameservers_and_suspend(struct evdns_base *base)
{
	struct nameserver *server, *started_at;
	int i;

	EVDNS_LOCK(base);
	server = base->server_head;
	started_at = base->server_head;
	if (!server) {
		EVDNS_UNLOCK(base);
		return 0;
	}
	while (1) {
		struct nameserver *next = server->next;
		(void) event_del(&server->event);
		if (evtimer_initialized(&server->timeout_event))
			(void) evtimer_del(&server->timeout_event);
		if (server->socket >= 0)
			CLOSE_SOCKET(server->socket);
		mm_free(server);
		if (next == started_at)
			break;
		server = next;
	}
	base->server_head = NULL;
	base->global_good_nameservers = 0;

	for (i = 0; i < base->n_req_heads; ++i) {
		struct evdns_request *req, *req_started_at;
		req = req_started_at = base->req_heads[i];
		while (req) {
			struct evdns_request *next = req->next;
			req->tx_count = req->reissue_count = 0;
			req->ns = NULL;
			/* ???? What to do about searches? */
			(void) evtimer_del(&req->timeout_event);
			req->trans_id = 0;
			req->transmit_me = 0;

			base->global_requests_waiting++;
			evdns_request_insert(req, &base->req_waiting_head);
			/* We want to insert these suspended elements at the front of
			 * the waiting queue, since they were pending before any of
			 * the waiting entries were added.  This is a circular list,
			 * so we can just shift the start back by one.*/
			base->req_waiting_head = base->req_waiting_head->prev;

			if (next == req_started_at)
				break;
			req = next;
		}
		base->req_heads[i] = NULL;
	}

	base->global_requests_inflight = 0;

	EVDNS_UNLOCK(base);
	return 0;
}

int
evdns_clear_nameservers_and_suspend(void)
{
	return evdns_base_clear_nameservers_and_suspend(current_base);
}


/* exported function */
int
evdns_base_resume(struct evdns_base *base)
{
	EVDNS_LOCK(base);
	evdns_requests_pump_waiting_queue(base);
	EVDNS_UNLOCK(base);
	return 0;
}

int
evdns_resume(void)
{
	return evdns_base_resume(current_base);
}

static int
_evdns_nameserver_add_impl(struct evdns_base *base, const struct sockaddr *address, int addrlen) {
	/* first check to see if we already have this nameserver */

	const struct nameserver *server = base->server_head, *const started_at = base->server_head;
	struct nameserver *ns;
	int err = 0;

	ASSERT_LOCKED(base);
	if (server) {
		do {
			if (!evutil_sockaddr_cmp((struct sockaddr*)&server->address, address, 1)) return 3;
			server = server->next;
		} while (server != started_at);
	}
	if (addrlen > (int)sizeof(ns->address)) {
		log(EVDNS_LOG_DEBUG, "Addrlen %d too long.", (int)addrlen);
		return 2;
	}

	ns = (struct nameserver *) mm_malloc(sizeof(struct nameserver));
	if (!ns) return -1;

	memset(ns, 0, sizeof(struct nameserver));
	ns->base = base;

	evtimer_assign(&ns->timeout_event, ns->base->event_base, nameserver_prod_callback, ns);

	ns->socket = socket(PF_INET, SOCK_DGRAM, 0);
	if (ns->socket < 0) { err = 1; goto out1; }
	evutil_make_socket_nonblocking(ns->socket);

	if (base->global_outgoing_addrlen) {
		if (bind(ns->socket,
			(struct sockaddr*)&base->global_outgoing_address,
			base->global_outgoing_addrlen) < 0) {
			log(EVDNS_LOG_WARN,"Couldn't bind to outgoing address");
			err = 2;
			goto out2;
		}
	}

	memcpy(&ns->address, address, addrlen);
	ns->addrlen = addrlen;
	ns->state = 1;
	event_assign(&ns->event, ns->base->event_base, ns->socket, EV_READ | EV_PERSIST, nameserver_ready_callback, ns);
	if (event_add(&ns->event, NULL) < 0) {
		err = 2;
		goto out2;
	}

	log(EVDNS_LOG_DEBUG, "Added nameserver %s", debug_ntop(address));

	/* insert this nameserver into the list of them */
	if (!base->server_head) {
		ns->next = ns->prev = ns;
		base->server_head = ns;
	} else {
		ns->next = base->server_head->next;
		ns->prev = base->server_head;
		base->server_head->next = ns;
		if (base->server_head->prev == base->server_head) {
			base->server_head->prev = ns;
		}
	}

	base->global_good_nameservers++;

	return 0;

out2:
	CLOSE_SOCKET(ns->socket);
out1:
	mm_free(ns);
	log(EVDNS_LOG_WARN, "Unable to add nameserver %s: error %d", debug_ntop(address), err);
	return err;
}

/* exported function */
int
evdns_base_nameserver_add(struct evdns_base *base,
						  unsigned long int address)
{
	struct sockaddr_in sin;
	int res;
	sin.sin_addr.s_addr = address;
	sin.sin_port = htons(53);
	sin.sin_family = AF_INET;
	EVDNS_LOCK(base);
	res = _evdns_nameserver_add_impl(base, (struct sockaddr*)&sin, sizeof(sin));
	EVDNS_UNLOCK(base);
	return res;
}

int
evdns_nameserver_add(unsigned long int address) {
	if (!current_base)
		current_base = evdns_base_new(NULL, 0);
	return evdns_base_nameserver_add(current_base, address);
}

/* exported function */
int
evdns_base_nameserver_ip_add(struct evdns_base *base, const char *ip_as_string) {
	struct sockaddr_storage ss;
	struct sockaddr *sa;
	int len = sizeof(ss);
	int res;
	if (evutil_parse_sockaddr_port(ip_as_string, (struct sockaddr *)&ss,
		&len)) {
		log(EVDNS_LOG_WARN, "Unable to parse nameserver address %s",
			ip_as_string);
		return 4;
	}
	sa = (struct sockaddr *) &ss;
	if (sa->sa_family == AF_INET) {
		struct sockaddr_in *sin = (struct sockaddr_in *)sa;
		if (sin->sin_port == 0)
			sin->sin_port = htons(53);
	}
#ifdef AF_INET6
	else if (sa->sa_family == AF_INET6) {
		struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)sa;
		if (sin6->sin6_port == 0)
			sin6->sin6_port = htons(53);
	}
#endif
	else
		return -1;

	EVDNS_LOCK(base);
	res = _evdns_nameserver_add_impl(base, sa, len);
	EVDNS_UNLOCK(base);
	return res;
}

int
evdns_nameserver_ip_add(const char *ip_as_string) {
	if (!current_base)
		current_base = evdns_base_new(NULL, 0);
	return evdns_base_nameserver_ip_add(current_base, ip_as_string);
}

/* remove from the queue */
static void
evdns_request_remove(struct evdns_request *req, struct evdns_request **head)
{
	ASSERT_LOCKED(req->base);

#if 0
	{
		struct evdns_request *ptr;
		int found = 0;
		assert(*head != NULL);

		ptr = *head;
		do {
			if (ptr == req) {
				found = 1;
				break;
			}
			ptr = ptr->next;
		} while (ptr != *head);
		assert(found);

		assert(req->next);
	}
#endif

	if (req->next == req) {
		/* only item in the list */
		*head = NULL;
	} else {
		req->next->prev = req->prev;
		req->prev->next = req->next;
		if (*head == req) *head = req->next;
	}
	req->next = req->prev = NULL;
}

/* insert into the tail of the queue */
static void
evdns_request_insert(struct evdns_request *req, struct evdns_request **head) {
	ASSERT_LOCKED(req->base);
	if (!*head) {
		*head = req;
		req->next = req->prev = req;
		return;
	}

	req->prev = (*head)->prev;
	req->prev->next = req;
	req->next = *head;
	(*head)->prev = req;
}

static int
string_num_dots(const char *s) {
	int count = 0;
	while ((s = strchr(s, '.'))) {
		s++;
		count++;
	}
	return count;
}

static struct evdns_request *
request_new(struct evdns_base *base, int type, const char *name, int flags,
    evdns_callback_type callback, void *user_ptr) {

	const char issuing_now =
	    (base->global_requests_inflight < base->global_max_requests_inflight) ? 1 : 0;

	const size_t name_len = strlen(name);
	const size_t request_max_len = evdns_request_len(name_len);
	const u16 trans_id = issuing_now ? transaction_id_pick(base) : 0xffff;
	/* the request data is alloced in a single block with the header */
	struct evdns_request *const req =
	    mm_malloc(sizeof(struct evdns_request) + request_max_len);
	int rlen;
	char namebuf[256];
	(void) flags;

	ASSERT_LOCKED(base);

	if (!req) return NULL;

	if (name_len >= sizeof(namebuf)) {
		free(req);
		return NULL;
	}

	memset(req, 0, sizeof(struct evdns_request));
	req->base = base;

	evtimer_assign(&req->timeout_event, req->base->event_base, evdns_request_timeout_callback, req);

	if (base->global_randomize_case) {
		unsigned i;
		char randbits[(sizeof(namebuf)+7)/8];
		strlcpy(namebuf, name, sizeof(namebuf));
		rand_bytes_function(randbits, (name_len+7)/8);
		for (i = 0; i < name_len; ++i) {
			if (EVUTIL_ISALPHA(namebuf[i])) {
				if ((randbits[i >> 3] & (1<<(i & 7))))
					namebuf[i] |= 0x20;
				else
					namebuf[i] &= ~0x20;
			}
		}
		name = namebuf;
	}

	/* request data lives just after the header */
	req->request = ((u8 *) req) + sizeof(struct evdns_request);
	/* denotes that the request data shouldn't be free()ed */
	req->request_appended = 1;
	rlen = evdns_request_data_build(name, name_len, trans_id,
	    type, CLASS_INET, req->request, request_max_len);
	if (rlen < 0)
		goto err1;

	req->request_len = rlen;
	req->trans_id = trans_id;
	req->tx_count = 0;
	req->request_type = type;
	req->user_pointer = user_ptr;
	req->user_callback = callback;
	req->ns = issuing_now ? nameserver_pick(base) : NULL;
	req->next = req->prev = NULL;

	return req;
err1:
	mm_free(req);
	return NULL;
}

static void
request_submit(struct evdns_request *const req) {
	struct evdns_base *base = req->base;
	ASSERT_LOCKED(base);
	if (req->ns) {
		/* if it has a nameserver assigned then this is going */
		/* straight into the inflight queue */
		evdns_request_insert(req, &REQ_HEAD(base, req->trans_id));
		base->global_requests_inflight++;
		evdns_request_transmit(req);
	} else {
		evdns_request_insert(req, &base->req_waiting_head);
		base->global_requests_waiting++;
	}
}

/* exported function */
void
evdns_cancel_request(struct evdns_base *base, struct evdns_request *req)
{
	if (!base)
		base = req->base;

	EVDNS_LOCK(base);
	reply_schedule_callback(req, 0, DNS_ERR_CANCEL, NULL);
	if (req->ns) {
		/* remove from inflight queue */
		request_finished(req, &REQ_HEAD(base, req->trans_id));
	} else {
		/* remove from global_waiting head */
		request_finished(req, &base->req_waiting_head);
	}
	EVDNS_UNLOCK(base);
}

/* exported function */
struct evdns_request *
evdns_base_resolve_ipv4(struct evdns_base *base, const char *name, int flags,
    evdns_callback_type callback, void *ptr) {
	struct evdns_request *req;
	log(EVDNS_LOG_DEBUG, "Resolve requested for %s", name);
	EVDNS_LOCK(base);
	if (flags & DNS_QUERY_NO_SEARCH) {
		req =
			request_new(base, TYPE_A, name, flags, callback, ptr);
		if (req)
			request_submit(req);
	} else {
		req = search_request_new(base, TYPE_A, name, flags, callback, ptr);
	}
	EVDNS_UNLOCK(base);
	return req;
}

int evdns_resolve_ipv4(const char *name, int flags,
					   evdns_callback_type callback, void *ptr)
{
	return evdns_base_resolve_ipv4(current_base, name, flags, callback, ptr)
		? 0 : -1;
}


/* exported function */
struct evdns_request *
evdns_base_resolve_ipv6(struct evdns_base *base,
    const char *name, int flags,
    evdns_callback_type callback, void *ptr)
{
	struct evdns_request *req;
	log(EVDNS_LOG_DEBUG, "Resolve requested for %s", name);
	EVDNS_LOCK(base);
	if (flags & DNS_QUERY_NO_SEARCH) {
		req = request_new(base, TYPE_AAAA, name, flags, callback, ptr);
		if (req)
			request_submit(req);
	} else {
		req = search_request_new(base,TYPE_AAAA, name, flags, callback, ptr);
	}
	EVDNS_UNLOCK(base);
	return req;
}

int evdns_resolve_ipv6(const char *name, int flags,
    evdns_callback_type callback, void *ptr) {
	return evdns_base_resolve_ipv6(current_base, name, flags, callback, ptr)
		? 0 : -1;
}

struct evdns_request *
evdns_base_resolve_reverse(struct evdns_base *base, const struct in_addr *in, int flags, evdns_callback_type callback, void *ptr) {
	char buf[32];
	struct evdns_request *req;
	u32 a;
	assert(in);
	a = ntohl(in->s_addr);
	evutil_snprintf(buf, sizeof(buf), "%d.%d.%d.%d.in-addr.arpa",
			(int)(u8)((a	)&0xff),
			(int)(u8)((a>>8 )&0xff),
			(int)(u8)((a>>16)&0xff),
			(int)(u8)((a>>24)&0xff));
	log(EVDNS_LOG_DEBUG, "Resolve requested for %s (reverse)", buf);
	EVDNS_LOCK(base);
	req = request_new(base, TYPE_PTR, buf, flags, callback, ptr);
	if (req)
		request_submit(req);
	EVDNS_UNLOCK(base);
	return (req);
}

int evdns_resolve_reverse(const struct in_addr *in, int flags, evdns_callback_type callback, void *ptr) {
	return evdns_base_resolve_reverse(current_base, in, flags, callback, ptr)
		? 0 : -1;
}

struct evdns_request *
evdns_base_resolve_reverse_ipv6(struct evdns_base *base, const struct in6_addr *in, int flags, evdns_callback_type callback, void *ptr) {
	/* 32 nybbles, 32 periods, "ip6.arpa", NUL. */
	char buf[73];
	char *cp;
	struct evdns_request *req;
	int i;
	assert(in);
	cp = buf;
	for (i=15; i >= 0; --i) {
		u8 byte = in->s6_addr[i];
		*cp++ = "0123456789abcdef"[byte & 0x0f];
		*cp++ = '.';
		*cp++ = "0123456789abcdef"[byte >> 4];
		*cp++ = '.';
	}
	assert(cp + strlen("ip6.arpa") < buf+sizeof(buf));
	memcpy(cp, "ip6.arpa", strlen("ip6.arpa")+1);
	log(EVDNS_LOG_DEBUG, "Resolve requested for %s (reverse)", buf);
	EVDNS_LOCK(base);
	req = request_new(base, TYPE_PTR, buf, flags, callback, ptr);
	if (req)
		request_submit(req);
	EVDNS_UNLOCK(base);
	return (req);
}

int evdns_resolve_reverse_ipv6(const struct in6_addr *in, int flags, evdns_callback_type callback, void *ptr) {
	return evdns_base_resolve_reverse_ipv6(current_base, in, flags, callback, ptr)
		? 0 : -1;
}

/*/////////////////////////////////////////////////////////////////// */
/* Search support */
/* */
/* the libc resolver has support for searching a number of domains */
/* to find a name. If nothing else then it takes the single domain */
/* from the gethostname() call. */
/* */
/* It can also be configured via the domain and search options in a */
/* resolv.conf. */
/* */
/* The ndots option controls how many dots it takes for the resolver */
/* to decide that a name is non-local and so try a raw lookup first. */

struct search_domain {
	int len;
	struct search_domain *next;
	/* the text string is appended to this structure */
};

struct search_state {
	int refcount;
	int ndots;
	int num_domains;
	struct search_domain *head;
};

static void
search_state_decref(struct search_state *const state) {
	if (!state) return;
	state->refcount--;
	if (!state->refcount) {
		struct search_domain *next, *dom;
		for (dom = state->head; dom; dom = next) {
			next = dom->next;
			mm_free(dom);
		}
		mm_free(state);
	}
}

static struct search_state *
search_state_new(void) {
	struct search_state *state = (struct search_state *) mm_malloc(sizeof(struct search_state));
	if (!state) return NULL;
	memset(state, 0, sizeof(struct search_state));
	state->refcount = 1;
	state->ndots = 1;

	return state;
}

static void
search_postfix_clear(struct evdns_base *base) {
	search_state_decref(base->global_search_state);

	base->global_search_state = search_state_new();
}

/* exported function */
void
evdns_base_search_clear(struct evdns_base *base)
{
	EVDNS_LOCK(base);
	search_postfix_clear(base);
	EVDNS_UNLOCK(base);
}

void
evdns_search_clear(void) {
	evdns_base_search_clear(current_base);
}

static void
search_postfix_add(struct evdns_base *base, const char *domain) {
	int domain_len;
	struct search_domain *sdomain;
	while (domain[0] == '.') domain++;
	domain_len = strlen(domain);

	ASSERT_LOCKED(base);
	if (!base->global_search_state) base->global_search_state = search_state_new();
	if (!base->global_search_state) return;
	base->global_search_state->num_domains++;

	sdomain = (struct search_domain *) mm_malloc(sizeof(struct search_domain) + domain_len);
	if (!sdomain) return;
	memcpy( ((u8 *) sdomain) + sizeof(struct search_domain), domain, domain_len);
	sdomain->next = base->global_search_state->head;
	sdomain->len = domain_len;

	base->global_search_state->head = sdomain;
}

/* reverse the order of members in the postfix list. This is needed because, */
/* when parsing resolv.conf we push elements in the wrong order */
static void
search_reverse(struct evdns_base *base) {
	struct search_domain *cur, *prev = NULL, *next;
	ASSERT_LOCKED(base);
	cur = base->global_search_state->head;
	while (cur) {
		next = cur->next;
		cur->next = prev;
		prev = cur;
		cur = next;
	}

	base->global_search_state->head = prev;
}

/* exported function */
void
evdns_base_search_add(struct evdns_base *base, const char *domain) {
	EVDNS_LOCK(base);
	search_postfix_add(base, domain);
	EVDNS_UNLOCK(base);
}
void
evdns_search_add(const char *domain) {
	evdns_base_search_add(current_base, domain);
}

/* exported function */
void
evdns_base_search_ndots_set(struct evdns_base *base, const int ndots) {
	EVDNS_LOCK(base);
	if (!base->global_search_state) base->global_search_state = search_state_new();
	if (base->global_search_state)
		base->global_search_state->ndots = ndots;
	EVDNS_UNLOCK(base);
}
void
evdns_search_ndots_set(const int ndots) {
	evdns_base_search_ndots_set(current_base, ndots);
}

static void
search_set_from_hostname(struct evdns_base *base) {
	char hostname[HOST_NAME_MAX + 1], *domainname;

	ASSERT_LOCKED(base);
	search_postfix_clear(base);
	if (gethostname(hostname, sizeof(hostname))) return;
	domainname = strchr(hostname, '.');
	if (!domainname) return;
	search_postfix_add(base, domainname);
}

/* warning: returns malloced string */
static char *
search_make_new(const struct search_state *const state, int n, const char *const base_name) {
	const int base_len = strlen(base_name);
	const char need_to_append_dot = base_name[base_len - 1] == '.' ? 0 : 1;
	struct search_domain *dom;

	for (dom = state->head; dom; dom = dom->next) {
		if (!n--) {
			/* this is the postfix we want */
			/* the actual postfix string is kept at the end of the structure */
			const u8 *const postfix = ((u8 *) dom) + sizeof(struct search_domain);
			const int postfix_len = dom->len;
			char *const newname = (char *) mm_malloc(base_len + need_to_append_dot + postfix_len + 1);
			if (!newname) return NULL;
			memcpy(newname, base_name, base_len);
			if (need_to_append_dot) newname[base_len] = '.';
			memcpy(newname + base_len + need_to_append_dot, postfix, postfix_len);
			newname[base_len + need_to_append_dot + postfix_len] = 0;
			return newname;
		}
	}

	/* we ran off the end of the list and still didn't find the requested string */
	abort();
	return NULL; /* unreachable; stops warnings in some compilers. */
}

static struct evdns_request *
search_request_new(struct evdns_base *base, int type, const char *const name, int flags, evdns_callback_type user_callback, void *user_arg) {
	ASSERT_LOCKED(base);
	assert(type == TYPE_A || type == TYPE_AAAA);
	if ( ((flags & DNS_QUERY_NO_SEARCH) == 0) &&
	     base->global_search_state &&
		 base->global_search_state->num_domains) {
		/* we have some domains to search */
		struct evdns_request *req;
		if (string_num_dots(name) >= base->global_search_state->ndots) {
			req = request_new(base, type, name, flags, user_callback, user_arg);
			if (!req) return NULL;
			req->search_index = -1;
		} else {
			char *const new_name = search_make_new(base->global_search_state, 0, name);
			if (!new_name) return NULL;
			req = request_new(base, type, new_name, flags, user_callback, user_arg);
			mm_free(new_name);
			if (!req) return NULL;
			req->search_index = 0;
		}
		req->search_origname = mm_strdup(name);
		req->search_state = base->global_search_state;
		req->search_flags = flags;
		base->global_search_state->refcount++;
		request_submit(req);
		return req;
	} else {
		struct evdns_request *const req = request_new(base, type, name, flags, user_callback, user_arg);
		if (!req) return NULL;
		request_submit(req);
		return req;
	}
}

/* this is called when a request has failed to find a name. We need to check */
/* if it is part of a search and, if so, try the next name in the list */
/* returns: */
/*   0 another request has been submitted */
/*   1 no more requests needed */
static int
search_try_next(struct evdns_request *const req) {
	struct evdns_base *base = req->base;
	ASSERT_LOCKED(base);
	if (req->search_state) {
		/* it is part of a search */
		char *new_name;
		struct evdns_request *newreq;
		req->search_index++;
		if (req->search_index >= req->search_state->num_domains) {
			/* no more postfixes to try, however we may need to try */
			/* this name without a postfix */
			if (string_num_dots(req->search_origname) < req->search_state->ndots) {
				/* yep, we need to try it raw */
				newreq = request_new(base, req->request_type, req->search_origname, req->search_flags, req->user_callback, req->user_pointer);
				log(EVDNS_LOG_DEBUG, "Search: trying raw query %s", req->search_origname);
				if (newreq) {
					request_submit(newreq);
					return 0;
				}
			}
			return 1;
		}

		new_name = search_make_new(req->search_state, req->search_index, req->search_origname);
		if (!new_name) return 1;
		log(EVDNS_LOG_DEBUG, "Search: now trying %s (%d)", new_name, req->search_index);
		newreq = request_new(base, req->request_type, new_name, req->search_flags, req->user_callback, req->user_pointer);
		mm_free(new_name);
		if (!newreq) return 1;
		newreq->search_origname = req->search_origname;
		req->search_origname = NULL;
		newreq->search_state = req->search_state;
		newreq->search_flags = req->search_flags;
		newreq->search_index = req->search_index;
		newreq->search_state->refcount++;
		request_submit(newreq);
		return 0;
	}
	return 1;
}

static void
search_request_finished(struct evdns_request *const req) {
	ASSERT_LOCKED(req->base);
	if (req->search_state) {
		search_state_decref(req->search_state);
		req->search_state = NULL;
	}
	if (req->search_origname) {
		mm_free(req->search_origname);
		req->search_origname = NULL;
	}
}

/*/////////////////////////////////////////////////////////////////// */
/* Parsing resolv.conf files */

static void
evdns_resolv_set_defaults(struct evdns_base *base, int flags) {
	/* if the file isn't found then we assume a local resolver */
	ASSERT_LOCKED(base);
	if (flags & DNS_OPTION_SEARCH) search_set_from_hostname(base);
	if (flags & DNS_OPTION_NAMESERVERS) evdns_base_nameserver_ip_add(base,"127.0.0.1");
}

#ifndef _EVENT_HAVE_STRTOK_R
static char *
strtok_r(char *s, const char *delim, char **state) {
	char *cp, *start;
	start = cp = s ? s : *state;
	if (!cp)
		return NULL;
	while (*cp && !strchr(delim, *cp))
		++cp;
	if (!*cp) {
		if (cp == start)
			return NULL;
		*state = NULL;
		return start;
	} else {
		*cp++ = '\0';
		*state = cp;
		return start;
	}
}
#endif

/* helper version of atoi which returns -1 on error */
static int
strtoint(const char *const str)
{
	char *endptr;
	const int r = strtol(str, &endptr, 10);
	if (*endptr) return -1;
	return r;
}

/* Parse a number of seconds into a timeval; return -1 on error. */
static int
strtotimeval(const char *const str, struct timeval *out)
{
	double d;
	char *endptr;
	d = strtod(str, &endptr);
	if (*endptr) return -1;
	out->tv_sec = (int) d;
	out->tv_usec = (int) ((d - (int) d)*1000000);
	return 0;
}

/* helper version of atoi that returns -1 on error and clips to bounds. */
static int
strtoint_clipped(const char *const str, int min, int max)
{
	int r = strtoint(str);
	if (r == -1)
		return r;
	else if (r<min)
		return min;
	else if (r>max)
		return max;
	else
		return r;
}

static int
evdns_base_set_max_requests_inflight(struct evdns_base *base, int maxinflight)
{
	int old_n_heads = base->n_req_heads, n_heads;
	struct evdns_request **old_heads = base->req_heads, **new_heads, *req;
	int i;

	ASSERT_LOCKED(base);
	if (maxinflight < 1)
		maxinflight = 1;
	n_heads = (maxinflight+4) / 5;
	assert(n_heads > 0);
	new_heads = mm_malloc(n_heads * sizeof(struct evdns_request*));
	if (!new_heads)
		return (-1);
	for (i=0; i < n_heads; ++i)
		new_heads[i] = NULL;
	if (old_heads) {
		for (i = 0; i < old_n_heads; ++i) {
			while (old_heads[i]) {
				req = old_heads[i];
				evdns_request_remove(req, &old_heads[i]);
				evdns_request_insert(req, &new_heads[req->trans_id % n_heads]);
			}
		}
		mm_free(old_heads);
	}
	base->req_heads = new_heads;
	base->n_req_heads = n_heads;
	base->global_max_requests_inflight = maxinflight;
	return (0);
}

/* exported function */
int
evdns_base_set_option(struct evdns_base *base,
    const char *option, const char *val, int flags)
{
	int res;
	EVDNS_LOCK(base);
	res = evdns_base_set_option_impl(base, option, val, flags);
	EVDNS_UNLOCK(base);
	return res;
}

static int
evdns_base_set_option_impl(struct evdns_base *base,
    const char *option, const char *val, int flags)
{
	ASSERT_LOCKED(base);
	if (!strncmp(option, "ndots:", 6)) {
		const int ndots = strtoint(val);
		if (ndots == -1) return -1;
		if (!(flags & DNS_OPTION_SEARCH)) return 0;
		log(EVDNS_LOG_DEBUG, "Setting ndots to %d", ndots);
		if (!base->global_search_state) base->global_search_state = search_state_new();
		if (!base->global_search_state) return -1;
		base->global_search_state->ndots = ndots;
	} else if (!strncmp(option, "timeout:", 8)) {
		struct timeval tv;
		if (strtotimeval(val, &tv) == -1) return -1;
		if (!(flags & DNS_OPTION_MISC)) return 0;
		log(EVDNS_LOG_DEBUG, "Setting timeout to %s", val);
		memcpy(&base->global_timeout, &tv, sizeof(struct timeval));
	} else if (!strncmp(option, "max-timeouts:", 12)) {
		const int maxtimeout = strtoint_clipped(val, 1, 255);
		if (maxtimeout == -1) return -1;
		if (!(flags & DNS_OPTION_MISC)) return 0;
		log(EVDNS_LOG_DEBUG, "Setting maximum allowed timeouts to %d",
			maxtimeout);
		base->global_max_nameserver_timeout = maxtimeout;
	} else if (!strncmp(option, "max-inflight:", 13)) {
		const int maxinflight = strtoint_clipped(val, 1, 65000);
		if (maxinflight == -1) return -1;
		if (!(flags & DNS_OPTION_MISC)) return 0;
		log(EVDNS_LOG_DEBUG, "Setting maximum inflight requests to %d",
			maxinflight);
		evdns_base_set_max_requests_inflight(base, maxinflight);
	} else if (!strncmp(option, "attempts:", 9)) {
		int retries = strtoint(val);
		if (retries == -1) return -1;
		if (retries > 255) retries = 255;
		if (!(flags & DNS_OPTION_MISC)) return 0;
		log(EVDNS_LOG_DEBUG, "Setting retries to %d", retries);
		base->global_max_retransmits = retries;
	} else if (!strncmp(option, "randomize-case:", 15)) {
		int randcase = strtoint(val);
		if (!(flags & DNS_OPTION_MISC)) return 0;
		base->global_randomize_case = randcase;
	} else if (!strncmp(option, "bind-to:", 8)) {
		/* XXX This only applies to successive nameservers, not
		 * to already-configured ones.	We might want to fix that. */
		int len = sizeof(base->global_outgoing_address);
		if (!(flags & DNS_OPTION_NAMESERVERS)) return 0;
		if (evutil_parse_sockaddr_port(val,
			(struct sockaddr*)&base->global_outgoing_address, &len))
			return -1;
		base->global_outgoing_addrlen = len;
	}
	return 0;
}

int
evdns_set_option(const char *option, const char *val, int flags)
{
	if (!current_base)
		current_base = evdns_base_new(NULL, 0);
	return evdns_base_set_option(current_base, option, val, flags);
}

static void
resolv_conf_parse_line(struct evdns_base *base, char *const start, int flags) {
	char *strtok_state;
	static const char *const delims = " \t";
#define NEXT_TOKEN strtok_r(NULL, delims, &strtok_state)


	char *const first_token = strtok_r(start, delims, &strtok_state);
	ASSERT_LOCKED(base);
	if (!first_token) return;

	if (!strcmp(first_token, "nameserver") && (flags & DNS_OPTION_NAMESERVERS)) {
		const char *const nameserver = NEXT_TOKEN;

		evdns_base_nameserver_ip_add(base, nameserver);
	} else if (!strcmp(first_token, "domain") && (flags & DNS_OPTION_SEARCH)) {
		const char *const domain = NEXT_TOKEN;
		if (domain) {
			search_postfix_clear(base);
			search_postfix_add(base, domain);
		}
	} else if (!strcmp(first_token, "search") && (flags & DNS_OPTION_SEARCH)) {
		const char *domain;
		search_postfix_clear(base);

		while ((domain = NEXT_TOKEN)) {
			search_postfix_add(base, domain);
		}
		search_reverse(base);
	} else if (!strcmp(first_token, "options")) {
		const char *option;
		while ((option = NEXT_TOKEN)) {
			const char *val = strchr(option, ':');
			evdns_base_set_option(base, option, val ? val+1 : "", flags);
		}
	}
#undef NEXT_TOKEN
}

/* exported function */
/* returns: */
/*   0 no errors */
/*   1 failed to open file */
/*   2 failed to stat file */
/*   3 file too large */
/*   4 out of memory */
/*   5 short read from file */
int
evdns_base_resolv_conf_parse(struct evdns_base *base, int flags, const char *const filename) {
	int res;
	EVDNS_LOCK(base);
	res = evdns_base_resolv_conf_parse_impl(base, flags, filename);
	EVDNS_UNLOCK(base);
	return res;
}

static int
evdns_base_resolv_conf_parse_impl(struct evdns_base *base, int flags, const char *const filename) {
	struct stat st;
	int fd, n, r;
	u8 *resolv;
	char *start;
	int err = 0;

	log(EVDNS_LOG_DEBUG, "Parsing resolv.conf file %s", filename);

	fd = open(filename, O_RDONLY);
	if (fd < 0) {
		evdns_resolv_set_defaults(base, flags);
		return 1;
	}

	if (fstat(fd, &st)) { err = 2; goto out1; }
	if (!st.st_size) {
		evdns_resolv_set_defaults(base, flags);
		err = (flags & DNS_OPTION_NAMESERVERS) ? 6 : 0;
		goto out1;
	}
	if (st.st_size > 65535) { err = 3; goto out1; }	 /* no resolv.conf should be any bigger */

	resolv = (u8 *) mm_malloc((size_t)st.st_size + 1);
	if (!resolv) { err = 4; goto out1; }

	n = 0;
	while ((r = read(fd, resolv+n, (size_t)st.st_size-n)) > 0) {
		n += r;
		if (n == st.st_size)
			break;
		assert(n < st.st_size);
	}
	if (r < 0) { err = 5; goto out2; }
	resolv[n] = 0;	 /* we malloced an extra byte; this should be fine. */

	start = (char *) resolv;
	for (;;) {
		char *const newline = strchr(start, '\n');
		if (!newline) {
			resolv_conf_parse_line(base, start, flags);
			break;
		} else {
			*newline = 0;
			resolv_conf_parse_line(base, start, flags);
			start = newline + 1;
		}
	}

	if (!base->server_head && (flags & DNS_OPTION_NAMESERVERS)) {
		/* no nameservers were configured. */
		evdns_base_nameserver_ip_add(base, "127.0.0.1");
		err = 6;
	}
	if (flags & DNS_OPTION_SEARCH && (!base->global_search_state || base->global_search_state->num_domains == 0)) {
		search_set_from_hostname(base);
	}

out2:
	mm_free(resolv);
out1:
	close(fd);
	return err;
}

int
evdns_resolv_conf_parse(int flags, const char *const filename) {
	if (!current_base)
		current_base = evdns_base_new(NULL, 0);
	return evdns_base_resolv_conf_parse(current_base, flags, filename);
}

#ifdef WIN32
/* Add multiple nameservers from a space-or-comma-separated list. */
static int
evdns_nameserver_ip_add_line(struct evdns_base *base, const char *ips) {
	const char *addr;
	char *buf;
	int r;
	ASSERT_LOCKED(base);
	while (*ips) {
		while (isspace(*ips) || *ips == ',' || *ips == '\t')
			++ips;
		addr = ips;
		while (isdigit(*ips) || *ips == '.' || *ips == ':' ||
		    *ips=='[' || *ips==']')
			++ips;
		buf = mm_malloc(ips-addr+1);
		if (!buf) return 4;
		memcpy(buf, addr, ips-addr);
		buf[ips-addr] = '\0';
		r = evdns_base_nameserver_ip_add(base, buf);
		mm_free(buf);
		if (r) return r;
	}
	return 0;
}

typedef DWORD(WINAPI *GetNetworkParams_fn_t)(FIXED_INFO *, DWORD*);

/* Use the windows GetNetworkParams interface in iphlpapi.dll to */
/* figure out what our nameservers are. */
static int
load_nameservers_with_getnetworkparams(struct evdns_base *base)
{
	/* Based on MSDN examples and inspection of  c-ares code. */
	FIXED_INFO *fixed;
	HMODULE handle = 0;
	ULONG size = sizeof(FIXED_INFO);
	void *buf = NULL;
	int status = 0, r, added_any;
	IP_ADDR_STRING *ns;
	GetNetworkParams_fn_t fn;

	ASSERT_LOCKED(base);
	if (!(handle = LoadLibrary("iphlpapi.dll"))) {
		log(EVDNS_LOG_WARN, "Could not open iphlpapi.dll");
		status = -1;
		goto done;
	}
	if (!(fn = (GetNetworkParams_fn_t) GetProcAddress(handle, "GetNetworkParams"))) {
		log(EVDNS_LOG_WARN, "Could not get address of function.");
		status = -1;
		goto done;
	}

	buf = mm_malloc(size);
	if (!buf) { status = 4; goto done; }
	fixed = buf;
	r = fn(fixed, &size);
	if (r != ERROR_SUCCESS && r != ERROR_BUFFER_OVERFLOW) {
		status = -1;
		goto done;
	}
	if (r != ERROR_SUCCESS) {
		mm_free(buf);
		buf = mm_malloc(size);
		if (!buf) { status = 4; goto done; }
		fixed = buf;
		r = fn(fixed, &size);
		if (r != ERROR_SUCCESS) {
			log(EVDNS_LOG_DEBUG, "fn() failed.");
			status = -1;
			goto done;
		}
	}

	assert(fixed);
	added_any = 0;
	ns = &(fixed->DnsServerList);
	while (ns) {
		r = evdns_nameserver_ip_add_line(base, ns->IpAddress.String);
		if (r) {
			log(EVDNS_LOG_DEBUG,"Could not add nameserver %s to list,error: %d",
				(ns->IpAddress.String),(int)GetLastError());
			status = r;
		} else {
			++added_any;
			log(EVDNS_LOG_DEBUG,"Succesfully added %s as nameserver",ns->IpAddress.String);
		}

		ns = ns->Next;
	}

	if (!added_any) {
		log(EVDNS_LOG_DEBUG, "No nameservers added.");
		if (status == 0)
			status = -1;
	} else {
		status = 0;
	}

 done:
	if (buf)
		mm_free(buf);
	if (handle)
		FreeLibrary(handle);
	return status;
}

static int
config_nameserver_from_reg_key(struct evdns_base *base, HKEY key, const char *subkey)
{
	char *buf;
	DWORD bufsz = 0, type = 0;
	int status = 0;

	ASSERT_LOCKED(base);
	if (RegQueryValueEx(key, subkey, 0, &type, NULL, &bufsz)
	    != ERROR_MORE_DATA)
		return -1;
	if (!(buf = mm_malloc(bufsz)))
		return -1;

	if (RegQueryValueEx(key, subkey, 0, &type, (LPBYTE)buf, &bufsz)
	    == ERROR_SUCCESS && bufsz > 1) {
		status = evdns_nameserver_ip_add_line(base,buf);
	}

	mm_free(buf);
	return status;
}

#define SERVICES_KEY "System\\CurrentControlSet\\Services\\"
#define WIN_NS_9X_KEY  SERVICES_KEY "VxD\\MSTCP"
#define WIN_NS_NT_KEY  SERVICES_KEY "Tcpip\\Parameters"

static int
load_nameservers_from_registry(struct evdns_base *base)
{
	int found = 0;
	int r;
#define TRY(k, name) \
	if (!found && config_nameserver_from_reg_key(base,k,name) == 0) { \
		log(EVDNS_LOG_DEBUG,"Found nameservers in %s/%s",#k,name); \
		found = 1;						\
	} else if (!found) {						\
		log(EVDNS_LOG_DEBUG,"Didn't find nameservers in %s/%s", \
		    #k,#name);						\
	}

	ASSERT_LOCKED(base);

	if (((int)GetVersion()) > 0) { /* NT */
		HKEY nt_key = 0, interfaces_key = 0;

		if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, WIN_NS_NT_KEY, 0,
				 KEY_READ, &nt_key) != ERROR_SUCCESS) {
			log(EVDNS_LOG_DEBUG,"Couldn't open nt key, %d",(int)GetLastError());
			return -1;
		}
		r = RegOpenKeyEx(nt_key, "Interfaces", 0,
			     KEY_QUERY_VALUE|KEY_ENUMERATE_SUB_KEYS,
			     &interfaces_key);
		if (r != ERROR_SUCCESS) {
			log(EVDNS_LOG_DEBUG,"Couldn't open interfaces key, %d",(int)GetLastError());
			return -1;
		}
		TRY(nt_key, "NameServer");
		TRY(nt_key, "DhcpNameServer");
		TRY(interfaces_key, "NameServer");
		TRY(interfaces_key, "DhcpNameServer");
		RegCloseKey(interfaces_key);
		RegCloseKey(nt_key);
	} else {
		HKEY win_key = 0;
		if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, WIN_NS_9X_KEY, 0,
				 KEY_READ, &win_key) != ERROR_SUCCESS) {
			log(EVDNS_LOG_DEBUG, "Couldn't open registry key, %d", (int)GetLastError());
			return -1;
		}
		TRY(win_key, "NameServer");
		RegCloseKey(win_key);
	}

	if (found == 0) {
		log(EVDNS_LOG_WARN,"Didn't find any nameservers.");
	}

	return found ? 0 : -1;
#undef TRY
}

int
evdns_base_config_windows_nameservers(struct evdns_base *base)
{
	int r;
	if (base == NULL)
		base = current_base;
	if (base == NULL)
		return -1;
	EVDNS_LOCK(base);
	if (load_nameservers_with_getnetworkparams(base) == 0)
		return 0;
	r = load_nameservers_from_registry(base);
	EVDNS_UNLOCK(base);
	return r;
}

int
evdns_config_windows_nameservers(void)
{
	if (!current_base) {
		current_base = evdns_base_new(NULL, 1);
		return current_base == NULL ? -1 : 0;
	} else {
		return evdns_base_config_windows_nameservers(current_base);
	}
}
#endif

struct evdns_base *
evdns_base_new(struct event_base *event_base, int initialize_nameservers)
{
	struct evdns_base *base;
	base = mm_malloc(sizeof(struct evdns_base));
	if (base == NULL)
		return (NULL);
	memset(base, 0, sizeof(struct evdns_base));
	base->req_waiting_head = NULL;

	EVTHREAD_ALLOC_LOCK(base->lock);
	EVDNS_LOCK(base);

	/* Set max requests inflight and allocate req_heads. */
	base->req_heads = NULL;

	evdns_base_set_max_requests_inflight(base, 64);

	base->server_head = NULL;
	base->event_base = event_base;
	base->global_good_nameservers = base->global_requests_inflight =
		base->global_requests_waiting = 0;

	base->global_timeout.tv_sec = 5;
	base->global_timeout.tv_usec = 0;
	base->global_max_reissues = 1;
	base->global_max_retransmits = 3;
	base->global_max_nameserver_timeout = 3;
	base->global_search_state = NULL;
	base->global_randomize_case = 1;

	if (initialize_nameservers) {
		int r;
#ifdef WIN32
		r = evdns_base_config_windows_nameservers(base);
#else
		r = evdns_base_resolv_conf_parse(base, DNS_OPTIONS_ALL, "/etc/resolv.conf");
#endif
		if (r == -1) {
			evdns_base_free_and_unlock(base, 0);
			return NULL;
		}
	}
	EVDNS_UNLOCK(base);
	return base;
}

int
evdns_init(void)
{
	struct evdns_base *base = evdns_base_new(NULL, 1);
	if (base) {
		current_base = base;
		return 0;
	} else {
		return -1;
	}
}

const char *
evdns_err_to_string(int err)
{
    switch (err) {
	case DNS_ERR_NONE: return "no error";
	case DNS_ERR_FORMAT: return "misformatted query";
	case DNS_ERR_SERVERFAILED: return "server failed";
	case DNS_ERR_NOTEXIST: return "name does not exist";
	case DNS_ERR_NOTIMPL: return "query not implemented";
	case DNS_ERR_REFUSED: return "refused";

	case DNS_ERR_TRUNCATED: return "reply truncated or ill-formed";
	case DNS_ERR_UNKNOWN: return "unknown";
	case DNS_ERR_TIMEOUT: return "request timed out";
	case DNS_ERR_SHUTDOWN: return "dns subsystem shut down";
	case DNS_ERR_CANCEL: return "dns request canceled";
	default: return "[Unknown error code]";
    }
}

static void
evdns_base_free_and_unlock(struct evdns_base *base, int fail_requests)
{
	struct nameserver *server, *server_next;
	struct search_domain *dom, *dom_next;
	int i;

	/* Requires that we hold the lock. */

	/* TODO(nickm) we might need to refcount here. */

	for (i = 0; i < base->n_req_heads; ++i) {
		while (base->req_heads[i]) {
			if (fail_requests)
				reply_schedule_callback(base->req_heads[i], 0, DNS_ERR_SHUTDOWN, NULL);
			request_finished(base->req_heads[i], &REQ_HEAD(base, base->req_heads[i]->trans_id));
		}
	}
	while (base->req_waiting_head) {
		if (fail_requests)
			reply_schedule_callback(base->req_waiting_head, 0, DNS_ERR_SHUTDOWN, NULL);
		request_finished(base->req_waiting_head, &base->req_waiting_head);
	}
	base->global_requests_inflight = base->global_requests_waiting = 0;

	for (server = base->server_head; server; server = server_next) {
		server_next = server->next;
		if (server->socket >= 0)
			CLOSE_SOCKET(server->socket);
		(void) event_del(&server->event);
		if (server->state == 0)
			(void) event_del(&server->timeout_event);
		mm_free(server);
		if (server_next == base->server_head)
			break;
	}
	base->server_head = NULL;
	base->global_good_nameservers = 0;

	if (base->global_search_state) {
		for (dom = base->global_search_state->head; dom; dom = dom_next) {
			dom_next = dom->next;
			mm_free(dom);
		}
		mm_free(base->global_search_state);
		base->global_search_state = NULL;
	}
	EVDNS_UNLOCK(base);
	EVTHREAD_FREE_LOCK(base->lock);

	mm_free(base);
}

void
evdns_base_free(struct evdns_base *base, int fail_requests)
{
	EVDNS_LOCK(base);
	evdns_base_free_and_unlock(base, fail_requests);
}

void
evdns_shutdown(int fail_requests)
{
	if (current_base) {
		struct evdns_base *b = current_base;
		current_base = NULL;
		evdns_base_free(b, fail_requests);
	}
	evdns_log_fn = NULL;
}

#ifdef EVDNS_MAIN
void
main_callback(int result, char type, int count, int ttl,
			  void *addrs, void *orig) {
	char *n = (char*)orig;
	int i;
	for (i = 0; i < count; ++i) {
		if (type == DNS_IPv4_A) {
			printf("%s: %s\n", n, debug_ntoa(((u32*)addrs)[i]));
		} else if (type == DNS_PTR) {
			printf("%s: %s\n", n, ((char**)addrs)[i]);
		}
	}
	if (!count) {
		printf("%s: No answer (%d)\n", n, result);
	}
	fflush(stdout);
}
void
evdns_server_callback(struct evdns_server_request *req, void *data)
{
	int i, r;
	(void)data;
	/* dummy; give 192.168.11.11 as an answer for all A questions,
	 *	give foo.bar.example.com as an answer for all PTR questions. */
	for (i = 0; i < req->nquestions; ++i) {
		u32 ans = htonl(0xc0a80b0bUL);
		if (req->questions[i]->type == EVDNS_TYPE_A &&
			req->questions[i]->dns_question_class == EVDNS_CLASS_INET) {
			printf(" -- replying for %s (A)\n", req->questions[i]->name);
			r = evdns_server_request_add_a_reply(req, req->questions[i]->name,
										  1, &ans, 10);
			if (r<0)
				printf("eeep, didn't work.\n");
		} else if (req->questions[i]->type == EVDNS_TYPE_PTR &&
				   req->questions[i]->dns_question_class == EVDNS_CLASS_INET) {
			printf(" -- replying for %s (PTR)\n", req->questions[i]->name);
			r = evdns_server_request_add_ptr_reply(req, NULL, req->questions[i]->name,
											"foo.bar.example.com", 10);
		} else {
			printf(" -- skipping %s [%d %d]\n", req->questions[i]->name,
				   req->questions[i]->type, req->questions[i]->dns_question_class);
		}
	}

	r = evdns_server_request_respond(req, 0);
	if (r<0)
		printf("eeek, couldn't send reply.\n");
}

void
logfn(int is_warn, const char *msg) {
  (void) is_warn;
  fprintf(stderr, "%s\n", msg);
}
int
main(int c, char **v) {
	int idx;
	int reverse = 0, verbose = 1, servertest = 0;
	if (c<2) {
		fprintf(stderr, "syntax: %s [-x] [-v] hostname\n", v[0]);
		fprintf(stderr, "syntax: %s [-servertest]\n", v[0]);
		return 1;
	}
	idx = 1;
	while (idx < c && v[idx][0] == '-') {
		if (!strcmp(v[idx], "-x"))
			reverse = 1;
		else if (!strcmp(v[idx], "-v"))
			verbose = 1;
		else if (!strcmp(v[idx], "-servertest"))
			servertest = 1;
		else
			fprintf(stderr, "Unknown option %s\n", v[idx]);
		++idx;
	}
	event_init();
	evdns_init();
	if (verbose)
		evdns_set_log_fn(logfn);
	evdns_resolv_conf_parse(DNS_OPTION_NAMESERVERS, "/etc/resolv.conf");
	if (servertest) {
		int sock;
		struct sockaddr_in my_addr;
		sock = socket(PF_INET, SOCK_DGRAM, 0);
		evutil_make_socket_nonblocking(sock);
		my_addr.sin_family = AF_INET;
		my_addr.sin_port = htons(10053);
		my_addr.sin_addr.s_addr = INADDR_ANY;
		if (bind(sock, (struct sockaddr*)&my_addr, sizeof(my_addr))<0) {
			perror("bind");
			exit(1);
		}
		evdns_add_server_port(sock, 0, evdns_server_callback, NULL);
	}
	for (; idx < c; ++idx) {
		if (reverse) {
			struct in_addr addr;
			if (!inet_aton(v[idx], &addr)) {
				fprintf(stderr, "Skipping non-IP %s\n", v[idx]);
				continue;
			}
			fprintf(stderr, "resolving %s...\n",v[idx]);
			evdns_resolve_reverse(&addr, 0, main_callback, v[idx]);
		} else {
			fprintf(stderr, "resolving (fwd) %s...\n",v[idx]);
			evdns_resolve_ipv4(v[idx], 0, main_callback, v[idx]);
		}
	}
	fflush(stdout);
	event_dispatch();
	return 0;
}
#endif
