/*
 * Copyright 2000-2003 Niels Provos <provos@citi.umich.edu>
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

#include <stdint.h>
#include <sys/types.h>
#include <sys/resource.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#else
#include <sys/_libevent_time.h>
#endif
#include <sys/queue.h>
#include <sys/epoll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif

#include "event.h"
#include "event-internal.h"
#include "evsignal.h"
#include "log.h"

/* due to limitations in the epoll interface, we need to keep track of
 * all file descriptors outself.
 */
struct evepoll {      //epoll的读写操作
	struct event *evread;
	struct event *evwrite;
};

struct epollop {
	struct evepoll *fds;    //fd指向evepoll数组，数组索引就是fd大小，数组元素就是读写event
	int nfds;                   //fd的个数
	struct epoll_event *events;      //epoll事件
	int nevents;    //epoll事件数目
	int epfd;   //epollfd
};

//这就是eventop中的那几个函数指针，此处针对epoll实现，注意返回值和参数都要和eventop中一致
static void *epoll_init	(struct event_base *);
static int epoll_add	(void *, struct event *);
static int epoll_del	(void *, struct event *);
static int epoll_dispatch	(struct event_base *, void *, struct timeval *);
static void epoll_dealloc	(struct event_base *, void *);

const struct eventop epollops = {  //在eventop注册一下epoll的各个方法
	"epoll",
	epoll_init,
	epoll_add,
	epoll_del,
	epoll_dispatch,
	epoll_dealloc,
	1 /* need reinit */
};

#ifdef HAVE_SETFD  //设置close-on-exec，默认为0，设置1即可
#define FD_CLOSEONEXEC(x) do { \          
        if (fcntl(x, F_SETFD, 1) == -1) \   
                event_warn("fcntl(%d, F_SETFD)", x); \      
} while (0)
#else
#define FD_CLOSEONEXEC(x)
#endif

/* On Linux kernels at least up to 2.6.24.4, epoll can't handle timeout
 * values bigger than (LONG_MAX - 999ULL)/HZ.  HZ in the wild can be
 * as big as 1000, and LONG_MAX can be as small as (1<<31)-1, so the
 * largest number of msec we can support here is 2147482.  Let's
 * round that down by 47 seconds.
 */
#define MAX_EPOLL_TIMEOUT_MSEC (35*60*1000)   //linux超时最大值

#define INITIAL_NFILES 32     //初始的文件描述符数
#define INITIAL_NEVENTS 32  //事件数
#define MAX_NEVENTS 4096    //最大事件数

static void *
epoll_init(struct event_base *base)
{
	int epfd;
	struct epollop *epollop;

	/* Disable epollueue when this environment variable is set */
	if (evutil_getenv("EVENT_NOEPOLL"))  //禁用epollueue当这个环境变量设置,不允许使用epoll，返回NULL
		return (NULL);

	/* Initalize the kernel queue */
	if ((epfd = epoll_create(32000)) == -1) {   //创建一个epoll的句柄，32000为建议监听事件的数目，实际上只是一个占位
		if (errno != ENOSYS)      //errno.h中定义#define ENOSYS 38 /* Function not implemented */
			event_warn("epoll_create");
		return (NULL);
	}

	FD_CLOSEONEXEC(epfd);     // fcntl(epollfd, F_SETFD, 1)，出于安全性考虑，在exec程序中关掉fd。
				//close_on_exec另外的一大意义就是安全。比如父进程打开了某些文件，父进程fork了子进程，
				//但是子进程就会默认有这些文件的读取权限，但是很多时候我们并不想让子进程有这么多的权限。
	if (!(epollop = calloc(1, sizeof(struct epollop))))
		return (NULL);

	epollop->epfd = epfd;

	/* Initalize fields */
	epollop->events = malloc(INITIAL_NEVENTS * sizeof(struct epoll_event));
	if (epollop->events == NULL) {   //对，这点很好，此处分配失败，要将上文中已分配的内存释放
		free(epollop);
		return (NULL);
	}
	epollop->nevents = INITIAL_NEVENTS;

	epollop->fds = calloc(INITIAL_NFILES, sizeof(struct evepoll));
	if (epollop->fds == NULL) {
		free(epollop->events);    //同理
		free(epollop);
		return (NULL);
	}
	epollop->nfds = INITIAL_NFILES;

	evsignal_init(base);   //把base->sig初始化，并和base关联起来，包括初始化socket对组

	return (epollop);
}

static int
epoll_recalc(struct event_base *base, void *arg, int max)  //这个base参数在这里没什么用???
{
	struct epollop *epollop = arg;

	if (max >= epollop->nfds) {     //如果fd最大值大于最大fd，nfds是最大fd下标
		struct evepoll *fds;
		int nfds;

		nfds = epollop->nfds;
		while (nfds <= max)   //nfds以扩大2倍的方式直到满足max需求
			nfds <<= 1;

		fds = realloc(epollop->fds, nfds * sizeof(struct evepoll));
		if (fds == NULL) {
			event_warn("realloc");
			return (-1);
		}
		epollop->fds = fds;     
		memset(fds + epollop->nfds, 0,     //fds是evepoll*类型，加上nfds，就是初始化evepoll数组新realloc的那部分为0
		    (nfds - epollop->nfds) * sizeof(struct evepoll));   //要清空的大小是新recalc后的大小nfds-epollop->nfds
		epollop->nfds = nfds;  //更新为新的nfds
	}

	return (0);
}

static int
epoll_dispatch(struct event_base *base, void *arg, struct timeval *tv)   //base, evbase, tv_p
{
	struct epollop *epollop = arg;
	struct epoll_event *events = epollop->events;
	struct evepoll *evep;
	int i, res, timeout = -1;

	if (tv != NULL)
		timeout = tv->tv_sec * 1000 + (tv->tv_usec + 999) / 1000;//如果tv不为空，设置超时，应该是把超时转化成ms
														    //因为epoll_wait超时单位是ms												
	if (timeout > MAX_EPOLL_TIMEOUT_MSEC) {     //不能大于最大
		/* Linux kernels can wait forever if the timeout is too big;
		 * see comment on MAX_EPOLL_TIMEOUT_MSEC. */
		timeout = MAX_EPOLL_TIMEOUT_MSEC;
	}

	res = epoll_wait(epollop->epfd, events, epollop->nevents, timeout);   
	if (res == -1) {
		if (errno != EINTR) {   //EINTR是信号打断的标志
			event_warn("epoll_wait");
			return (-1);
		}

		evsignal_process(base); //如果有信号，就处理，由此可以看出libevent将信号事件和I/O事件统一用I/O复用机制来处理了
		return (0);                         //至于怎么统一的，我后续博客会有分析
	} else if (base->sig.evsignal_caught) {    //sig.evsignal_caught是是否有信号发生的标记
		evsignal_process(base);       //这个是正常信号处理，上面那个是EINTR，什么的非正常信号处理
	}

	event_debug(("%s: epoll_wait reports %d", __func__, res));

	for (i = 0; i < res; i++) {       //处理返回的res个活动事件
		int what = events[i].events;      
		struct event *evread = NULL, *evwrite = NULL;
		int fd = events[i].data.fd;   //活动事件描述符

		if (fd < 0 || fd >= epollop->nfds)    //如果<0或大于最大的继续，为什么有这两种情况???
			continue;
		evep = &epollop->fds[fd];   //通过fd索引该事件的读写操作结构体

		if (what & (EPOLLHUP|EPOLLERR)) {   //描述符挂断或者错误 
			evread = evep->evread;
			evwrite = evep->evwrite;
		} else {
			if (what & EPOLLIN) {   //可读
				evread = evep->evread;
			}

			if (what & EPOLLOUT) {   //可写
				evwrite = evep->evwrite;
			}
		}

		if (!(evread||evwrite))//其他时间类型,比如EPOLLPRI(关联的fd有紧急优先事件可以进行读操作了)，一律忽略
			continue;

		if (evread != NULL)  
			event_active(evread, EV_READ, 1);
		if (evwrite != NULL)
			event_active(evwrite, EV_WRITE, 1);
	}

	//如果所有的事件都被触发了，表明事件数组还是太小了，需要扩展数组的大小
	if (res == epollop->nevents && epollop->nevents < MAX_NEVENTS) {   //如果epoll时间数目已经到达已设置最大值，扩充为2倍
		/* We used all of the event space this time.  We should
		   be ready for more events next time. */
		int new_nevents = epollop->nevents * 2;
		struct epoll_event *new_events;

		new_events = realloc(epollop->events,
		    new_nevents * sizeof(struct epoll_event));
		if (new_events) {
			epollop->events = new_events;
			epollop->nevents = new_nevents;
		}
	}

	return (0);
}


static int
epoll_add(void *arg, struct event *ev)
{
	struct epollop *epollop = arg;   //这个隐式转换，如果换C++绝对报错
	struct epoll_event epev = {0, {0}};
	struct evepoll *evep;
	int fd, op, events;

	if (ev->ev_events & EV_SIGNAL)   //如果是信号事件，添加到信号事件队列，为什么要在epoll_add函数中添加信号事件，因为I/O和signal统一
		return (evsignal_add(ev));

	fd = ev->ev_fd;     //对应的fd描述符
	if (fd >= epollop->nfds) {   //nfds是描述符数组的个数，fd能和它直接相比，得益于linux描述符递增机制，0标准输入，1标准输出，2标准错误，从3开始递增
		/* Extent the file descriptor array as necessary */
		if (epoll_recalc(ev->ev_base, epollop, fd) == -1)
			return (-1);
	}
	evep = &epollop->fds[fd];   //索引到该fd的读写操作结构体evepoll结构体
	op = EPOLL_CTL_ADD;    //默认为加
	events = 0;    //关注的事件类型
	if (evep->evread != NULL) {    //读
		events |= EPOLLIN;
		op = EPOLL_CTL_MOD;
	}
	if (evep->evwrite != NULL) {    //写
		events |= EPOLLOUT;
		op = EPOLL_CTL_MOD;
	}

	if (ev->ev_events & EV_READ)
		events |= EPOLLIN;
	if (ev->ev_events & EV_WRITE)
		events |= EPOLLOUT;

	epev.data.fd = fd;
	epev.events = events;    //组合封装成为一个epoll_event类型，变量名为epev
	if (epoll_ctl(epollop->epfd, op, ev->ev_fd, &epev) == -1)
			return (-1);

	/* Update events responsible */
	if (ev->ev_events & EV_READ)
		evep->evread = ev;
	if (ev->ev_events & EV_WRITE)
		evep->evwrite = ev;

	return (0);
}

static int
epoll_del(void *arg, struct event *ev)
{
	struct epollop *epollop = arg;
	struct epoll_event epev = {0, {0}};
	struct evepoll *evep;
	int fd, events, op;
	int needwritedelete = 1, needreaddelete = 1;

	if (ev->ev_events & EV_SIGNAL)   //删信号事件
		return (evsignal_del(ev));

	fd = ev->ev_fd;
	if (fd >= epollop->nfds)
		return (0);
	evep = &epollop->fds[fd];

	op = EPOLL_CTL_DEL;
	events = 0;

	if (ev->ev_events & EV_READ)
		events |= EPOLLIN;                   //给上面的events加属性
	if (ev->ev_events & EV_WRITE)
		events |= EPOLLOUT;

	//判断该事件是否同时关注EPOLIN和EPOLLOUT，如果不等于，说明关注了一个
	if ((events & (EPOLLIN|EPOLLOUT)) != (EPOLLIN|EPOLLOUT)) {   
		if ((events & EPOLLIN) && evep->evwrite != NULL) {   //如果是EPOLLIN，并且可写事件不为空，准备删除可写属性
			needwritedelete = 0;
			events = EPOLLOUT;
			op = EPOLL_CTL_MOD;
		} else if ((events & EPOLLOUT) && evep->evread != NULL) { //准备删除可读事件
			needreaddelete = 0;
			events = EPOLLIN;
			op = EPOLL_CTL_MOD;
		}
	}

	epev.events = events;
	epev.data.fd = fd;

	if (needreaddelete)
		evep->evread = NULL;    //删除可读，下同
	if (needwritedelete)
		evep->evwrite = NULL;

	if (epoll_ctl(epollop->epfd, op, fd, &epev) == -1)
		return (-1);

	return (0);
}

static void
epoll_dealloc(struct event_base *base, void *arg)
{
	struct epollop *epollop = arg;

	evsignal_dealloc(base);   //销毁有关信号的一系列东西
	if (epollop->fds)
		free(epollop->fds);
	if (epollop->events)
		free(epollop->events);
	if (epollop->epfd >= 0)
		close(epollop->epfd);

	memset(epollop, 0, sizeof(struct epollop));
	free(epollop);
}
