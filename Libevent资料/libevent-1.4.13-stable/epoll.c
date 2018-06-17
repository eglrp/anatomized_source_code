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
struct evepoll {      //epoll�Ķ�д����
	struct event *evread;
	struct event *evwrite;
};

struct epollop {
	struct evepoll *fds;    //fdָ��evepoll���飬������������fd��С������Ԫ�ؾ��Ƕ�дevent
	int nfds;                   //fd�ĸ���
	struct epoll_event *events;      //epoll�¼�
	int nevents;    //epoll�¼���Ŀ
	int epfd;   //epollfd
};

//�����eventop�е��Ǽ�������ָ�룬�˴����epollʵ�֣�ע�ⷵ��ֵ�Ͳ�����Ҫ��eventop��һ��
static void *epoll_init	(struct event_base *);
static int epoll_add	(void *, struct event *);
static int epoll_del	(void *, struct event *);
static int epoll_dispatch	(struct event_base *, void *, struct timeval *);
static void epoll_dealloc	(struct event_base *, void *);

const struct eventop epollops = {  //��eventopע��һ��epoll�ĸ�������
	"epoll",
	epoll_init,
	epoll_add,
	epoll_del,
	epoll_dispatch,
	epoll_dealloc,
	1 /* need reinit */
};

#ifdef HAVE_SETFD  //����close-on-exec��Ĭ��Ϊ0������1����
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
#define MAX_EPOLL_TIMEOUT_MSEC (35*60*1000)   //linux��ʱ���ֵ

#define INITIAL_NFILES 32     //��ʼ���ļ���������
#define INITIAL_NEVENTS 32  //�¼���
#define MAX_NEVENTS 4096    //����¼���

static void *
epoll_init(struct event_base *base)
{
	int epfd;
	struct epollop *epollop;

	/* Disable epollueue when this environment variable is set */
	if (evutil_getenv("EVENT_NOEPOLL"))  //����epollueue�����������������,������ʹ��epoll������NULL
		return (NULL);

	/* Initalize the kernel queue */
	if ((epfd = epoll_create(32000)) == -1) {   //����һ��epoll�ľ����32000Ϊ��������¼�����Ŀ��ʵ����ֻ��һ��ռλ
		if (errno != ENOSYS)      //errno.h�ж���#define ENOSYS 38 /* Function not implemented */
			event_warn("epoll_create");
		return (NULL);
	}

	FD_CLOSEONEXEC(epfd);     // fcntl(epollfd, F_SETFD, 1)�����ڰ�ȫ�Կ��ǣ���exec�����йص�fd��
				//close_on_exec�����һ��������ǰ�ȫ�����縸���̴���ĳЩ�ļ���������fork���ӽ��̣�
				//�����ӽ��̾ͻ�Ĭ������Щ�ļ��Ķ�ȡȨ�ޣ����Ǻܶ�ʱ�����ǲ��������ӽ�������ô���Ȩ�ޡ�
	if (!(epollop = calloc(1, sizeof(struct epollop))))
		return (NULL);

	epollop->epfd = epfd;

	/* Initalize fields */
	epollop->events = malloc(INITIAL_NEVENTS * sizeof(struct epoll_event));
	if (epollop->events == NULL) {   //�ԣ����ܺã��˴�����ʧ�ܣ�Ҫ���������ѷ�����ڴ��ͷ�
		free(epollop);
		return (NULL);
	}
	epollop->nevents = INITIAL_NEVENTS;

	epollop->fds = calloc(INITIAL_NFILES, sizeof(struct evepoll));
	if (epollop->fds == NULL) {
		free(epollop->events);    //ͬ��
		free(epollop);
		return (NULL);
	}
	epollop->nfds = INITIAL_NFILES;

	evsignal_init(base);   //��base->sig��ʼ��������base����������������ʼ��socket����

	return (epollop);
}

static int
epoll_recalc(struct event_base *base, void *arg, int max)  //���base����������ûʲô��???
{
	struct epollop *epollop = arg;

	if (max >= epollop->nfds) {     //���fd���ֵ�������fd��nfds�����fd�±�
		struct evepoll *fds;
		int nfds;

		nfds = epollop->nfds;
		while (nfds <= max)   //nfds������2���ķ�ʽֱ������max����
			nfds <<= 1;

		fds = realloc(epollop->fds, nfds * sizeof(struct evepoll));
		if (fds == NULL) {
			event_warn("realloc");
			return (-1);
		}
		epollop->fds = fds;     
		memset(fds + epollop->nfds, 0,     //fds��evepoll*���ͣ�����nfds�����ǳ�ʼ��evepoll������realloc���ǲ���Ϊ0
		    (nfds - epollop->nfds) * sizeof(struct evepoll));   //Ҫ��յĴ�С����recalc��Ĵ�Сnfds-epollop->nfds
		epollop->nfds = nfds;  //����Ϊ�µ�nfds
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
		timeout = tv->tv_sec * 1000 + (tv->tv_usec + 999) / 1000;//���tv��Ϊ�գ����ó�ʱ��Ӧ���ǰѳ�ʱת����ms
														    //��Ϊepoll_wait��ʱ��λ��ms												
	if (timeout > MAX_EPOLL_TIMEOUT_MSEC) {     //���ܴ������
		/* Linux kernels can wait forever if the timeout is too big;
		 * see comment on MAX_EPOLL_TIMEOUT_MSEC. */
		timeout = MAX_EPOLL_TIMEOUT_MSEC;
	}

	res = epoll_wait(epollop->epfd, events, epollop->nevents, timeout);   
	if (res == -1) {
		if (errno != EINTR) {   //EINTR���źŴ�ϵı�־
			event_warn("epoll_wait");
			return (-1);
		}

		evsignal_process(base); //������źţ��ʹ����ɴ˿��Կ���libevent���ź��¼���I/O�¼�ͳһ��I/O���û�����������
		return (0);                         //������ôͳһ�ģ��Һ������ͻ��з���
	} else if (base->sig.evsignal_caught) {    //sig.evsignal_caught���Ƿ����źŷ����ı��
		evsignal_process(base);       //����������źŴ��������Ǹ���EINTR��ʲô�ķ������źŴ���
	}

	event_debug(("%s: epoll_wait reports %d", __func__, res));

	for (i = 0; i < res; i++) {       //�����ص�res����¼�
		int what = events[i].events;      
		struct event *evread = NULL, *evwrite = NULL;
		int fd = events[i].data.fd;   //��¼�������

		if (fd < 0 || fd >= epollop->nfds)    //���<0��������ļ�����Ϊʲô�����������???
			continue;
		evep = &epollop->fds[fd];   //ͨ��fd�������¼��Ķ�д�����ṹ��

		if (what & (EPOLLHUP|EPOLLERR)) {   //�������Ҷϻ��ߴ��� 
			evread = evep->evread;
			evwrite = evep->evwrite;
		} else {
			if (what & EPOLLIN) {   //�ɶ�
				evread = evep->evread;
			}

			if (what & EPOLLOUT) {   //��д
				evwrite = evep->evwrite;
			}
		}

		if (!(evread||evwrite))//����ʱ������,����EPOLLPRI(������fd�н��������¼����Խ��ж�������)��һ�ɺ���
			continue;

		if (evread != NULL)  
			event_active(evread, EV_READ, 1);
		if (evwrite != NULL)
			event_active(evwrite, EV_WRITE, 1);
	}

	//������е��¼����������ˣ������¼����黹��̫С�ˣ���Ҫ��չ����Ĵ�С
	if (res == epollop->nevents && epollop->nevents < MAX_NEVENTS) {   //���epollʱ����Ŀ�Ѿ��������������ֵ������Ϊ2��
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
	struct epollop *epollop = arg;   //�����ʽת���������C++���Ա���
	struct epoll_event epev = {0, {0}};
	struct evepoll *evep;
	int fd, op, events;

	if (ev->ev_events & EV_SIGNAL)   //������ź��¼�����ӵ��ź��¼����У�ΪʲôҪ��epoll_add����������ź��¼�����ΪI/O��signalͳһ
		return (evsignal_add(ev));

	fd = ev->ev_fd;     //��Ӧ��fd������
	if (fd >= epollop->nfds) {   //nfds������������ĸ�����fd�ܺ���ֱ����ȣ�������linux�������������ƣ�0��׼���룬1��׼�����2��׼���󣬴�3��ʼ����
		/* Extent the file descriptor array as necessary */
		if (epoll_recalc(ev->ev_base, epollop, fd) == -1)
			return (-1);
	}
	evep = &epollop->fds[fd];   //��������fd�Ķ�д�����ṹ��evepoll�ṹ��
	op = EPOLL_CTL_ADD;    //Ĭ��Ϊ��
	events = 0;    //��ע���¼�����
	if (evep->evread != NULL) {    //��
		events |= EPOLLIN;
		op = EPOLL_CTL_MOD;
	}
	if (evep->evwrite != NULL) {    //д
		events |= EPOLLOUT;
		op = EPOLL_CTL_MOD;
	}

	if (ev->ev_events & EV_READ)
		events |= EPOLLIN;
	if (ev->ev_events & EV_WRITE)
		events |= EPOLLOUT;

	epev.data.fd = fd;
	epev.events = events;    //��Ϸ�װ��Ϊһ��epoll_event���ͣ�������Ϊepev
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

	if (ev->ev_events & EV_SIGNAL)   //ɾ�ź��¼�
		return (evsignal_del(ev));

	fd = ev->ev_fd;
	if (fd >= epollop->nfds)
		return (0);
	evep = &epollop->fds[fd];

	op = EPOLL_CTL_DEL;
	events = 0;

	if (ev->ev_events & EV_READ)
		events |= EPOLLIN;                   //�������events������
	if (ev->ev_events & EV_WRITE)
		events |= EPOLLOUT;

	//�жϸ��¼��Ƿ�ͬʱ��עEPOLIN��EPOLLOUT����������ڣ�˵����ע��һ��
	if ((events & (EPOLLIN|EPOLLOUT)) != (EPOLLIN|EPOLLOUT)) {   
		if ((events & EPOLLIN) && evep->evwrite != NULL) {   //�����EPOLLIN�����ҿ�д�¼���Ϊ�գ�׼��ɾ����д����
			needwritedelete = 0;
			events = EPOLLOUT;
			op = EPOLL_CTL_MOD;
		} else if ((events & EPOLLOUT) && evep->evread != NULL) { //׼��ɾ���ɶ��¼�
			needreaddelete = 0;
			events = EPOLLIN;
			op = EPOLL_CTL_MOD;
		}
	}

	epev.events = events;
	epev.data.fd = fd;

	if (needreaddelete)
		evep->evread = NULL;    //ɾ���ɶ�����ͬ
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

	evsignal_dealloc(base);   //�����й��źŵ�һϵ�ж���
	if (epollop->fds)
		free(epollop->fds);
	if (epollop->events)
		free(epollop->events);
	if (epollop->epfd >= 0)
		close(epollop->epfd);

	memset(epollop, 0, sizeof(struct epollop));
	free(epollop);
}
