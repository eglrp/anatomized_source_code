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
#ifndef _EVENT_INTERNAL_H_
#define _EVENT_INTERNAL_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "config.h"
#include "min_heap.h"
#include "evsignal.h"

struct eventop {
	const char *name;
	void *(*init)(struct event_base *);   //��ʼ��
	int (*add)(void *, struct event *);    //ע���¼�
	int (*del)(void *, struct event *);     //ɾ���¼�
	int (*dispatch)(struct event_base *, void *, struct timeval *);   //�¼��ַ�
	void (*dealloc)(struct event_base *, void *);    //ע�����ͷ���Դ
	/* set if we need to reinitialize the event base */
	int need_reinit;    //�����Ƿ����³�ʼ��
};

struct event_base {
	const struct eventop *evsel;  //��������ϵͳ����ǰ����I/O���Ƶ����֣�����eventops
	void *evbase;     //����init���ص�I/O��·���õ�op����epollop
	int event_count;		/* counts number of total events */  //�¼�����
	int event_count_active;	/* counts number of active events */   //��Ծ�¼���Ŀ

	int event_gotterm;		/* Set to terminate loop */  
	int event_break;		/* Set to terminate loop immediately */

	/* active event management */
	struct event_list **activequeues; //activequeues[priority]��һ�����������е�ÿ����㶼�����ȼ�Ϊ
	 							  //priority�ľ����¼�event
	int nactivequeues;          //��Ծ�������Ŀ

	/* signal handling info */
	struct evsignal_info sig;      //ר�Ź����źŵĽṹ��

	struct event_list eventqueue;   //��������������ע���¼�event��ָ��
	struct timeval event_tv;       //����ʱ�����

	struct min_heap timeheap;   //��������ʱʱ���С����

	struct timeval tv_cache;   //����ʱ�����
};

/* Internal use only: Functions that might be missing from <sys/queue.h> */
#ifndef HAVE_TAILQFOREACH
#define	TAILQ_FIRST(head)		((head)->tqh_first)
#define	TAILQ_END(head)			NULL
#define	TAILQ_NEXT(elm, field)		((elm)->field.tqe_next)
#define TAILQ_FOREACH(var, head, field)					\
	for((var) = TAILQ_FIRST(head);					\
	    (var) != TAILQ_END(head);					\
	    (var) = TAILQ_NEXT(var, field))
#define	TAILQ_INSERT_BEFORE(listelm, elm, field) do {			\
	(elm)->field.tqe_prev = (listelm)->field.tqe_prev;		\
	(elm)->field.tqe_next = (listelm);				\
	*(listelm)->field.tqe_prev = (elm);				\
	(listelm)->field.tqe_prev = &(elm)->field.tqe_next;		\
} while (0)
#endif /* TAILQ_FOREACH */

int _evsignal_set_handler(struct event_base *base, int evsignal,
			  void (*fn)(int));
int _evsignal_restore_handler(struct event_base *base, int evsignal);

/* defined in evutil.c */
const char *evutil_getenv(const char *varname);

#ifdef __cplusplus
}
#endif

#endif /* _EVENT_INTERNAL_H_ */
