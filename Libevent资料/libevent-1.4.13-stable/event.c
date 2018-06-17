/*
 * Copyright (c) 2000-2004 Niels Provos <provos@citi.umich.edu>
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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef WIN32_LEAN_AND_MEAN
#endif
#include <sys/types.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#else 
#include <sys/_libevent_time.h>
#endif
#include <sys/queue.h>
#include <stdio.h>
#include <stdlib.h>
#ifndef WIN32
#include <unistd.h>
#endif
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <assert.h>
#include <time.h>

#include "event.h"
#include "event-internal.h"
#include "evutil.h"
#include "log.h"

#ifdef HAVE_EVENT_PORTS
extern const struct eventop evportops;
#endif
#ifdef HAVE_SELECT
extern const struct eventop selectops;
#endif
#ifdef HAVE_POLL
extern const struct eventop pollops;
#endif
#ifdef HAVE_EPOLL
extern const struct eventop epollops;
#endif
#ifdef HAVE_WORKING_KQUEUE
extern const struct eventop kqops;
#endif
#ifdef HAVE_DEVPOLL
extern const struct eventop devpollops;
#endif
#ifdef WIN32
extern const struct eventop win32ops;
#endif

//����֧�ֵ�I/O demultiplex���ƴ洢�����ȫ�־������У��ڱ���׶�ѡ��ʹ�ú��ֻ��ƣ��������ݸ������ȼ�˳����������
/* In order of preference */
static const struct eventop *eventops[] = {
#ifdef HAVE_EVENT_PORTS
	&evportops,
#endif
#ifdef HAVE_WORKING_KQUEUE
	&kqops,
#endif
#ifdef HAVE_EPOLL
	&epollops,
#endif
#ifdef HAVE_DEVPOLL
	&devpollops,
#endif
#ifdef HAVE_POLL
	&pollops,
#endif
#ifdef HAVE_SELECT
	&selectops,
#endif
#ifdef WIN32
	&win32ops,
#endif
	NULL
};

/* Global state */
struct event_base *current_base = NULL;
extern struct event_base *evsignal_base;
static int use_monotonic;

/* Prototypes */
static void	event_queue_insert(struct event_base *, struct event *, int);
static void	event_queue_remove(struct event_base *, struct event *, int);
static int	event_haveevents(struct event_base *);

static void	event_process_active(struct event_base *);

static int	timeout_next(struct event_base *, struct timeval **);
static void	timeout_process(struct event_base *);
static void	timeout_correct(struct event_base *, struct timeval *);

//��ʼ��ʱ����ϵͳ֧�ֵ�ʱ�����ͣ������ڲ�����clock_gettime()�����
//ϵͳ�Ƿ�֧��monotonicʱ�����ͣ������ʱ��
static void
detect_monotonic(void)
{
#if defined(HAVE_CLOCK_GETTIME) && defined(CLOCK_MONOTONIC)   //CLOCK_MONOTONIC��time.h���ж���
	struct timespec	ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
		use_monotonic = 1;
#endif
}

static int
gettime(struct event_base *base, struct timeval *tp)
{
	//���tv_cacheʱ�仺�������ã���ֱ��ʹ��
	//��ÿ��ϵͳ�¼�ѭ���У�ʱ�仺��tv_cache���ᱻ��Ӧ����պ�����
	if (base->tv_cache.tv_sec) {
		*tp = base->tv_cache;
		return (0);
	}
	
	//���֧��monotonic,����clock_gettime��ȡmonotonicʱ�䣬linux��֧�ֵģ������ߵ�����һ����֧
#if defined(HAVE_CLOCK_GETTIME) && defined(CLOCK_MONOTONIC)
	if (use_monotonic) {
		struct timespec	ts;

		if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1)
			return (-1);

		tp->tv_sec = ts.tv_sec;                
		tp->tv_usec = ts.tv_nsec / 1000;   //nanosecond��ת��Ϊu
		return (0);
	}
#endif
	//������֧��MONOTONICʱ��ʱ�Ͳ���Ҫ����һ����֧��
	//����ֻ��ʹ��ϵͳ��ǰʱ��
	//evutil_gettimeofday(tp, NULL))��linux����ʵ����ϵͳ����gettimeofday()
	return (evutil_gettimeofday(tp, NULL));
}

struct event_base *
event_init(void)
{
	struct event_base *base = event_base_new();

	if (base != NULL)
		current_base = base;    //��ʼ����ʱ��������ѵ�ǰevent_base��ֵΪnew������ֵ

	return (base);
}

struct event_base *
event_base_new(void)
{
	int i;
	struct event_base *base;

	if ((base = calloc(1, sizeof(struct event_base))) == NULL)  //�����ڴ�
		event_err(1, "%s: calloc", __func__);

	detect_monotonic();
	gettime(base, &base->event_tv);
	
	min_heap_ctor(&base->timeheap);
	TAILQ_INIT(&base->eventqueue);
	base->sig.ev_signal_pair[0] = -1;
	base->sig.ev_signal_pair[1] = -1;

	//����ϵͳ���úͱ���ѡ�����ʹ����һ��I/O demultiplex����
	//���Կ���,libevent�ڱ���׶�ѡ��ϵͳ��I/O demultiplex���ƣ�����֧�������н׶θ��������ٴ�ѡ��
	base->evbase = NULL;    //��ΪNULL���൱���ҵ�֮��break
	for (i = 0; eventops[i] && !base->evbase; i++) {  //��˳���������Ϊ��Щ�����ǰ������ɴ�С�ŵ�
		base->evsel = eventops[i];

		base->evbase = base->evsel->init(base);
	}

	if (base->evbase == NULL)
		event_errx(1, "%s: no event mechanism available", __func__);

	if (evutil_getenv("EVENT_SHOW_METHOD")) 
		event_msgx("libevent using: %s\n",
			   base->evsel->name);

	/* allocate a single active event queue */
	event_base_priority_init(base, 1);

	return (base);
}

void
event_base_free(struct event_base *base)
{
	int i, n_deleted=0;
	struct event *ev;

	if (base == NULL && current_base)
		base = current_base;
	if (base == current_base)
		current_base = NULL;

	/* XXX(niels) - check for internal events first */
	assert(base);
	/* Delete all non-internal events. */
	for (ev = TAILQ_FIRST(&base->eventqueue); ev; ) {
		struct event *next = TAILQ_NEXT(ev, ev_next);
		if (!(ev->ev_flags & EVLIST_INTERNAL)) {
			event_del(ev);
			++n_deleted;
		}
		ev = next;
	}
	while ((ev = min_heap_top(&base->timeheap)) != NULL) {
		event_del(ev);
		++n_deleted;
	}

	for (i = 0; i < base->nactivequeues; ++i) {
		for (ev = TAILQ_FIRST(base->activequeues[i]); ev; ) {
			struct event *next = TAILQ_NEXT(ev, ev_active_next);
			if (!(ev->ev_flags & EVLIST_INTERNAL)) {
				event_del(ev);
				++n_deleted;
			}
			ev = next;
		}
	}

	if (n_deleted)
		event_debug(("%s: %d events were still set in base",
			__func__, n_deleted));

	if (base->evsel->dealloc != NULL)
		base->evsel->dealloc(base, base->evbase);

	for (i = 0; i < base->nactivequeues; ++i)
		assert(TAILQ_EMPTY(base->activequeues[i]));

	assert(min_heap_empty(&base->timeheap));
	min_heap_dtor(&base->timeheap);

	for (i = 0; i < base->nactivequeues; ++i)
		free(base->activequeues[i]);
	free(base->activequeues);

	assert(TAILQ_EMPTY(&base->eventqueue));

	free(base);
}

/* reinitialized the event base after a fork */
int
event_reinit(struct event_base *base)
{
	const struct eventop *evsel = base->evsel;  //���¼����¼��뷴Ӧ��
	void *evbase = base->evbase;
	int res = 0;
	struct event *ev;

	/* check if this event mechanism requires reinit */
	if (!evsel->need_reinit)     //�������reinit=0��ֱ�ӷ���
		return (0);

	/* prevent internal delete */
	if (base->sig.ev_signal_added) {     //����ź��¼��Ѿ���ע��
		/* we cannot call event_del here because the base has
		 * not been reinitialized yet. */
		event_queue_remove(base, &base->sig.ev_signal,
		    EVLIST_INSERTED);                          //����ע���¼��������Ƴ����¼�
		if (base->sig.ev_signal.ev_flags & EVLIST_ACTIVE)     //����ź��¼��ǻ�Ծ̬���Ƴ���
			event_queue_remove(base, &base->sig.ev_signal,
			    EVLIST_ACTIVE);
		base->sig.ev_signal_added = 0;     //����ź��¼�ע����
	}
	
	if (base->evsel->dealloc != NULL)   //����
		base->evsel->dealloc(base, base->evbase);
	evbase = base->evbase = evsel->init(base);
	if (base->evbase == NULL)
		event_errx(1, "%s: could not reinitialize event mechanism",
		    __func__);

	TAILQ_FOREACH(ev, &base->eventqueue, ev_next) {
		if (evsel->add(evbase, ev) == -1)
			res = -1;
	}

	return (res);
}

int
event_priority_init(int npriorities)
{
  return event_base_priority_init(current_base, npriorities);
}

//��Ծ�¼����ȼ����г�ʼ��
int
event_base_priority_init(struct event_base *base, int npriorities)   //�ڶ�������event_base_new���������ȼ���Ŀ
{                                                                                 
	int i;

	if (base->event_count_active)   //��ʼ��ʱ���ܴ��ڻ�Ծ�¼�
		return (-1);
 
	if (base->nactivequeues && npriorities != base->nactivequeues) {   //�л�Ծ���������ȼ���Ϊna,��Ϊ�±����ֵ��na-1
		for (i = 0; i < base->nactivequeues; ++i) {
			free(base->activequeues[i]);           //ΪʲôҪ���ͷ�?
		}
		free(base->activequeues);
	}

	/* Allocate our priority queues */
	base->nactivequeues = npriorities;
	base->activequeues = (struct event_list **)    //��Ծ�¼����У�event_list����tailqueueʵ�ֵ�
	    calloc(base->nactivequeues, sizeof(struct event_list *));
	if (base->activequeues == NULL)
		event_err(1, "%s: calloc", __func__);

	for (i = 0; i < base->nactivequeues; ++i) {   //��ʼ���������ȼ��Ķ���
		base->activequeues[i] = malloc(sizeof(struct event_list));
		if (base->activequeues[i] == NULL)
			event_err(1, "%s: malloc", __func__);
		TAILQ_INIT(base->activequeues[i]);
	}

	return (0);
}

int
event_haveevents(struct event_base *base)
{
	return (base->event_count > 0);
}

/*
 * Active events are stored in priority queues.  Lower priorities are always
 * process before higher priorities.  Low priority events can starve high
 * priority ones.
 */

static void
event_process_active(struct event_base *base)
{
	struct event *ev;
	struct event_list *activeq = NULL;
	int i;
	short ncalls;

	for (i = 0; i < base->nactivequeues; ++i) {
		if (TAILQ_FIRST(base->activequeues[i]) != NULL) {
			activeq = base->activequeues[i];     //�ҵ�������ȼ���Ϊ��Ծ�¼��������ɴ˿ɼ��������ȼ������ǻ�Ծ�¼���Ҳ���ᴦ��
			break;
		}
	}

	assert(activeq != NULL);

	//���Դ��������ѭ��������libeventĬ��һ���Իᴦ��ͬһ���ȼ���Ծ�������¼�����������event_breakʱ������������ȼ���һ���¼�(�ҽ�ִ��һ��)
	for (ev = TAILQ_FIRST(activeq); ev; ev = TAILQ_FIRST(activeq)) {//���ѭ����ʲô��?(ps��ͨ�ˣ������ɾ����Ծ�����е�һ���¼���
		if (ev->ev_events & EV_PERSIST)                                                 //�ٵ���TAILQ_FIRST���þ�����һ���¼�)
			event_queue_remove(base, ev, EVLIST_ACTIVE);  //����ǳ����¼���ֻ�����ӻ�Ծ������ɾ��
		else
			event_del(ev);   //���ǳ����¼���ֱ�Ӵӻ�Ծ����ͷǻ�Ծ������ɾ��
		
		/* Allows deletes to work */
		ncalls = ev->ev_ncalls;    //ִ���¼���Ӧ�Ļص�����
		ev->ev_pncalls = &ncalls;
		while (ncalls) {
			ncalls--;
			ev->ev_ncalls = ncalls;
			(*ev->ev_callback)((int)ev->ev_fd, ev->ev_res, ev->ev_arg);
			if (base->event_break)     //���������ֱ������ѭ������ִ��һ�ε�һ���¼�
				return;
		}
	}
}

/*
 * Wait continously for events.  We exit only if no events are left.
 */

//�൱�ڵ���0��ǵ�event_base_loop
int
event_dispatch(void)
{
	return (event_loop(0));
}

int
event_base_dispatch(struct event_base *event_base)
{
  return (event_base_loop(event_base, 0));
}

const char *
event_base_get_method(struct event_base *base)
{
	assert(base);
	return (base->evsel->name);
}

static void
event_loopexit_cb(int fd, short what, void *arg)
{
	struct event_base *base = arg;
	base->event_gotterm = 1;
}

/* not thread safe */
int
event_loopexit(const struct timeval *tv)
{
	return (event_once(-1, EV_TIMEOUT, event_loopexit_cb,   //Ϊʲô��-1�������������Ϊ������������������ֻ����
		    current_base, tv));                                               //����һ����ʱ���¼���ʹ�ã��������¼��ڶ೤ʱ������
}                                                                                                //�������event_loopexit_cb�Ϳ��Կ���������ʱ���¼�ʱ�䵽ʱ����λevent_gotterm����ֹ���¼�

int
event_base_loopexit(struct event_base *event_base, const struct timeval *tv)
{
	return (event_base_once(event_base, -1, EV_TIMEOUT, event_loopexit_cb,
		    event_base, tv));
}

/* not thread safe */
int
event_loopbreak(void)
{
	return (event_base_loopbreak(current_base));
}

int
event_base_loopbreak(struct event_base *event_base)
{
	if (event_base == NULL)
		return (-1);

	event_base->event_break = 1;
	return (0);
}


/* not thread safe */

int
event_loop(int flags)
{
	return event_base_loop(current_base, flags);
}

int
event_base_loop(struct event_base *base, int flags)
{
	const struct eventop *evsel = base->evsel;
	void *evbase = base->evbase;
	struct timeval tv;
	struct timeval *tv_p;
	int res, done;

	//���ʱ�仺��
	/* clear time cache */
	base->tv_cache.tv_sec = 0;

	//evsignal_base��ȫ�ֱ������ڴ���signalʱ������ָ��signal������event_baseʵ��
	if (base->sig.ev_signal_added)   //����ź�ʱ���Ѿ���ע����
		evsignal_base = base;
	done = 0;
	while (!done) {
		//�鿴�Ƿ���Ҫ����ѭ����������Ե���event_loopexit_cb()����event_gotterm���
		//����event_base_loopbreak()����event_break���
		/* Terminate the loop if we have been asked to */
		if (base->event_gotterm) {
			base->event_gotterm = 0;
			break;
				
		}

		if (base->event_break) {
			base->event_break = 0;
			break;
		}

		//У��ϵͳʱ�䣬���ϵͳʹ�õ��Ƿ�MONOTONICʱ�䣬�û����ܻ���������ϵͳʱ��
		//��timeout_correcct������Ƚ�last wait time�͵�ǰʱ�䣬�����ǰʱ��С��last wait time
		//����ʱ�������⣬��ʱ��Ҫ����timer_heap�����ж�ʱ�¼��ĳ�ʱʱ��
		timeout_correct(base, &tv);   //linux֧��MONOTONICʱ�䣬���Բ���У��

		//����timer heap�е���С��ʱʱ�䣬����ϵͳI/O demultiplexer��
		tv_p = &tv;
		if (!base->event_count_active && !(flags & EVLOOP_NONBLOCK)) {  //���������ѭ���費����
			timeout_next(base, &tv_p);  //���㵽��һ���¼���ʱ���//����Timerʱ�����evsel-dispatch�����ȴ�ʱ��
		} else { 	
			/* 
			 * if we have active events, we just poll new events
			 * without waiting.
			 */
			evutil_timerclear(&tv);
		}

		//�����ǰû��ע���¼������˳�
		/* If we have no events, we just exit */
		if (!event_haveevents(base)) {
			event_debug(("%s: no events registered.", __func__));
			return (1);
		}

		//����last wait time
		/* update last old time */
		gettime(base, &base->event_tv);

		//���time cache
		/* clear time cache */
		base->tv_cache.tv_sec = 0;

		//����ϵͳI/O demultplexer �ȴ�����I/O�¼�����signal�¼���������epoll_wait����select��
		//��evsel->dispatch()�У���Ѿ���signal event��I/O event���뵽����������
		res = evsel->dispatch(base, evbase, tv_p);  //tv_p�����������Ķ��¼���С��ʱʱ�䣬������Ϊepoll_wait()��I/O���Ƶĳ�ʱʱ�䣬
											 //��ô�������û��I/O�¼�������epoll_wait()��ʱ�����ÿ���ִ����С�ѵĶ�ʱ�¼�
		if (res == -1)
			return (-1);
		//��time cache��ֵΪ��ǰϵͳʱ��
		gettime(base, &base->tv_cache);

		//���heap�е�timer events����������(�����ж��)timer event��heap��ɾ���������뼤��������
		timeout_process(base);

		//����event_process_active()�����������о�����event��������ص�����ִ���¼�����
		//�ú�����Ѱ��������ȼ�(priorityֵԽС���ȼ�Խ��)�ļ����¼�����
		//Ȼ���������е����о����¼�(�������event_break��ִ�����ε�һ���¼�)
		//��˵����ȼ��ľ����¼����ܵò�����ʱ����
		if (base->event_count_active) {
			event_process_active(base);
			if (!base->event_count_active && (flags & EVLOOP_ONCE))
				done = 1;
		} else if (flags & EVLOOP_NONBLOCK)
			done = 1;
	}

	/* clear time cache */
	base->tv_cache.tv_sec = 0;   //ѭ�����������ʱ�仺��

	event_debug(("%s: asked to terminate loop.", __func__));
	return (0);
}

/* Sets up an event for processing once */

struct event_once {
	struct event ev;

	void (*cb)(int, short, void *);
	void *arg;
};

/* One-time callback, it deletes itself */

static void
event_once_cb(int fd, short events, void *arg)
{
	struct event_once *eonce = arg;

	(*eonce->cb)(fd, events, eonce->arg);
	free(eonce);
}

/* not threadsafe, event scheduled once. */
int
event_once(int fd, short events,
    void (*callback)(int, short, void *), void *arg, const struct timeval *tv)
{
	return event_base_once(current_base, fd, events, callback, arg, tv);
}

/* Schedules an event once */
int
event_base_once(struct event_base *base, int fd, short events,
    void (*callback)(int, short, void *), void *arg, const struct timeval *tv)
{
	struct event_once *eonce;
	struct timeval etv;
	int res;

	/* We cannot support signals that just fire once */
	if (events & EV_SIGNAL)   //��ģʽ��֧���ź�
		return (-1);

	if ((eonce = calloc(1, sizeof(struct event_once))) == NULL)
		return (-1);

	eonce->cb = callback;
	eonce->arg = arg;

	if (events == EV_TIMEOUT) {  //���ֻ��һ����ʱ�¼�
		if (tv == NULL) {
			evutil_timerclear(&etv);
			tv = &etv;
		}

		evtimer_set(&eonce->ev, event_once_cb, eonce);
	} else if (events & (EV_READ|EV_WRITE)) {  //����й��ĵ��¼�����
		events &= EV_READ|EV_WRITE;

		event_set(&eonce->ev, fd, events, event_once_cb, eonce);//��fd��events�Ⱥ�eonce->ev��������
	} else {
		/* Bad event combination */
		free(eonce);
		return (-1);
	}

	res = event_base_set(base, &eonce->ev);  //����event_base
	if (res == 0)
		res = event_add(&eonce->ev, tv);  //�ѳ�ʱ�¼�������Ӧ����
	if (res != 0) {
		free(eonce);
		return (res);
	}

	return (0);
}

//evtimer_set(ev,cb,arg) event_set(ev, -1, 0, cb, arg)  //��ʱʱ��ev,cb�ص���arg������-1��0�����Ƕ�ֵ
void
event_set(struct event *ev, int fd, short events,     //events������ʾ��ע���¼�����
	  void (*callback)(int, short, void *), void *arg)   
{
	/* Take the current base - caller needs to set the real base later */
	ev->ev_base = current_base;   //init�����Ѿ���ֵcurrent_base�ˣ����Բ�Ϊ��

	ev->ev_callback = callback;
	ev->ev_arg = arg;
	ev->ev_fd = fd;
	ev->ev_events = events;   //��ע���¼�����
	ev->ev_res = 0;       //��¼�˵�ǰ����ʱ������
	ev->ev_flags = EVLIST_INIT;   //���event��Ϣ���ֶΣ�������ǰ״̬,��EVLIST_TIMEOUT, EVLIST_INSERTED,
	ev->ev_ncalls = 0;     //ʱ�����ʱ�ص�����ִ�еĴ���
	ev->ev_pncalls = NULL;   //ָ��ncalls

	min_heap_elem_init(ev);   //��С�����±��ʼΪ-1//void min_heap_elem_init(struct event* e) { e->min_heap_idx = -1; }

	/* by default, we put new events into the middle priority */
	if(current_base)
		ev->ev_pri = current_base->nactivequeues/2;   //Ĭ�Ͻ����ȼ���Ϊ��Ծ�����м�ֵ����Ծ������±�������ȼ�
}

int
event_base_set(struct event_base *base, struct event *ev)   //����Ѿ�event_set�ˣ����Ҫ����Ĭ��base���Ͳ���event_base_set��
{
	/* Only innocent events may be assigned to a different base */
	if (ev->ev_flags != EVLIST_INIT)     //ֻ��δ�������¼����Է����һ����ͬ��base
		return (-1);

	ev->ev_base = base;
	ev->ev_pri = base->nactivequeues/2;

	return (0);
}

/*
 * Set's the priority of an event - if an event is already scheduled
 * changing the priority is going to fail.
 */

int
event_priority_set(struct event *ev, int pri)
{
	if (ev->ev_flags & EVLIST_ACTIVE)
		return (-1);
	if (pri < 0 || pri >= ev->ev_base->nactivequeues)
		return (-1);

	ev->ev_pri = pri;

	return (0);
}

/*
 * Checks if a specific event is pending or scheduled.
 */
//event_pending()���ڼ��event�ṹ�����ָ�����¼��Ƿ��ڵȴ�״̬.����趨��EV_TIMEOUT,����tv�ṹ��ָ������ǿ�,���¼���ֹʱ����tv����.
int
event_pending(struct event *ev, short event, struct timeval *tv)
{
	struct timeval	now, res;
	int flags = 0;

	if (ev->ev_flags & EVLIST_INSERTED)
		flags |= (ev->ev_events & (EV_READ|EV_WRITE|EV_SIGNAL));
	if (ev->ev_flags & EVLIST_ACTIVE)
		flags |= ev->ev_res;
	if (ev->ev_flags & EVLIST_TIMEOUT)
		flags |= EV_TIMEOUT;

	event &= (EV_TIMEOUT|EV_READ|EV_WRITE|EV_SIGNAL);

	/* See if there is a timeout that we should report */
	if (tv != NULL && (flags & event & EV_TIMEOUT)) {
		gettime(ev->ev_base, &now);
		evutil_timersub(&ev->ev_timeout, &now, &res);
		/* correctly remap to real time */
		evutil_gettimeofday(&now, NULL);
		evutil_timeradd(&now, &res, tv);
	}

	return (flags & event);
}

//���¼����뵽��Ӧ����
int
event_add(struct event *ev, const struct timeval *tv) 
{
	struct event_base *base = ev->ev_base;  //��ȡҪע�ᵽ��event_base
	const struct eventop *evsel = base->evsel;
	void *evbase = base->evbase;     //baseʹ�õ�ϵͳI/O����,ָ��epollop
	int res = 0;

	event_debug((
		 "event_add: event: %p, %s%s%scall %p",
		 ev,
		 ev->ev_events & EV_READ ? "EV_READ " : " ",
		 ev->ev_events & EV_WRITE ? "EV_WRITE " : " ",
		 tv ? "EV_TIMEOUT " : " ",
		 ev->ev_callback));

	assert(!(ev->ev_flags & ~EVLIST_ALL)); //����ȷ���б�־λ

	/*
	 * prepare for timeout insertion further below, if we get a
	 * failure on any step, we should not change any state.
	 */
	//���tv����NULL��ͬʱע�ᶨʱ�¼��������¼����뵽�Ѳ�������
	 //�µ�timer�¼�������timer heap�ӿ��ڶ���Ԥ��һ��λ��
	 //�����ܱ�֤������ԭ����
	 //��ϵͳI/O����ע����ܻ�ʧ�ܣ������ڶ���Ԥ���ɹ���   // ??
	 //��ʱ�¼�����ӽ��϶�����ʧ��
	 //��Ԥ��λ�õĿ��ܽ���Ƕ����䣬�����ڲ�Ԫ�ز�����ı�
	if (tv != NULL && !(ev->ev_flags & EVLIST_TIMEOUT)) {
		if (min_heap_reserve(&base->timeheap,
			1 + min_heap_size(&base->timeheap)) == -1)
			return (-1);  /* ENOMEM == errno */
	}

	//����¼�������ע����߼��������У������evbaseע���¼�������ڲ���tv!=NULL,����Ӷ�ʱ�¼�
	//��һ������ǰ����˵�Ŀ���ʧ��
	if ((ev->ev_events & (EV_READ|EV_WRITE|EV_SIGNAL)) &&
	    !(ev->ev_flags & (EVLIST_INSERTED|EVLIST_ACTIVE))) {
		res = evsel->add(evbase, ev);                //������ӦI/O����������epoll_add
		if (res != -1)           //ע��ɹ�������event����ע��������
			event_queue_insert(base, ev, EVLIST_INSERTED);
	}    //�� event_set(&timer2, -1, EV_READ, thing_2, NULL);������Ӷ�ʱʱ��fd��Ϊ-1��EVREAD��Ϊ0���ɣ���EV_READδ����Ϊ0��ϵͳ��Ϊ��fd�ɶ��������evsel->add��
  		//epoll_add�ж�fd=-1�����������-1����I/O�������ʧ��
	/* 
	 * we should change the timout state only if the previous event
	 * addition succeeded.
	 */
	 //׼����Ӷ�ʱ�¼�
	if (res != -1 && tv != NULL) {
		struct timeval now;

		/* 
		 * we already reserved memory above for the case where we
		 * are not replacing an exisiting timeout.
		 */
		 //EVLIST_TIMEOUT����event�Ѿ��ڶ�ʱ�������ˣ�ɾ���ɵģ�
		if (ev->ev_flags & EVLIST_TIMEOUT)
			event_queue_remove(base, ev, EVLIST_TIMEOUT);

		/* Check if it is active due to a timeout.  Rescheduling
		 * this timeout before the callback can be executed
		 * removes it from the active list. */
		 //����¼��Ѿ��Ǿ���״̬��Ӽ���������ɾ��,��Ϊ����״̬�����޸�
		if ((ev->ev_flags & EVLIST_ACTIVE) &&
		    (ev->ev_res & EV_TIMEOUT)) {    //???
			/* See if we are just active executing this
			 * event in a loop
			 */
			 //��ev_callback���õĴ�������Ϊ0         ???
			if (ev->ev_ncalls && ev->ev_pncalls) {
				/* Abort loop */
				*ev->ev_pncalls = 0;
			}
			
			event_queue_remove(base, ev, EVLIST_ACTIVE);  //�Ӷ�Ӧ��������ɾ��
		}

		//����ʱ��
		gettime(base, &now);
		evutil_timeradd(&now, tv, &ev->ev_timeout);  //���ݵ�ǰʱ����϶�ʱʱ�䣬�õ�������С��ʱʱ��(�¼��������ľ���ʱ��)

		event_debug((
			 "event_add: timeout in %ld seconds, call %p",
			 tv->tv_sec, ev->ev_callback));

		event_queue_insert(base, ev, EVLIST_TIMEOUT);    //���¼����뵽��Ӧ��������
	}  //���res����-1������I/O�������ʧ�ܣ������������Ӷ�ʱ�¼������潫res=-1����

	return (res);   
}

int
event_del(struct event *ev)
{
	struct event_base *base;
	const struct eventop *evsel;
	void *evbase;

	event_debug(("event_del: %p, callback %p",
		 ev, ev->ev_callback));

	/* An event without a base has not been added */
	//ev_baseΪNULL������evû�б�ע��
	if (ev->ev_base == NULL)
		return (-1);

	//ȡ��evע���event_base��eventopָ��
	base = ev->ev_base;
	evsel = base->evsel;
	evbase = base->evbase;

	assert(!(ev->ev_flags & ~EVLIST_ALL));   //ȷ����־λ��ֵ

	/* See if we are just active executing this event in a loop */
	//����callback����
	if (ev->ev_ncalls && ev->ev_pncalls) {
		/* Abort loop */
		*ev->ev_pncalls = 0;
	}

	//�Ӷ�Ӧ��������ɾ��
	if (ev->ev_flags & EVLIST_TIMEOUT)
		event_queue_remove(base, ev, EVLIST_TIMEOUT);

	if (ev->ev_flags & EVLIST_ACTIVE)
		event_queue_remove(base, ev, EVLIST_ACTIVE);

	if (ev->ev_flags & EVLIST_INSERTED) {
		event_queue_remove(base, ev, EVLIST_INSERTED);
		//EVLIST_INSERTED������I/O��signal�¼�
		//ͬʱ��Ҫ����I/O demultiplexerע���¼�
		return (evsel->del(evbase, ev));
	}

	return (0);
}

//���¼������Ѽ�������
void
event_active(struct event *ev, int res, short ncalls)
{
	/* We get different kinds of events, add them together */
	if (ev->ev_flags & EVLIST_ACTIVE) {   //����Ѿ��ӹ��ˣ�����
		ev->ev_res |= res;
		return;
	}

	ev->ev_res = res;
	ev->ev_ncalls = ncalls;
	ev->ev_pncalls = NULL;
	event_queue_insert(ev->ev_base, ev, EVLIST_ACTIVE);
}

//���ݶ��о�����С��ʱֵ���¼��͵�ǰʱ��������ȴ�ʱ��
static int
timeout_next(struct event_base *base, struct timeval **tv_p)
{
	struct timeval now;
	struct event *ev;
	struct timeval *tv = *tv_p;

	//�ѵ���Ԫ�ؾ�����С�ĳ�ʱֵ
	if ((ev = min_heap_top(&base->timeheap)) == NULL) {
		//���û�ж�ʱ�¼������ȴ�ʱ������ΪNULL����ʾһֱ����ֱ����I/O�¼�����
		/* if no time-based events are active wait for I/O */
		*tv_p = NULL;
		return (0);
	}

	//ȡ�õ�ǰʱ��
	if (gettime(base, &now) == -1)
		return (-1);

	//�����ʱʱ��<=��ǰֵ�����ܵȴ�����Ҫ��������
	if (evutil_timercmp(&ev->ev_timeout, &now, <=)) {    // <=������÷��Ƚ����أ��ú궨�彫���������ȥ
		evutil_timerclear(tv);
		return (0);
	}
	
	//����ȴ���ʱ��=��ǰʱ��-��С�ĳ�ʱʱ��
	evutil_timersub(&ev->ev_timeout, &now, tv);

	assert(tv->tv_sec >= 0);
	assert(tv->tv_usec >= 0);

	event_debug(("timeout_next: in %ld seconds", tv->tv_sec));
	return (0);
}

/*
 * Determines if the time is running backwards by comparing the current
 * time against the last time we checked.  Not needed when using clock
 * monotonic.
 */

static void
timeout_correct(struct event_base *base, struct timeval *tv)
{
	struct event **pev;
	unsigned int size;
	struct timeval off;
	if (use_monotonic)  //monotonicʱ���ֱ�ӷ��أ��������������linux��֧��monotonicʱ��ģ�������ֱ�ӷ��ء�
		return;

	/* Check if time is running backwards */
	gettime(base, tv);  //tv <-- tv_cache
	//����ǰ��ķ�������֪��event_tvӦ��С��tv_cache
	//���tv < event_tv�����û�ʱ����ǰ�����ˣ���ҪУ��ʱ��
	if (evutil_timercmp(tv, &base->event_tv, >=)) {
		base->event_tv = *tv;
		return;
	}

	event_debug(("%s: time is running backwards, corrected",
		    __func__));
	//����ʱ���ֵ
	evutil_timersub(&base->event_tv, tv, &off);

	/*
	 * We can modify the key element of the node without destroying
	 * the key, beause we apply it to all in the right order.
	 */
	 //������ʱʱ��С����
	pev = base->timeheap.p;
	size = base->timeheap.n;
	for (; size-- > 0; ++pev) {
		struct timeval *ev_tv = &(**pev).ev_timeout;
		evutil_timersub(ev_tv, &off, ev_tv);
	}
	/* Now remember what the new time turned out to be. */
	base->event_tv = *tv;  //����event_evΪtv_cache
}

void
timeout_process(struct event_base *base)  //��ʱ����
{
	struct timeval now;
	struct event *ev;

	if (min_heap_empty(&base->timeheap))   //return 0 == s->n
		return;

	gettime(base, &now);
													//һ��Ҫע�������õ���ѭ�����п��ܶ���¼���ʱ�ˣ�������Ҫȫ������
	while ((ev = min_heap_top(&base->timeheap))) {    //ȡ����С��ʱ�¼���ѭ����һֱ���ϵ�ȡ�Ѿ���ʱ���¼���ȫ�������Ծ�ȴ�����
		if (evutil_timercmp(&ev->ev_timeout, &now, >))   //����¼���ʱʱ��δ��
			break;

		/* delete this event from the I/O queues */
		event_del(ev);

		event_debug(("timeout_process: call %p",
			 ev->ev_callback));
		event_active(ev, EV_TIMEOUT, 1);
	}
}

void
event_queue_remove(struct event_base *base, struct event *ev, int queue)
{
	if (!(ev->ev_flags & queue))   //ȷ���ڸö�����
		event_errx(1, "%s: %p(fd %d) not on queue %x", __func__,
			   ev, ev->ev_fd, queue);

	if (~ev->ev_flags & EVLIST_INTERNAL)   //������ڲ��¼�����--
		base->event_count--;

	ev->ev_flags &= ~queue;   //��ȥ
	switch (queue) {
	case EVLIST_INSERTED:
		TAILQ_REMOVE(&base->eventqueue, ev, ev_next);
		break;
	case EVLIST_ACTIVE:
		base->event_count_active--;
		TAILQ_REMOVE(base->activequeues[ev->ev_pri],
		    ev, ev_active_next);
		break;
	case EVLIST_TIMEOUT:
		min_heap_erase(&base->timeheap, ev);
		break;
	default:
		event_errx(1, "%s: unknown queue %x", __func__, queue);
	}
}

void
event_queue_insert(struct event_base *base, struct event *ev, int queue)
{
	//ev�¼������Ѿ��ڶ�Ӧ�������ˣ������ظ�����
	if (ev->ev_flags & queue) {             //why double
		/* Double insertion is possible for active events */       //���û����
		if (queue & EVLIST_ACTIVE)
			return;

		event_errx(1, "%s: %p(fd %d) already on queue %x", __func__,
			   ev, ev->ev_fd, queue);
	}

	//��������ڲ��¼����¼���Ŀ��1    //???
	if (~ev->ev_flags & EVLIST_INTERNAL)
		base->event_count++;

	ev->ev_flags |= queue;   //��¼queue���
	switch (queue) {
	case EVLIST_INSERTED:
		TAILQ_INSERT_TAIL(&base->eventqueue, ev, ev_next);
		break;
	case EVLIST_ACTIVE:
		base->event_count_active++;
		TAILQ_INSERT_TAIL(base->activequeues[ev->ev_pri],
		    ev,ev_active_next);
		break;
	case EVLIST_TIMEOUT: {
		min_heap_push(&base->timeheap, ev);
		break;
	}
	default:
		event_errx(1, "%s: unknown queue %x", __func__, queue);
	}
}

/* Functions for debugging */

const char *
event_get_version(void)
{
	return (VERSION);
}

/* 
 * No thread-safe interface needed - the information should be the same
 * for all threads.
 */
//���������֪������������I/O multilplexer
const char *
event_get_method(void)
{
	return (current_base->evsel->name);
}
