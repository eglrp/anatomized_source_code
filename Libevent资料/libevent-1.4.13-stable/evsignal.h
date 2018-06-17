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
#ifndef _EVSIGNAL_H_
#define _EVSIGNAL_H_

typedef void (*ev_sighandler_t)(int);

//event_info�ĳ�ʼ������������socket pair������ev_signal�¼�(����û��ע�ᣬ���ǵȵ����ź�ע��ʱ�ż�鲢ע��
//�������б�����㣬��ʼ���źŵ�ע���¼�����ָ���
struct evsignal_info {
	struct event ev_signal;    //Ϊsocket pair�Ķ�socket��event_baseע����¼�ʱʹ�õ�event�ṹ��
	int ev_signal_pair[2];      //socket pair��
	int ev_signal_added;       //��¼ev_signal�¼��Ƿ��Ѿ�ע����
	volatile sig_atomic_t evsignal_caught;//�Ƿ����źŷ����ı�ǣ�volatile���ͣ���Ϊ������������̱߳��޸�
	struct event_list evsigevents[NSIG];   //���飬evsigevent[signo]��ʾע�ᵽ�ź�signo���¼�����
	sig_atomic_t evsigcaught[NSIG]; //�����¼ÿ���źŴ����Ĵ�����evsigcaught[signo]��ʾsigno�źű���������
#ifdef HAVE_SIGACTION
	struct sigaction **sh_old;//��¼��ԭ��signal�������ĺ���ָ�룬���ź�signoע���event�����ʱ,
#else					   //��Ҫ���������䴦����
	ev_sighandler_t **sh_old;
#endif
	int sh_old_max;    //�ɵ��źŴ��������ֵ���
};
int evsignal_init(struct event_base *);
void evsignal_process(struct event_base *);
int evsignal_add(struct event *);
int evsignal_del(struct event *);
void evsignal_dealloc(struct event_base *);

#endif /* _EVSIGNAL_H_ */
