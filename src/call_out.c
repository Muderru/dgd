# define INCLUDE_FILE_IO
# include "dgd.h"
# include "str.h"
# include "array.h"
# include "object.h"
# include "xfloat.h"
# include "interpret.h"
# include "data.h"
# include "call_out.h"

# define CYCBUF_SIZE	128		/* cyclic buffer size, power of 2 */
# define CYCBUF_MASK	(CYCBUF_SIZE - 1) /* cyclic buffer mask */
# define SWPERIOD	60		/* swaprate buffer size */

typedef struct {
    uindex handle;	/* callout handle */
    uindex oindex;	/* index in object table */
    Uint time;		/* when to call */
    uindex mtime;	/* when to call in milliseconds */
} call_out;

# define prev		oindex
# define next		time
# define count		mtime

struct _cbuf_ {
    uindex list;	/* list */
    uindex last;	/* last in list */
};

static char cb_layout[] = "uu";

static call_out *cotab;			/* callout table */
static uindex cotabsz;			/* callout table size */
static uindex queuebrk;			/* queue brk */
static uindex cycbrk;			/* cyclic buffer brk */
static uindex flist;			/* free list index */
static uindex nzero;			/* # immediate callouts */
static uindex nshort;			/* # short-term callouts, incl. nzero */
static cbuf running;			/* running callouts */
static cbuf immediate;			/* immediate callouts */
static cbuf cycbuf[CYCBUF_SIZE];	/* cyclic buffer of callout lists */
static Uint timestamp;			/* cycbuf start time */
static Uint timeout;			/* time of first callout in cycbuf */
static Uint atimeout;			/* alarm time in seconds */
static unsigned short amtime;		/* alarm time in milliseconds */
static Uint timediff;			/* stored/actual time difference */
static Uint swaptime;			/* last swap count timestamp */
static Uint swapped1[SWPERIOD];		/* swap info for last minute */
static Uint swapped5[SWPERIOD];		/* swap info for last five minutes */
static Uint swaprate1;			/* swaprate per minute */
static Uint swaprate5;			/* swaprate per 5 minutes */

/*
 * NAME:	call_out->init()
 * DESCRIPTION:	initialize callout handling
 */
void co_init(max)
unsigned int max;
{
    if (max != 0) {
	cotab = ALLOC(call_out, max + 1);
	cotab[0].time = 0;	/* sentinel for the heap */
	cotab++;
	flist = 0;
	/* only if callouts are enabled */
	if (P_time() >> 24 <= 1) {
	    fatal("bad time (early seventies)");
	}
	timestamp = timeout = 0;
	atimeout = amtime = 0;
	timediff = 0;
    }
    running.list = immediate.list = 0;
    memset(cycbuf, '\0', sizeof(cycbuf));
    cycbrk = cotabsz = max;
    queuebrk = 0;
    nzero = nshort = 0;

    swaptime = P_time();
    memset(swapped1, '\0', sizeof(swapped1));
    memset(swapped5, '\0', sizeof(swapped5));
    swaprate1 = swaprate5 = 0;
}

/*
 * NAME:	restart()
 * DESCRIPTION:	possibly restart timeout
 */
static void restart(t)
register Uint t;
{
    register unsigned short m;

    if (t != 0) {
	if (nshort != nzero) {
	    /* look for next callout */
	    while (cycbuf[t & CYCBUF_MASK].list == 0) {
		t++;
	    }
	    timeout = t;
	} else {
	    /* no callouts left */
	    timeout = 0;
	}
    }

    t = timeout;
    m = 0;
    if (queuebrk != 0 &&
	(t == 0 || cotab[0].time < t ||
	 (cotab[0].time == t && cotab[0].mtime < m))) {
	t = cotab[0].time;
	m = cotab[0].mtime;
    }

    if (t != atimeout || m != amtime) {
	P_timer(atimeout = t, amtime = m);
    }
}

/*
 * NAME:	enqueue()
 * DESCRIPTION:	put a callout in the queue
 */
static call_out *enqueue(t, m)
register Uint t;
unsigned short m;
{
    register uindex i, j;
    register call_out *l;

    /*
     * create a free spot in the heap, and sift it upward
     */
    i = ++queuebrk;
    l = cotab - 1;
    for (j = i >> 1; l[j].time > t || (l[j].time == t && l[j].mtime > m);
	 i = j, j >>= 1) {
	l[i] = l[j];
    }

    l = &l[i];
    l->time = t;
    l->mtime = m;
    if (atimeout == 0 || t < atimeout || (t == atimeout && m < amtime)) {
	restart((Uint) 0);
    }
    return l;
}

/*
 * NAME:	dequeue()
 * DESCRIPTION:	remove a callout from the queue
 */
static void dequeue(i)
register uindex i;
{
    register Uint t;
    register short m;
    register uindex j;
    register call_out *l;

    l = cotab - 1;
    i++;
    t = l[queuebrk].time;
    m = l[queuebrk].mtime;
    if (t < l[i].time) {
	/* sift upward */
	for (j = i >> 1; l[j].time > t || (l[j].time == t && l[j].mtime > m);
	     i = j, j >>= 1) {
	    l[i] = l[j];
	}
    } else {
	/* sift downward */
	for (j = i << 1; j < queuebrk; i = j, j <<= 1) {
	    if (l[j].time > l[j + 1].time ||
		(l[j].time == l[j + 1].time && l[j].mtime > l[j + 1].mtime)) {
		j++;
	    }
	    if (t < l[j].time || (t == l[j].time && m <= l[j].mtime)) {
		break;
	    }
	    l[i] = l[j];
	}
    }
    /* put into place */
    l[i] = l[queuebrk--];
}

/*
 * NAME:	newcallout()
 * DESCRIPTION:	allocate a new callout for the cyclic buffer
 */
static call_out *newcallout(list, t)
register cbuf *list;
Uint t;
{
    register uindex i;
    register call_out *co;

    if (flist != 0) {
	/* get callout from free list */
	i = flist;
	flist = cotab[i].next;
    } else {
	/* allocate new callout */
	i = --cycbrk;
    }
    nshort++;
    if (t == 0) {
	nzero++;
    }

    co = &cotab[i];
    if (list->list == 0) {
	/* first one in list */
	list->list = i;
	co->count = 1;

	if (t != 0 && (timeout == 0 || t < timeout)) {
	    restart(t);
	}
    } else {
	/* add to list */
	cotab[list->list].count++;
	cotab[list->last].next = i;
    }
    list->last = i;
    co->next = 0;

    return co;
}

/*
 * NAME:	freecallout()
 * DESCRIPTION:	remove a callout from the cyclic buffer
 */
static void freecallout(cyc, j, i, t)
register cbuf *cyc;
register uindex j, i;
register Uint t;
{
    register call_out *l;

    --nshort;
    if (t == 0) {
	--nzero;
    }

    l = cotab;
    if (i == j) {
	cyc->list = l[i].next;
	if (cyc->list != 0) {
	    l[cyc->list].count = l[i].count - 1;
	} else if (t != 0 && t == timeout) {
	    restart(t);
	}
    } else {
	if (i == cyc->last) {
	    /* last element of the list */
	    l[cyc->last = j].next = 0;
	} else {
	    /* connect previous to next */
	    l[j].next = l[i].next;
	}
	--l[cyc->list].count;
    }
    l += i;
    l->handle = 0;	/* mark as unused */
    if (i == cycbrk) {
	/*
	 * callout at the edge
	 */
	while (++cycbrk != cotabsz && (++l)->handle == 0) {
	    /* followed by free callout */
	    if (cycbrk == flist) {
		/* first in the free list */
		flist = l->next;
	    } else {
		/* connect previous to next */
		cotab[l->prev].next = l->next;
		if (l->next != 0) {
		    /* connect next to previous */
		    cotab[l->next].prev = l->prev;
		}
	    }
	}
    } else {
	/* add to free list */
	if (flist != 0) {
	    /* link next to current */
	    cotab[flist].prev = i;
	}
	/* link to next */
	l->next = flist;
	flist = i;
    }
}

/*
 * NAME:	encode()
 * DESCRIPTION:	encode millisecond time
 */
static Uint encode(time, mtime)
Uint time;
unsigned int mtime;
{
    return 0x01000000L + (((time - timediff) & 0xff) << 16) + mtime;
}

/*
 * NAME:	decode()
 * DESCRIPTION:	decode millisecond time
 */
static Uint decode(time, mtime)
register Uint time;
unsigned short *mtime;
{
    *mtime = time & 0xffff;
    time = ((timestamp - timediff) & 0xffffff00L) + ((time >> 16) & 0xff) +
	   timediff;
    if (time < timestamp) {
	time += 0x100;
    }
    return time;
}

/*
 * NAME:	call_out->time()
 * DESCRIPTION:	get the current (adjusted) time
 */
static Uint co_time(mtime)
unsigned short *mtime;
{
    Uint t;

    t = P_mtime(mtime);
    if (t < timestamp) {
	/* clock turned back? */
	t = timestamp;
	*mtime = 0;
    } else if (timestamp < t) {
	if (atimeout == 0 || atimeout > t) {
	    timestamp = t;
	} else {
	    if (timestamp < atimeout - 1) {
		timestamp = atimeout - 1;
	    }
	    if (t > timestamp + 60) {
		/* lot of lag? */
		t = timestamp + 60;
		*mtime = 0;
	    }
	}
    }

    return t;
}

/*
 * NAME:	call_out->check()
 * DESCRIPTION:	check if, and how, a new callout can be added
 */
Uint co_check(n, delay, mdelay, tp, mp, qp)
unsigned int n, mdelay;
Int delay;
Uint *tp;
unsigned short *mp;
cbuf **qp;
{
    register Uint t;
    register unsigned short m;
    register call_out *co;

    if (cotabsz == 0) {
	/*
	 * call_outs are disabled
	 */
	*qp = (cbuf *) NULL;
	return 0;
    }

    if (queuebrk + nshort + n == cotabsz || nshort + n == cotabsz - 1) {
	error("Too many callouts");
    }

    if (delay == 0 && (mdelay == 0 || mdelay == 0xffff)) {
	/*
	 * immediate callout
	 */
	*qp = &immediate;
	*tp = t = 0;
	*mp = 0;
    } else {
	/*
	 * delayed callout
	 */
	t = co_time(mp);
	if (t + delay + 1 <= t) {
	    error("Too long delay");
	}
	t += delay;
	if (mdelay != 0xffff) {
	    m = *mp + mdelay;
	    if (m >= 1000) {
		m -= 1000;
		t++;
	    }
	} else {
	    m = 0;
	}

	if (mdelay == 0xffff && t < timestamp + CYCBUF_SIZE) {
	    /* use cyclic buffer */
	    *qp = &cycbuf[t & CYCBUF_MASK];
	} else {
	    /* use queue */
	    *qp = (cbuf *) NULL;
	}
	*tp = t;
	*mp = m;

	if (mdelay == 0xffff) {
	    t -= timediff;
	} else {
	    t = encode(t, m);
	}
    }

    return t;
}

/*
 * NAME:	call_out->new()
 * DESCRIPTION:	add a callout
 */
void co_new(handle, obj, t, m, q)
unsigned int handle, m;
object *obj;
Uint t;
cbuf *q;
{
    register call_out *co;

    co = (q != (cbuf *) NULL) ? newcallout(q, t) : enqueue(t, m);
    co->handle = handle;
    co->oindex = obj->index;
}

/*
 * NAME:	rmshort()
 * DESCRIPTION:	remove a short-term callout
 */
static bool rmshort(cyc, i, handle, t)
register cbuf *cyc;
register uindex i, handle;
Uint t;
{
    register uindex j, k;
    register call_out *l;

    k = cyc->list;
    if (k != 0) {
	/*
	 * this time-slot is in use
	 */
	l = cotab;
	if (l[k].oindex == i && l[k].handle == handle) {
	    /* first element in list */
	    freecallout(cyc, k, k, t);
	    return TRUE;
	}
	if (k != cyc->last) {
	    /*
	     * list contains more than 1 element
	     */
	    j = k;
	    k = l[j].next;
	    do {
		if (l[k].oindex == i && l[k].handle == handle) {
		    /* found it */
		    freecallout(cyc, j, k, t);
		    return TRUE;
		}
		j = k;
	    } while ((k=l[j].next) != 0);
	}
    }
    return FALSE;
}

/*
 * NAME:	call_out->remaining()
 * DESCRIPTION:	return the time remaining before a callout expires
 */
Int co_remaining(t)
register Uint t;
{
    unsigned short m, mtime;

    if (t >> 24 != 1) {
	t += timediff;
	return (t > timestamp) ? t - timestamp : 0;
    } else {
	/* encoded millisecond */
	t = decode((Uint) t, &m) - co_time(&mtime);
	return -2 - t * 1000 - m + mtime;
    }
}

/*
 * NAME:	call_out->del()
 * DESCRIPTION:	remove a callout
 */
void co_del(obj, handle, t)
object *obj;
register unsigned int handle;
Uint t;
{
    register uindex i;
    register call_out *l;

    i = obj->index;
    if (t >> 24 != 1) {
	t += timediff;
	if (t <= timestamp) {
	    /*
	     * possible immediate callout
	     */
	    if (rmshort(&immediate, i, handle, 0) ||
		rmshort(&running, i, handle, 0)) {
		return;
	    }
	}

	if (t < timestamp + CYCBUF_SIZE) {
	    /*
	     * try to find the callout in the cyclic buffer
	     */
	    if (rmshort(&cycbuf[t & CYCBUF_MASK], i, handle, t)) {
		return;
	    }
	}
    }

    /*
     * Not found in the cyclic buffer; it <must> be in the queue.
     */
    l = cotab;
    for (;;) {
	if (l->oindex == i && l->handle == handle) {
	    dequeue(l - cotab);
	    return;
	}
	l++;
# ifdef DEBUG
	if (l == cotab + queuebrk) {
	    fatal("failed to remove callout");
	}
# endif
    }
}

/*
 * NAME:	call_out->list()
 * DESCRIPTION:	adjust callout delays in array
 */
void co_list(a)
array *a;
{
    register value *v, *w;
    register unsigned short i;
    unsigned short mtime, m;
    Uint t;
    xfloat flt;

    for (i = a->size, v = a->elts; i != 0; --i, v++) {
	w = &v->u.array->elts[2];
	switch ((Uint) w->u.number >> 24) {
	case 0:
	    /* immediate */
	    break;

	case 1:
	    /* encoded millisecond */
	    t = decode((Uint) w->u.number, &m) - co_time(&mtime);
	    flt_itof((Int) t * 1000 + m - mtime, &flt);
	    flt_mult(&flt, &thousandth);
	    w->type = T_FLOAT;
	    VFLT_PUT(w, flt);
	    break;

	default:
	    /* normal */
	    w->u.number -= timestamp - timediff;
	    break;
	}
    }
}

/*
 * NAME:	call_out->expire()
 * DESCRIPTION:	collect callouts to run next
 */
static void co_expire()
{
    register call_out *co;
    register uindex handle, oindex, i;
    register cbuf *cyc;
    Uint t;
    unsigned short m;

    if (P_timeout(&t, &m)) {
	while (timestamp < t) {
	    timestamp++;

	    /*
	     * from queue
	     */
	    while (queuebrk != 0 && cotab[0].time < timestamp) {
		handle = cotab[0].handle;
		oindex = cotab[0].oindex;
		dequeue(0);
		co = newcallout(&immediate, 0);
		co->handle = handle;
		co->oindex = oindex;
	    }

	    /*
	     * from cyclic buffer list
	     */
	    cyc = &cycbuf[timestamp & CYCBUF_MASK];
	    i = cyc->list;
	    if (i != 0) {
		cyc->list = 0;
		if (immediate.list == 0) {
		    immediate.list = i;
		} else {
		    cotab[immediate.last].next = i;
		}
		immediate.last = cyc->last;
		i = cotab[i].count;
		cotab[immediate.list].count += i;
		nzero += i;
	    }
	}

	/*
	 * from queue
	 */
	while (queuebrk != 0 &&
	       (cotab[0].time < t ||
		(cotab[0].time == t && cotab[0].mtime <= m))) {
	    handle = cotab[0].handle;
	    oindex = cotab[0].oindex;
	    dequeue(0);
	    co = newcallout(&immediate, 0);
	    co->handle = handle;
	    co->oindex = oindex;
	}

	restart(t);
    }

    /* handle swaprate */
    while (swaptime < t) {
	++swaptime;
	swaprate1 -= swapped1[swaptime % SWPERIOD];
	swapped1[swaptime % SWPERIOD] = 0;
	if (swaptime % 5 == 0) {
	    swaprate5 -= swapped5[swaptime % (5 * SWPERIOD) / 5];
	    swapped5[swaptime % (5 * SWPERIOD) / 5] = 0;
	}
    }
}

/*
 * NAME:	call_out->call()
 * DESCRIPTION:	call expired callouts
 */
void co_call(f)
frame *f;
{
    register uindex i, handle;
    object *obj;
    string *str;
    int nargs;

    co_expire();
    running = immediate;
    immediate.list = 0;

    if (running.list != 0) {
	/*
	 * callouts to do
	 */
	while (ec_push((ec_ftn) errhandler)) {
	    endthread();
	}
	while ((i=running.list) != 0) {
	    handle = cotab[i].handle;
	    obj = &otable[cotab[i].oindex];
	    freecallout(&running, i, i, 0);

	    str = d_get_call_out(o_dataspace(obj), handle, f, &nargs);
	    if (i_call(f, obj, str->text, str->len, TRUE, nargs)) {
		/* function exists */
		i_del_value(f->sp++);
		str_del((f->sp++)->u.string);
	    } else {
		/* function doesn't exist */
		str_del((f->sp++)->u.string);
	    }
	    endthread();
	}
	ec_pop();
    }
}

/*
 * NAME:	call_out->info()
 * DESCRIPTION:	give information about callouts
 */
void co_info(n1, n2)
uindex *n1, *n2;
{
    *n1 = nshort;
    *n2 = queuebrk;
}

/*
 * NAME:	call_out->delay()
 * DESCRIPTION:	return the time until the next timeout
 */
Uint co_delay(mtime)
unsigned short *mtime;
{
    Uint t;
    unsigned short m;

    if (nzero != 0) {
	/* immediate */
	*mtime = 0;
	return 0;
    }
    if (atimeout == 0) {
	/* infinite */
	*mtime = 0xffff;
	return 0;
    }

    t = co_time(&m);
    if (t > atimeout || (t == atimeout && m >= amtime)) {
	/* immediate */
	*mtime = 0;
	return 0;
    }
    if (m > amtime) {
	m -= 1000;
	t++;
    }
    *mtime = amtime - m;
    return atimeout - t;
}

/*
 * NAME:	call_out->swapcount()
 * DESCRIPTION:	keep track of the number of objects swapped out
 */
void co_swapcount(count)
unsigned int count;
{
    swaprate1 += count;
    swaprate5 += count;
    swapped1[swaptime % SWPERIOD] += count;
    swapped5[swaptime % (SWPERIOD * 5) / 5] += count;
}

/*
 * NAME:	call_out->swaprate1()
 * DESCRIPTION:	return the number of objects swapped out per minute
 */
long co_swaprate1()
{
    return swaprate1;
}

/*
 * NAME:	call_out->swaprate5()
 * DESCRIPTION:	return the number of objects swapped out per 5 minutes
 */
long co_swaprate5()
{
    return swaprate5;
}


typedef struct {
    uindex cotabsz;		/* callout table size */
    uindex queuebrk;		/* queue brk */
    uindex cycbrk;		/* cyclic buffer brk */
    uindex flist;		/* free list index */
    uindex nshort;		/* # of short-term callouts */
    uindex nlong0;		/* # of long-term callouts and imm. callouts */
    Uint timestamp;		/* time the last alarm came */
    Uint timediff;		/* accumulated time difference */
} dump_header;

static char dh_layout[] = "uuuuuuii";

typedef struct {
    uindex handle;	/* callout handle */
    uindex oindex;	/* index in object table */
    Uint time;		/* when to call */
} dump_callout;

static char dco_layout[] = "uui";

/*
 * NAME:	call_out->dump()
 * DESCRIPTION:	dump callout table
 */
bool co_dump(fd)
int fd;
{
    dump_header dh;
    register uindex list, last;
    register dump_callout *dc;
    register call_out *co;
    register uindex n;
    register cbuf *cb;
    unsigned short m;
    bool ret;

    /* update timestamp */
    co_time(&m);

    /* fill in header */
    dh.cotabsz = cotabsz;
    dh.queuebrk = queuebrk;
    dh.cycbrk = cycbrk;
    dh.flist = flist;
    dh.nshort = nshort;
    dh.nlong0 = queuebrk + nzero;
    dh.timestamp = timestamp;
    dh.timediff = timediff;

    /* copy callouts */
    n = queuebrk + cotabsz - cycbrk;
    if (n != 0) {
	dc = ALLOCA(dump_callout, n);
	for (co = cotab, n = queuebrk; n != 0; co++, --n) {
	    dc->handle = co->handle;
	    dc->oindex = co->oindex;
	    dc->time = (co->mtime != 0) ?
			encode(co->time, co->mtime) : co->time;
	    dc++;
	}
	for (co = cotab + cycbrk, n = cotabsz - cycbrk; n != 0; co++, --n) {
	    dc->handle = co->handle;
	    dc->oindex = co->oindex;
	    dc->time = co->time;
	    dc++;
	}
	n = queuebrk + cotabsz - cycbrk;
	dc -= n;
    }

    /* deal with immediate callouts */
    if (nzero != 0) {
	if (running.list != 0) {
	    list = running.list;
	    if (immediate.list != 0) {
		dc[running.last + n - cotabsz].next = immediate.list;
		last = immediate.last;
	    } else {
		last = running.last;
	    }
	} else {
	    list = immediate.list;
	    last = immediate.last;
	}
	cb = &cycbuf[timestamp & CYCBUF_MASK];
	last = dc[last + n - cotabsz].next = cb->list;
	cb->list = list;
    }

    /* write header and callouts */
    ret = (P_write(fd, (char *) &dh, sizeof(dump_header)) > 0 &&
	   (n == 0 || P_write(fd, (char *) dc, n * sizeof(dump_callout)) > 0) &&
	   P_write(fd, (char *) cycbuf, CYCBUF_SIZE * sizeof(cbuf)) > 0);

    if (n != 0) {
	AFREE(dc);
    }

    if (nzero != 0) {
	cb->list = last;
    }

    return ret;
}

/*
 * NAME:	call_out->restore()
 * DESCRIPTION:	restore callout table
 */
void co_restore(fd, t)
int fd;
register Uint t;
{
    register uindex n, i, offset, last;
    register dump_callout *dc;
    register call_out *co;
    register cbuf *cb;
    dump_header dh;
    cbuf buffer[CYCBUF_SIZE];
    unsigned short m;

    /* read and check header */
    conf_dread(fd, (char *) &dh, dh_layout, (Uint) 1);
    queuebrk = dh.queuebrk;
    offset = cotabsz - dh.cotabsz;
    cycbrk = dh.cycbrk + offset;
    if (queuebrk > cycbrk + offset || cycbrk == 0) {
	error("Restored too many callouts");
    }

    /* read tables */
    n = queuebrk + cotabsz - cycbrk;
    if (n != 0) {
	dc = ALLOCA(dump_callout, n);
	conf_dread(fd, (char *) dc, dco_layout, (Uint) n);
    }
    conf_dread(fd, (char *) buffer, cb_layout, (Uint) CYCBUF_SIZE);

    flist = dh.flist;
    nshort = dh.nshort;
    nzero = dh.nlong0 - dh.queuebrk;
    timestamp = t;
    t -= dh.timestamp;
    timediff = dh.timediff + t;

    /* copy callouts */
    if (n != 0) {
	for (co = cotab, i = queuebrk; i != 0; co++, --i) {
	    co->handle = dc->handle;
	    co->oindex = dc->oindex;
	    if (dc->time >> 24 == 1) {
		co->time = decode(dc->time, &m);
		co->mtime = m;
	    } else {
		co->time = dc->time + t;
	    }
	    dc++;
	}
	for (co = cotab + cycbrk, i = cotabsz - cycbrk; i != 0; co++, --i) {
	    co->handle = dc->handle;
	    co->oindex = dc->oindex;
	    co->time = dc->time;
	    dc++;
	}
	dc -= n;
	AFREE(dc);
    }

    /* cycle around cyclic buffer */
    t &= CYCBUF_MASK;
    memcpy(cycbuf + t, buffer, (unsigned int) (CYCBUF_SIZE - t) * sizeof(cbuf));
    memcpy(cycbuf, buffer + CYCBUF_SIZE - t, (unsigned int) t * sizeof(cbuf));

    if (offset != 0) {
	/* patch callout references */
	if (flist != 0) {
	    flist += offset;
	}
	for (i = CYCBUF_SIZE, cb = cycbuf; i > 0; --i, cb++) {
	    if (cb->list != 0) {
		cb->list += offset;
		cb->last += offset;
	    }
	}
	for (i = cotabsz - cycbrk, co = cotab + cycbrk; i > 0; --i, co++) {
	    if (co->handle == 0) {
		co->prev += offset;
	    }
	    if (co->next != 0) {
		co->next += offset;
	    }
	}
    }

    /* fix up immediate callouts */
    if (nzero != 0) {
	cb = &cycbuf[timestamp & CYCBUF_MASK];
	immediate.list = cb->list;
	for (i = nzero - 1, last = cb->list; i != 0; --i) {
	    last = cotab[last].next;
	}
	immediate.last = last;
	cotab[immediate.list].count = nzero;
	cb->list = cotab[last].next;
	cotab[last].next = 0;
    }

    /* add counts */
    for (i = CYCBUF_SIZE, cb = cycbuf; i != 0; --i, cb++) {
	if (cb->list != 0) {
	    n = 0;
	    last = cb->list;
	    do {
		last = cotab[last].next;
		n++;
	    } while (last != 0);
	    cotab[cb->list].count = n;
	}
    }

    /* restart callouts */
    if (nshort != nzero) {
	for (t = timestamp; cycbuf[t & CYCBUF_MASK].list == 0; t++) ;
	timeout = t;
    }
    restart(timeout);
}
