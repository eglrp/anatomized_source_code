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

//所有支持的I/O demultiplex机制存储在这此全局静数组中，在编译阶段选择使用何种机制，数组内容根据优先级顺序声明如下
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

//初始化时会检测系统支持的时间类型，函数内部调用clock_gettime()来检测
//系统是否支持monotonic时钟类型，即相对时间
static void
detect_monotonic(void)
{
#if defined(HAVE_CLOCK_GETTIME) && defined(CLOCK_MONOTONIC)   //CLOCK_MONOTONIC在time.h中有定义
	struct timespec	ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
		use_monotonic = 1;
#endif
}

static int
gettime(struct event_base *base, struct timeval *tp)
{
	//如果tv_cache时间缓存已设置，就直接使用
	//在每次系统事件循环中，时间缓存tv_cache将会被相应的清空和设置
	if (base->tv_cache.tv_sec) {
		*tp = base->tv_cache;
		return (0);
	}
	
	//如果支持monotonic,就用clock_gettime获取monotonic时间，linux是支持的，所以走的是这一条分支
#if defined(HAVE_CLOCK_GETTIME) && defined(CLOCK_MONOTONIC)
	if (use_monotonic) {
		struct timespec	ts;

		if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1)
			return (-1);

		tp->tv_sec = ts.tv_sec;                
		tp->tv_usec = ts.tv_nsec / 1000;   //nanosecond，转化为u
		return (0);
	}
#endif
	//但我们支持MONOTONIC时间时就不需要走这一条分支了
	//否则只能使用系统当前时间
	//evutil_gettimeofday(tp, NULL))在linux下其实就是系统调用gettimeofday()
	return (evutil_gettimeofday(tp, NULL));
}

struct event_base *
event_init(void)
{
	struct event_base *base = event_base_new();

	if (base != NULL)
		current_base = base;    //初始化的时候在这里把当前event_base赋值为new出来的值

	return (base);
}

struct event_base *
event_base_new(void)
{
	int i;
	struct event_base *base;

	if ((base = calloc(1, sizeof(struct event_base))) == NULL)  //申请内存
		event_err(1, "%s: calloc", __func__);

	detect_monotonic();
	gettime(base, &base->event_tv);
	
	min_heap_ctor(&base->timeheap);
	TAILQ_INIT(&base->eventqueue);
	base->sig.ev_signal_pair[0] = -1;
	base->sig.ev_signal_pair[1] = -1;

	//根据系统配置和编译选项决定使用哪一种I/O demultiplex机制
	//可以看出,libevent在编译阶段选择系统的I/O demultiplex机制，而不支持在运行阶段根据配置再次选择
	base->evbase = NULL;    //赋为NULL，相当于找到之后break
	for (i = 0; eventops[i] && !base->evbase; i++) {  //按顺序遍历，因为这些机制是按性能由大到小排的
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
	const struct eventop *evsel = base->evsel;  //将事件重新加入反应堆
	void *evbase = base->evbase;
	int res = 0;
	struct event *ev;

	/* check if this event mechanism requires reinit */
	if (!evsel->need_reinit)     //如果定义reinit=0，直接返回
		return (0);

	/* prevent internal delete */
	if (base->sig.ev_signal_added) {     //如果信号事件已经被注册
		/* we cannot call event_del here because the base has
		 * not been reinitialized yet. */
		event_queue_remove(base, &base->sig.ev_signal,
		    EVLIST_INSERTED);                          //从已注册事件链表中移除该事件
		if (base->sig.ev_signal.ev_flags & EVLIST_ACTIVE)     //如果信号事件是活跃态，移除它
			event_queue_remove(base, &base->sig.ev_signal,
			    EVLIST_ACTIVE);
		base->sig.ev_signal_added = 0;     //清除信号事件注册标记
	}
	
	if (base->evsel->dealloc != NULL)   //析构
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

//活跃事件优先级队列初始化
int
event_base_priority_init(struct event_base *base, int npriorities)   //第二个参数event_base_new传的是优先级数目
{                                                                                 
	int i;

	if (base->event_count_active)   //初始化时不能存在活跃事件
		return (-1);
 
	if (base->nactivequeues && npriorities != base->nactivequeues) {   //有活跃链表，且优先级不为na,因为下标最大值是na-1
		for (i = 0; i < base->nactivequeues; ++i) {
			free(base->activequeues[i]);           //为什么要先释放?
		}
		free(base->activequeues);
	}

	/* Allocate our priority queues */
	base->nactivequeues = npriorities;
	base->activequeues = (struct event_list **)    //活跃事件队列，event_list是用tailqueue实现的
	    calloc(base->nactivequeues, sizeof(struct event_list *));
	if (base->activequeues == NULL)
		event_err(1, "%s: calloc", __func__);

	for (i = 0; i < base->nactivequeues; ++i) {   //初始化各个优先级的队列
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
			activeq = base->activequeues[i];     //找到最高优先级且为活跃事件的链表，由此可见，低优先级就算是活跃事件，也不会处理
			break;
		}
	}

	assert(activeq != NULL);

	//所以从下面这个循环来看，libevent默认一次性会处理同一优先级活跃的所有事件，当设置了event_break时，仅处理该优先级第一个事件(且仅执行一次)
	for (ev = TAILQ_FIRST(activeq); ev; ev = TAILQ_FIRST(activeq)) {//这个循环有什么用?(ps想通了，下面会删除活跃链表中第一个事件，
		if (ev->ev_events & EV_PERSIST)                                                 //再调用TAILQ_FIRST正好就是下一个事件)
			event_queue_remove(base, ev, EVLIST_ACTIVE);  //如果是持续事件，只把它从活跃链表中删除
		else
			event_del(ev);   //不是持续事件，直接从活跃链表和非活跃链表中删除
		
		/* Allows deletes to work */
		ncalls = ev->ev_ncalls;    //执行事件相应的回调函数
		ev->ev_pncalls = &ncalls;
		while (ncalls) {
			ncalls--;
			ev->ev_ncalls = ncalls;
			(*ev->ev_callback)((int)ev->ev_fd, ev->ev_res, ev->ev_arg);
			if (base->event_break)     //如果设置了直接跳出循环，仅执行一次第一个事件
				return;
		}
	}
}

/*
 * Wait continously for events.  We exit only if no events are left.
 */

//相当于调用0标记的event_base_loop
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
	return (event_once(-1, EV_TIMEOUT, event_loopexit_cb,   //为什么给-1这个参数，是因为不关心它的描述符，只把他
		    current_base, tv));                                               //当作一个定时器事件来使用，控制主事件在多长时间后结束
}                                                                                                //从上面的event_loopexit_cb就可以看出，当定时器事件时间到时，置位event_gotterm，终止主事件

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

	//清空时间缓存
	/* clear time cache */
	base->tv_cache.tv_sec = 0;

	//evsignal_base是全局变量，在处理signal时，用于指明signal所属的event_base实例
	if (base->sig.ev_signal_added)   //如果信号时间已经被注册了
		evsignal_base = base;
	done = 0;
	while (!done) {
		//查看是否需要跳出循环，程序可以调用event_loopexit_cb()设置event_gotterm标记
		//调用event_base_loopbreak()设置event_break标记
		/* Terminate the loop if we have been asked to */
		if (base->event_gotterm) {
			base->event_gotterm = 0;
			break;
				
		}

		if (base->event_break) {
			base->event_break = 0;
			break;
		}

		//校正系统时间，如果系统使用的是非MONOTONIC时间，用户可能会向后调整了系统时间
		//在timeout_correcct函数里，比较last wait time和当前时间，如果当前时间小于last wait time
		//表明时间有问题，这时需要更新timer_heap中所有定时事件的超时时间
		timeout_correct(base, &tv);   //linux支持MONOTONIC时间，所以不用校正

		//根据timer heap中的最小超时时间，计算系统I/O demultiplexer的
		tv_p = &tv;
		if (!base->event_count_active && !(flags & EVLOOP_NONBLOCK)) {  //这个是设置循环阻不阻塞
			timeout_next(base, &tv_p);  //计算到下一次事件的时间差//根据Timer时间计算evsel-dispatch的最大等待时间
		} else { 	
			/* 
			 * if we have active events, we just poll new events
			 * without waiting.
			 */
			evutil_timerclear(&tv);
		}

		//如果当前没有注册事件，就退出
		/* If we have no events, we just exit */
		if (!event_haveevents(base)) {
			event_debug(("%s: no events registered.", __func__));
			return (1);
		}

		//更新last wait time
		/* update last old time */
		gettime(base, &base->event_tv);

		//清空time cache
		/* clear time cache */
		base->tv_cache.tv_sec = 0;

		//调用系统I/O demultplexer 等待就绪I/O事件或者signal事件，可能是epoll_wait或者select等
		//在evsel->dispatch()中，会把就绪signal event、I/O event插入到激活链表中
		res = evsel->dispatch(base, evbase, tv_p);  //tv_p就是上面计算的堆事件最小超时时间，用来作为epoll_wait()等I/O机制的超时时间，
											 //那么就算如果没有I/O事件发生，epoll_wait()超时后，正好可以执行最小堆的定时事件
		if (res == -1)
			return (-1);
		//将time cache赋值为当前系统时间
		gettime(base, &base->tv_cache);

		//检查heap中的timer events，将就绪的(可能有多个)timer event从heap上删除，并插入激活链表中
		timeout_process(base);

		//调用event_process_active()处理激活链表中就绪的event，调用其回调函数执行事件处理
		//该函数会寻找最高优先级(priority值越小优先级越高)的激活事件链表
		//然后处理链表中的所有就绪事件(如果设置event_break仅执行依次第一个事件)
		//因此低优先级的就绪事件可能得不到及时处理
		if (base->event_count_active) {
			event_process_active(base);
			if (!base->event_count_active && (flags & EVLOOP_ONCE))
				done = 1;
		} else if (flags & EVLOOP_NONBLOCK)
			done = 1;
	}

	/* clear time cache */
	base->tv_cache.tv_sec = 0;   //循环结束，清空时间缓存

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
	if (events & EV_SIGNAL)   //该模式不支持信号
		return (-1);

	if ((eonce = calloc(1, sizeof(struct event_once))) == NULL)
		return (-1);

	eonce->cb = callback;
	eonce->arg = arg;

	if (events == EV_TIMEOUT) {  //如果只是一个定时事件
		if (tv == NULL) {
			evutil_timerclear(&etv);
			tv = &etv;
		}

		evtimer_set(&eonce->ev, event_once_cb, eonce);
	} else if (events & (EV_READ|EV_WRITE)) {  //如果有关心的事件属性
		events &= EV_READ|EV_WRITE;

		event_set(&eonce->ev, fd, events, event_once_cb, eonce);//把fd，events等和eonce->ev关联起来
	} else {
		/* Bad event combination */
		free(eonce);
		return (-1);
	}

	res = event_base_set(base, &eonce->ev);  //关联event_base
	if (res == 0)
		res = event_add(&eonce->ev, tv);  //把超时事件加入相应队列
	if (res != 0) {
		free(eonce);
		return (res);
	}

	return (0);
}

//evtimer_set(ev,cb,arg) event_set(ev, -1, 0, cb, arg)  //超时时间ev,cb回调，arg参数，-1，0，都是定值
void
event_set(struct event *ev, int fd, short events,     //events参数表示关注的事件类型
	  void (*callback)(int, short, void *), void *arg)   
{
	/* Take the current base - caller needs to set the real base later */
	ev->ev_base = current_base;   //init函数已经赋值current_base了，所以不为空

	ev->ev_callback = callback;
	ev->ev_arg = arg;
	ev->ev_fd = fd;
	ev->ev_events = events;   //关注的事件类型
	ev->ev_res = 0;       //记录了当前激活时间类型
	ev->ev_flags = EVLIST_INIT;   //标记event信息的字段，表明当前状态,如EVLIST_TIMEOUT, EVLIST_INSERTED,
	ev->ev_ncalls = 0;     //时间就绪时回调函数执行的次数
	ev->ev_pncalls = NULL;   //指向ncalls

	min_heap_elem_init(ev);   //最小堆中下标初始为-1//void min_heap_elem_init(struct event* e) { e->min_heap_idx = -1; }

	/* by default, we put new events into the middle priority */
	if(current_base)
		ev->ev_pri = current_base->nactivequeues/2;   //默认将优先级设为活跃链表中间值，活跃链表的下标就是优先级
}

int
event_base_set(struct event_base *base, struct event *ev)   //如果已经event_set了，如果要关联默认base，就不用event_base_set了
{
	/* Only innocent events may be assigned to a different base */
	if (ev->ev_flags != EVLIST_INIT)     //只有未关联的事件可以分配给一个不同的base
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
//event_pending()用于检测event结构体变量指定的事件是否处于等待状态.如果设定了EV_TIMEOUT,并且tv结构体指针变量非空,则事件终止时间由tv返回.
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

//将事件加入到相应队列
int
event_add(struct event *ev, const struct timeval *tv) 
{
	struct event_base *base = ev->ev_base;  //获取要注册到的event_base
	const struct eventop *evsel = base->evsel;
	void *evbase = base->evbase;     //base使用的系统I/O策略,指向epollop
	int res = 0;

	event_debug((
		 "event_add: event: %p, %s%s%scall %p",
		 ev,
		 ev->ev_events & EV_READ ? "EV_READ " : " ",
		 ev->ev_events & EV_WRITE ? "EV_WRITE " : " ",
		 tv ? "EV_TIMEOUT " : " ",
		 ev->ev_callback));

	assert(!(ev->ev_flags & ~EVLIST_ALL)); //断言确保有标志位

	/*
	 * prepare for timeout insertion further below, if we get a
	 * failure on any step, we should not change any state.
	 */
	//如果tv不是NULL，同时注册定时事件，否则将事件插入到已插入链表
	 //新的timer事件，调用timer heap接口在堆上预留一个位置
	 //这样能保证操作的原子性
	 //向系统I/O机制注册可能会失败，而当在堆上预留成功后   // ??
	 //定时事件的添加将肯定不会失败
	 //而预留位置的可能结果是堆扩充，但是内部元素并不会改变
	if (tv != NULL && !(ev->ev_flags & EVLIST_TIMEOUT)) {
		if (min_heap_reserve(&base->timeheap,
			1 + min_heap_size(&base->timeheap)) == -1)
			return (-1);  /* ENOMEM == errno */
	}

	//如果事件不在已注册或者激活链表中，则调用evbase注册事件。如果在并且tv!=NULL,仅添加定时事件
	//这一步就是前面所说的可能失败
	if ((ev->ev_events & (EV_READ|EV_WRITE|EV_SIGNAL)) &&
	    !(ev->ev_flags & (EVLIST_INSERTED|EVLIST_ACTIVE))) {
		res = evsel->add(evbase, ev);                //加上相应I/O操作，比如epoll_add
		if (res != -1)           //注册成功，插入event到已注册链表中
			event_queue_insert(base, ev, EVLIST_INSERTED);
	}    //如 event_set(&timer2, -1, EV_READ, thing_2, NULL);本来添加定时时间fd设为-1，EVREAD设为0即可，若EV_READ未设置为0，系统认为该fd可读，会调用evsel->add，
  		//epoll_add中对fd=-1这种情况返回-1，则I/O机制添加失败
	/* 
	 * we should change the timout state only if the previous event
	 * addition succeeded.
	 */
	 //准备添加定时事件
	if (res != -1 && tv != NULL) {
		struct timeval now;

		/* 
		 * we already reserved memory above for the case where we
		 * are not replacing an exisiting timeout.
		 */
		 //EVLIST_TIMEOUT表明event已经在定时器堆中了，删除旧的！
		if (ev->ev_flags & EVLIST_TIMEOUT)
			event_queue_remove(base, ev, EVLIST_TIMEOUT);

		/* Check if it is active due to a timeout.  Rescheduling
		 * this timeout before the callback can be executed
		 * removes it from the active list. */
		 //如果事件已经是就绪状态则从激活链表中删除,因为就绪状态不能修改
		if ((ev->ev_flags & EVLIST_ACTIVE) &&
		    (ev->ev_res & EV_TIMEOUT)) {    //???
			/* See if we are just active executing this
			 * event in a loop
			 */
			 //将ev_callback调用的次数设置为0         ???
			if (ev->ev_ncalls && ev->ev_pncalls) {
				/* Abort loop */
				*ev->ev_pncalls = 0;
			}
			
			event_queue_remove(base, ev, EVLIST_ACTIVE);  //从对应的链表中删除
		}

		//计算时间
		gettime(base, &now);
		evutil_timeradd(&now, tv, &ev->ev_timeout);  //根据当前时间加上定时时间，得到堆中最小超时时间(事件被触发的绝对时间)

		event_debug((
			 "event_add: timeout in %ld seconds, call %p",
			 tv->tv_sec, ev->ev_callback));

		event_queue_insert(base, ev, EVLIST_TIMEOUT);    //将事件插入到对应的链表中
	}  //如果res等于-1，就是I/O机制添加失败，不再向堆中添加定时事件，下面将res=-1返回

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
	//ev_base为NULL，表明ev没有被注册
	if (ev->ev_base == NULL)
		return (-1);

	//取得ev注册的event_base和eventop指针
	base = ev->ev_base;
	evsel = base->evsel;
	evbase = base->evbase;

	assert(!(ev->ev_flags & ~EVLIST_ALL));   //确保标志位有值

	/* See if we are just active executing this event in a loop */
	//清零callback次数
	if (ev->ev_ncalls && ev->ev_pncalls) {
		/* Abort loop */
		*ev->ev_pncalls = 0;
	}

	//从对应的链表中删除
	if (ev->ev_flags & EVLIST_TIMEOUT)
		event_queue_remove(base, ev, EVLIST_TIMEOUT);

	if (ev->ev_flags & EVLIST_ACTIVE)
		event_queue_remove(base, ev, EVLIST_ACTIVE);

	if (ev->ev_flags & EVLIST_INSERTED) {
		event_queue_remove(base, ev, EVLIST_INSERTED);
		//EVLIST_INSERTED表明是I/O或signal事件
		//同时需要调用I/O demultiplexer注销事件
		return (evsel->del(evbase, ev));
	}

	return (0);
}

//将事件插入已激活链表
void
event_active(struct event *ev, int res, short ncalls)
{
	/* We get different kinds of events, add them together */
	if (ev->ev_flags & EVLIST_ACTIVE) {   //如果已经加过了，返回
		ev->ev_res |= res;
		return;
	}

	ev->ev_res = res;
	ev->ev_ncalls = ncalls;
	ev->ev_pncalls = NULL;
	event_queue_insert(ev->ev_base, ev, EVLIST_ACTIVE);
}

//根据堆中具有最小超时值的事件和当前时间来计算等待时间
static int
timeout_next(struct event_base *base, struct timeval **tv_p)
{
	struct timeval now;
	struct event *ev;
	struct timeval *tv = *tv_p;

	//堆的首元素具有最小的超时值
	if ((ev = min_heap_top(&base->timeheap)) == NULL) {
		//如果没有定时事件，将等待时间设置为NULL，表示一直阻塞直到有I/O事件发生
		/* if no time-based events are active wait for I/O */
		*tv_p = NULL;
		return (0);
	}

	//取得当前时间
	if (gettime(base, &now) == -1)
		return (-1);

	//如果超时时间<=当前值，不能等待，需要立即返回
	if (evutil_timercmp(&ev->ev_timeout, &now, <=)) {    // <=，这个用法比较奇特，用宏定义将运算符传过去
		evutil_timerclear(tv);
		return (0);
	}
	
	//计算等待的时间=当前时间-最小的超时时间
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
	if (use_monotonic)  //monotonic时间就直接返回，无需调整，所以linux是支持monotonic时间的，在这里直接返回。
		return;

	/* Check if time is running backwards */
	gettime(base, tv);  //tv <-- tv_cache
	//根据前面的分析可以知道event_tv应该小于tv_cache
	//如果tv < event_tv表明用户时间向前调整了，需要校正时间
	if (evutil_timercmp(tv, &base->event_tv, >=)) {
		base->event_tv = *tv;
		return;
	}

	event_debug(("%s: time is running backwards, corrected",
		    __func__));
	//计算时间差值
	evutil_timersub(&base->event_tv, tv, &off);

	/*
	 * We can modify the key element of the node without destroying
	 * the key, beause we apply it to all in the right order.
	 */
	 //调整定时时间小根堆
	pev = base->timeheap.p;
	size = base->timeheap.n;
	for (; size-- > 0; ++pev) {
		struct timeval *ev_tv = &(**pev).ev_timeout;
		evutil_timersub(ev_tv, &off, ev_tv);
	}
	/* Now remember what the new time turned out to be. */
	base->event_tv = *tv;  //更新event_ev为tv_cache
}

void
timeout_process(struct event_base *base)  //超时处理
{
	struct timeval now;
	struct event *ev;

	if (min_heap_empty(&base->timeheap))   //return 0 == s->n
		return;

	gettime(base, &now);
													//一定要注意这里用的是循环，有可能多个事件超时了，我们需要全部处理
	while ((ev = min_heap_top(&base->timeheap))) {    //取得最小定时事件，循环，一直不断地取已经超时的事件，全部加入活跃等待队列
		if (evutil_timercmp(&ev->ev_timeout, &now, >))   //如果事件超时时间未到
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
	if (!(ev->ev_flags & queue))   //确保在该队列中
		event_errx(1, "%s: %p(fd %d) not on queue %x", __func__,
			   ev, ev->ev_fd, queue);

	if (~ev->ev_flags & EVLIST_INTERNAL)   //如果是内部事件，则--
		base->event_count--;

	ev->ev_flags &= ~queue;   //除去
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
	//ev事件可能已经在对应链表中了，避免重复插入
	if (ev->ev_flags & queue) {             //why double
		/* Double insertion is possible for active events */       //这个没明白
		if (queue & EVLIST_ACTIVE)
			return;

		event_errx(1, "%s: %p(fd %d) already on queue %x", __func__,
			   ev, ev->ev_fd, queue);
	}

	//如果不是内部事件，事件数目加1    //???
	if (~ev->ev_flags & EVLIST_INTERNAL)
		base->event_count++;

	ev->ev_flags |= queue;   //记录queue标记
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
//用这个可以知道采用了哪种I/O multilplexer
const char *
event_get_method(void)
{
	return (current_base->evsel->name);
}
