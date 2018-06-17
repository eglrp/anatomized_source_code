/*	$OpenBSD: select.c,v 1.2 2002/06/25 15:50:15 mickey Exp $	*/

/*
 * Copyright 2000-2002 Niels Provos <provos@citi.umich.edu>
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
#include <winsock2.h>
#include <windows.h>
#undef WIN32_LEAN_AND_MEAN
#endif
#include <sys/types.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#include <sys/queue.h>
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <errno.h>
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#include <assert.h>

#include "event.h"
#include "event-internal.h"
#include "evsignal.h"
#include "evutil.h"
#include "log.h"

struct event_base *evsignal_base = NULL;

static void evsignal_handler(int sig);

/* Callback for when the signal handler write a byte to our signaling socket */
static void
evsignal_cb(int fd, short what, void *arg)
{
	static char signals[1];   //static不可重入
#ifdef WIN32
	SSIZE_T n;
#else
	ssize_t n;
#endif

	n = recv(fd, signals, sizeof(signals), 0);   //用上面的signals接收
	if (n == -1)
		event_err(1, "%s: read", __func__);
}

#ifdef HAVE_SETFD
#define FD_CLOSEONEXEC(x) do { \
        if (fcntl(x, F_SETFD, 1) == -1) \
                event_warn("fcntl(%d, F_SETFD)", x); \
} while (0)
#else
#define FD_CLOSEONEXEC(x)
#endif

int
evsignal_init(struct event_base *base)   
{
	int i;

	/* 
	 * Our signal handler is going to write to one end of the socket
	 * pair to wake up our event loop.  The event loop then scans for
	 * signals that got delivered.
	 */
	if (evutil_socketpair(
		    AF_UNIX, SOCK_STREAM, 0, base->sig.ev_signal_pair) == -1) {//创建AF_UNIX本地套接字通信
#ifdef WIN32
		/* Make this nonfatal on win32, where sometimes people
		   have localhost firewalled. */
		event_warn("%s: socketpair", __func__);
#else
		event_err(1, "%s: socketpair", __func__);
#endif
		return -1;
	}

	FD_CLOSEONEXEC(base->sig.ev_signal_pair[0]);
	FD_CLOSEONEXEC(base->sig.ev_signal_pair[1]);
	base->sig.sh_old = NULL;
	base->sig.sh_old_max = 0;
	base->sig.evsignal_caught = 0;
	memset(&base->sig.evsigcaught, 0, sizeof(sig_atomic_t)*NSIG);   //sig_atomic_t信号原子类型
	/* initialize the queues for all events */
	for (i = 0; i < NSIG; ++i)    //NSIG为signo最大值
		TAILQ_INIT(&base->sig.evsigevents[i]);

        evutil_make_socket_nonblocking(base->sig.ev_signal_pair[0]);

	event_set(&base->sig.ev_signal, base->sig.ev_signal_pair[1],
		EV_READ | EV_PERSIST, evsignal_cb, &base->sig.ev_signal);  //evsignal_cb用来接收socket发来的那一个字节,无需处理
	base->sig.ev_signal.ev_base = base;
	base->sig.ev_signal.ev_flags |= EVLIST_INTERNAL;  //内部事件，这里就是传说中的内部事件，socketpair

	return 0;
}

/* Helper: set the signal handler for evsignal to handler in base, so that
 * we can restore the original handler when we clear the current one. */
int
_evsignal_set_handler(struct event_base *base,
		      int evsignal, void (*handler)(int))
{
#ifdef HAVE_SIGACTION
	struct sigaction sa;   //信号处理结构体
#else
	ev_sighandler_t sh;
#endif
	struct evsignal_info *sig = &base->sig;
	void *p;

	/*
	 * resize saved signal handler array up to the highest signal number.
	 * a dynamic array is used to keep footprint on the low side.
	 */
	if (evsignal >= sig->sh_old_max) {     //如果大于等于旧的最大值
		int new_max = evsignal + 1;
		event_debug(("%s: evsignal (%d) >= sh_old_max (%d), resizing",
			    __func__, evsignal, sig->sh_old_max));
		p = realloc(sig->sh_old, new_max * sizeof(*sig->sh_old));
		if (p == NULL) {
			event_warn("realloc");
			return (-1);
		}

		memset((char *)p + sig->sh_old_max * sizeof(*sig->sh_old),
		    0, (new_max - sig->sh_old_max) * sizeof(*sig->sh_old));

		sig->sh_old_max = new_max;
		sig->sh_old = p;
	}

	/* allocate space for previous handler out of dynamic array */
	sig->sh_old[evsignal] = malloc(sizeof *sig->sh_old[evsignal]);
	if (sig->sh_old[evsignal] == NULL) {
		event_warn("malloc");
		return (-1);
	}

	/* save previous handler and setup new handler */
#ifdef HAVE_SIGACTION
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = handler;
	sa.sa_flags |= SA_RESTART;    //由此信号中断的系统调用会自动重启
	sigfillset(&sa.sa_mask);   //初始化信号集，使其包含所有信号

	if (sigaction(evsignal, &sa, sig->sh_old[evsignal]) == -1) {   //设置新的信号处理方式
		event_warn("sigaction");
		free(sig->sh_old[evsignal]);
		return (-1);
	}
#else
	if ((sh = signal(evsignal, handler)) == SIG_ERR) {
		event_warn("signal");
		free(sig->sh_old[evsignal]);
		return (-1);
	}
	*sig->sh_old[evsignal] = sh;
#endif

	return (0);
}

int
evsignal_add(struct event *ev)
{
	int evsignal;
	struct event_base *base = ev->ev_base;
	struct evsignal_info *sig = &ev->ev_base->sig;

	if (ev->ev_events & (EV_READ|EV_WRITE))    
		event_errx(1, "%s: EV_SIGNAL incompatible use", __func__);
	evsignal = EVENT_SIGNAL(ev);   //获取描述符宏
	assert(evsignal >= 0 && evsignal < NSIG);
	if (TAILQ_EMPTY(&sig->evsigevents[evsignal])) {   //如果该信号的事件链表为空
		event_debug(("%s: %p: changing signal handler", __func__, ev));
		if (_evsignal_set_handler(
			    base, evsignal, evsignal_handler) == -1)
			return (-1);

		/* catch signals if they happen quickly */
		evsignal_base = base;

		if (!sig->ev_signal_added) {
			if (event_add(&sig->ev_signal, NULL))
				return (-1);
			sig->ev_signal_added = 1;
		}
	}

	/* multiple events may listen to the same signal */
	TAILQ_INSERT_TAIL(&sig->evsigevents[evsignal], ev, ev_signal_next);

	return (0);
}

int
_evsignal_restore_handler(struct event_base *base, int evsignal)
{
	int ret = 0;
	struct evsignal_info *sig = &base->sig;
#ifdef HAVE_SIGACTION
	struct sigaction *sh;    //信号操作结构体，内部封装了一个sig_handler指针，和信号标记
#else
	ev_sighandler_t *sh;
#endif

	/* restore previous handler */
	sh = sig->sh_old[evsignal];     //下标i，因为_evsignal_set_handler函数中sh_old[evsignal]保存了之前的handle，现在取出来即可
	sig->sh_old[evsignal] = NULL;     //取出之后赋空
#ifdef HAVE_SIGACTION
	if (sigaction(evsignal, sh, NULL) == -1) {   //sigaction(SIGINT, &act, &oldact);用新的处理机制代替旧的，act中的flag控制执行具体情况
		event_warn("sigaction");
		ret = -1;
	}
#else
	if (signal(evsignal, *sh) == SIG_ERR) {
		event_warn("signal");
		ret = -1;
	}
#endif
	free(sh);

	return ret;
}

int
evsignal_del(struct event *ev)
{
	struct event_base *base = ev->ev_base;
	struct evsignal_info *sig = &base->sig;
	int evsignal = EVENT_SIGNAL(ev);   //取得事件fd

	assert(evsignal >= 0 && evsignal < NSIG);

	/* multiple events may listen to the same signal */
	TAILQ_REMOVE(&sig->evsigevents[evsignal], ev, ev_signal_next);  //ev_signal_next ?? 为什么ev_signal_next可以直接调用

	if (!TAILQ_EMPTY(&sig->evsigevents[evsignal]))
		return (0);

	event_debug(("%s: %p: restoring signal handler", __func__, ev));

	return (_evsignal_restore_handler(ev->ev_base, EVENT_SIGNAL(ev)));  //restore means 恢复
}

//记录信号的发生次数，并通知event_base有信号触发，需要处理
static void
evsignal_handler(int sig)
{
	int save_errno = errno; //采用栈上变量保存，不覆盖原来错误代码

	if (evsignal_base == NULL) {  //如果未设置
		event_warn(
			"%s: received signal %d, but have no base configured",
			__func__, sig);
		return;
	}

	//记录信号sig的触发次数，并设置event触发标记
	evsignal_base->sig.evsigcaught[sig]++;
	evsignal_base->sig.evsignal_caught = 1;

#ifndef HAVE_SIGACTION
	signal(sig, evsignal_handler);    //重新注册信号
#endif

	//向socket写一个字节数据，触发event_base的I/O事件，从而通知有信号触发，需要处理
	/* Wake up our notification mechanism */
	send(evsignal_base->sig.ev_signal_pair[0], "a", 1, 0);
	errno = save_errno;   //错误代码
}

void
evsignal_process(struct event_base *base)  //处理信号事件
{
	struct evsignal_info *sig = &base->sig;
	struct event *ev, *next_ev;
	sig_atomic_t ncalls;   //信号类型
	int i;
	
	base->sig.evsignal_caught = 0;   //信号是否已发生的标记  
	for (i = 1; i < NSIG; ++i) {  //最大也不可能超过NSIG
		ncalls = sig->evsigcaught[i];
		if (ncalls == 0)  //event_init()是用memset清零过，所以要判断这个情况
			continue;
		sig->evsigcaught[i] -= ncalls; //取完清空

		for (ev = TAILQ_FIRST(&sig->evsigevents[i]);
		    ev != NULL; ev = next_ev) {
			next_ev = TAILQ_NEXT(ev, ev_signal_next);
			if (!(ev->ev_events & EV_PERSIST))   //如果不是持续事件，这个sianal事件这次发生了就可以干掉了
				event_del(ev);
			event_active(ev, EV_SIGNAL, ncalls); //加入到活跃事件链表，等候发落
		}
	}
}

void
evsignal_dealloc(struct event_base *base)
{
	int i = 0;
	if (base->sig.ev_signal_added) {   //如果信号事件注册过了
		event_del(&base->sig.ev_signal);   //析构它，该函数内部也会从响应队列中删除该事件
		base->sig.ev_signal_added = 0;     //清空已注册标记
	}								
	for (i = 0; i < NSIG; ++i) { //NSIG，/usr/include/signal.h里面有#include <bits/signum.h> /usr/include/bits/signum.h
		if (i < base->sig.sh_old_max && base->sig.sh_old[i] != NULL)//NSIG值为65，Biggest signal number + 1	
			_evsignal_restore_handler(base, i);   //先把旧的处理机制存下来
	}

	EVUTIL_CLOSESOCKET(base->sig.ev_signal_pair[0]);
	base->sig.ev_signal_pair[0] = -1;
	EVUTIL_CLOSESOCKET(base->sig.ev_signal_pair[1]);
	base->sig.ev_signal_pair[1] = -1;
	base->sig.sh_old_max = 0;

	/* per index frees are handled in evsignal_del() */
	free(base->sig.sh_old);
}
