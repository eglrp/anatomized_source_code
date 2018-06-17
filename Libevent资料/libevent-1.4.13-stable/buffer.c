/*
 * Copyright (c) 2002, 2003 Niels Provos <provos@citi.umich.edu>
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
#include <winsock2.h>
#include <windows.h>
#endif

#ifdef HAVE_VASPRINTF
/* If we have vasprintf, we need to define this before we include stdio.h. */
#define _GNU_SOURCE
#endif

#include <sys/types.h>

#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif

#ifdef HAVE_SYS_IOCTL_H
#include <sys/ioctl.h>
#endif

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_STDARG_H
#include <stdarg.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "event.h"
#include "config.h"
#include "evutil.h"

struct evbuffer *
evbuffer_new(void)
{
	struct evbuffer *buffer;
	
	buffer = calloc(1, sizeof(struct evbuffer));  //��̬����һ��evbuffer

	return (buffer);
}

void
evbuffer_free(struct evbuffer *buffer)
{
	if (buffer->orig_buffer != NULL)    //���ж�orig_buffer�Ƿ���Ҫ�ͷţ���ֹ�ڴ�й©
		free(buffer->orig_buffer);
	free(buffer);
}

/* 
 * This is a destructive add.  The data from one buffer moves into
 * the other buffer.
 */

#define SWAP(x,y) do { \        //����Ǵ�˵�еĵ���swap???
	(x)->buffer = (y)->buffer; \
	(x)->orig_buffer = (y)->orig_buffer; \
	(x)->misalign = (y)->misalign; \
	(x)->totallen = (y)->totallen; \
	(x)->off = (y)->off; \
} while (0)

//�ƶ����ݴ�һ��evbuffer����һ��evbuffer
int
evbuffer_add_buffer(struct evbuffer *outbuf, struct evbuffer *inbuf)
{
	int res;

	/* Short cut for better performance */
	if (outbuf->off == 0) {      //������outbuf����Ч���ݣ�ֱ�ӽ�����������outbuf��������inbuf���������inbuf�е�����
		struct evbuffer tmp;
		size_t oldoff = inbuf->off;

		/* Swap them directly */
		SWAP(&tmp, outbuf);    //��������������Ǹ�����swap������3��
		SWAP(outbuf, inbuf);
		SWAP(inbuf, &tmp);

		/* 
		 * Optimization comes with a price; we need to notify the
		 * buffer if necessary of the changes. oldoff is the amount
		 * of data that we transfered from inbuf to outbuf
		 */
		if (inbuf->off != oldoff && inbuf->cb != NULL)     //���inbuf->off!=oldoff˵�������ɹ��������ûص��͵���
			(*inbuf->cb)(inbuf, oldoff, inbuf->off, inbuf->cbarg);
		if (oldoff && outbuf->cb != NULL)   //����ϵ�oldoff�л��������outbuf���þ͵���
			(*outbuf->cb)(outbuf, 0, oldoff, outbuf->cbarg);
		
		return (0);
	}

	res = evbuffer_add(outbuf, inbuf->buffer, inbuf->off);   //��in��evbuffer׷�ӵ�outbuf�У����ﲻ���������inbuf->offΪ0���Ͳ��õ���
	if (res == 0) {
		/* We drain the input buffer on success */
		evbuffer_drain(inbuf, inbuf->off);
	}

	return (res);
}

int
evbuffer_add_vprintf(struct evbuffer *buf, const char *fmt, va_list ap)
{
	char *buffer;
	size_t space;
	size_t oldoff = buf->off;  
	int sz;
	va_list aq;

	/* make sure that at least some space is available */
	evbuffer_expand(buf, 64);    //64����Ҫֱ��ȥ��չ64�ֽڣ�������64��Ϊ��׼ȥ������û��free�ռ�
	for (;;) {                                  //�����64�ֽڶ��������Ž�����Ӧ����չ
		size_t used = buf->misalign + buf->off;   
		buffer = (char *)buf->buffer + buf->off;
		assert(buf->totallen >= used);
		space = buf->totallen - used;    //���пռ�

#ifndef va_copy
#define	va_copy(dst, src)	memcpy(&(dst), &(src), sizeof(va_list))   //va_list����
#endif
		va_copy(aq, ap);

		sz = evutil_vsnprintf(buffer, space, fmt, aq);   //�����ú���ʵ��

		va_end(aq);

		if (sz < 0)    //ʧ�ܷ���
			return (-1);
		if ((size_t)sz < space) {   //���ش�СС��space
			buf->off += sz;       //����ƫ��
			if (buf->cb != NULL)    //��������ˣ�����
				(*buf->cb)(buf, oldoff, buf->off, buf->cbarg);
			return (sz);
		}
		if (evbuffer_expand(buf, sz + 1) == -1)    //ȷ���ַ�����\0����д����buffer����Ч��ַ����ֹ\0д��λ��Խ��
			return (-1);

	}
	/* NOTREACHED */
}

//���һ����ʽ�����ַ�����evbufferβ��
int
evbuffer_add_printf(struct evbuffer *buf, const char *fmt, ...)  
{
	int res = -1;
	va_list ap;

	va_start(ap, fmt);
	res = evbuffer_add_vprintf(buf, fmt, ap);   //���øú���ʵ��
	va_end(ap);

	return (res);
}

/* Reads data from an event buffer and drains the bytes read */
//��ȡevbuffer�����������ݵ�data�У�����Ϊdatlen
int
evbuffer_remove(struct evbuffer *buf, void *data, size_t datlen)
{
	size_t nread = datlen;
	if (nread >= buf->off)    //����󣬶����е�
		nread = buf->off;

	memcpy(data, buf->buffer, nread); 
	evbuffer_drain(buf, nread);    //ͬ���������ĺ���������Ѷ�����
	
	return (nread);
}

/*
 * Reads a line terminated by either '\r\n', '\n\r' or '\r' or '\n'.
 * The returned buffer needs to be freed by the called.
 */
//��ȡ��\r��\n��β��һ������
char *
evbuffer_readline(struct evbuffer *buffer)
{
	u_char *data = EVBUFFER_DATA(buffer); //(x)->buffer
	size_t len = EVBUFFER_LENGTH(buffer); //(x)->off,��֪��Ϊʲôֻ�д˴������������꣬���ļ��������õĵط���û����
	char *line;
	unsigned int i;

	for (i = 0; i < len; i++) {
		if (data[i] == '\r' || data[i] == '\n')
			break;
	}

	if (i == len)   //û�ҵ�\r��\nֱ�ӷ���NULL
		return (NULL);

	if ((line = malloc(i + 1)) == NULL) {
		fprintf(stderr, "%s: out of memory\n", __func__);
		return (NULL);
	}

	memcpy(line, data, i);    //��buffer������line
	line[i] = '\0';

	/*
	 * Some protocols terminate a line with '\r\n', so check for
	 * that, too.
	 */
	if ( i < len - 1 ) {      //����ҵ���С��len-1����ЩЭ����ܴ���\r\n�����ҲҪ����һ��
		char fch = data[i], sch = data[i+1];

		/* Drain one more character if needed */
		if ( (sch == '\r' || sch == '\n') && sch != fch )
			i += 1;    //�����\r\n����������һ��
	}

	evbuffer_drain(buffer, i + 1);    //ע�����i=1����Ϊdrain��������buffer+=len�õ���buffer��λ��

	return (line);   //�������ݣ�ע�����line����malloc����ģ��û���Ҫ�ֶ��ͷ�!!!
}

/* Adds data to an event buffer */
//�˺�����evbuffer�ĵ�����������evbuffer��buffer��ǰ�Ƶ���ʼλ��orig_buffer
static void
evbuffer_align(struct evbuffer *buf)  
{
	memmove(buf->orig_buffer, buf->buffer, buf->off);    //ֱ�ӵ���memmove
	buf->buffer = buf->orig_buffer;
	buf->misalign = 0;            //����misalign
}

/* Expands the available space in the event buffer to at least datlen */

int
evbuffer_expand(struct evbuffer *buf, size_t datlen)
{
	size_t need = buf->misalign + buf->off + datlen;   //�ȼ���������

	/* If we can fit all the data, then we don't have to do anything */
	if (buf->totallen >= need)    //�����������չ��ֱ�ӷ���
		return (0);

	/*
	 * If the misalignment fulfills our data needs, we just force an
	 * alignment to happen.  Afterwards, we have enough space.
	 */
	if (buf->misalign >= datlen) {  //���ǰ��0->misalign�ռ��㹻datlen����evbuffer��������buffer��������ǰ��
		evbuffer_align(buf);
	} else {                                     //���������⿪��һ�οռ�
		void *newbuf;
		size_t length = buf->totallen;     //�ܴ�С

		if (length < 256)       //����һ���Կ���256�ֽ�
			length = 256;
		while (length < need)     //����ܴ�С��С�����󣬾�ֱ�ӿ���2����������STL��vector���ڴ����
			length <<= 1;

		if (buf->orig_buffer != buf->buffer)    //���⿪��ǰ�Ȱ�bufferǰ�Ƶ���ʼλ��
			evbuffer_align(buf);
		if ((newbuf = realloc(buf->buffer, length)) == NULL)    //����������ڴ�ռ�
			return (-1);

		buf->orig_buffer = buf->buffer = newbuf;   //������bufferָ����µ��µ��ڴ��ַ
		buf->totallen = length;    //�����ܴ�С
	}

	return (0);
}

//��data׷�ӵ�buffer��   
//��������и�bug����û���ж�datlen�Ĵ�С�����datlen=0����ôֱ�ӷ��ؾ�����
int
evbuffer_add(struct evbuffer *buf, const void *data, size_t datlen)
{
	size_t need = buf->misalign + buf->off + datlen;  //�����жϻ�������С��buf->off���Ѿ���ŵ���Ч���ݳ���
	size_t oldoff = buf->off;

	if (buf->totallen < need) {    //����ܴ�С���ɲ���datalen��С����������
		if (evbuffer_expand(buf, datlen) == -1)
			return (-1);
	}

	memcpy(buf->buffer + buf->off, data, datlen);    //��data׷�ӵ�buffer+off��
	buf->off += datlen;    //off��������

	if (datlen && buf->cb != NULL)     //������ȴ���0�������˻ص�����������ûص�����
		(*buf->cb)(buf, oldoff, buf->off, buf->cbarg);  //û���þ�ʲôҲ����

	return (0);
}

//�ú������������ã���һ����ȫ�����Ч������,len����Ϊ>=off���ɣ��൱����Ч������ȫ�����ĵ�
//���������һ���ֻ�������������ǰ�����ĵ����൱������ƶ���misalign����,off��С
void
evbuffer_drain(struct evbuffer *buf, size_t len)
{
	size_t oldoff = buf->off;

	if (len >= buf->off) {  //������ĵ�len�ĳ��ȴ��ڵ��ڻ�����off�ĳ��ȣ���ջ�����
		buf->off = 0;
		buf->buffer = buf->orig_buffer;
		buf->misalign = 0;
		goto done;     //���goto����ʱû��������ʲô�ã�����Ϊ������if-else�滻
	}

	//������ĵ�len������off����Ч��������ǰ�ƶ���ǰ���һ���ֱ���ȡ��misalign����off��С�
	buf->buffer += len;
	buf->misalign += len;

	buf->off -= len;

 done:
	/* Tell someone about changes in this buffer */
	if (buf->off != oldoff && buf->cb != NULL)
		(*buf->cb)(buf, oldoff, buf->off, buf->cbarg);

}

/*
 * Reads data from a file descriptor into a buffer.
 */

#define EVBUFFER_MAX_READ	4096       //evbuffer�����ɶ��ֽ���

//ֵ��ע��evbuffer_read���������evbuffer_write�����Ǵ���������buffer�����ݣ�evbuffer_remove���Ƕ�ȡevbuffer����
//��fd����buffer��ȡ���ݣ������������������expand
int
evbuffer_read(struct evbuffer *buf, int fd, int howmuch)
{
	u_char *p;
	size_t oldoff = buf->off;
	int n = EVBUFFER_MAX_READ;    //����ֽ���

#if defined(FIONREAD)  //FIONREAD���ػ������ж��ٸ��ֽ�
#ifdef WIN32
	long lng = n;
	if (ioctlsocket(fd, FIONREAD, &lng) == -1 || (n=lng) <= 0) {
#else
	if (ioctl(fd, FIONREAD, &n) == -1 || n <= 0) {   //����fd�Ŀɶ��ֽ�����ʧ�ܣ�����n=0
#endif
		n = EVBUFFER_MAX_READ;    //�����ȡ�������ֽ�ʧ�ܻ�n<=0,nȡ�����ΪҪ�����ܵ�����ȡ
	} else if (n > EVBUFFER_MAX_READ && n > howmuch) {
		/*
		 * It's possible that a lot of data is available for
		 * reading.  We do not want to exhaust resources
		 * before the reader has a chance to do something
		 * about it.  If the reader does not tell us how much
		 * data we should read, we artifically limit it.  //��Ϊ����
		 */
		if ((size_t)n > buf->totallen << 2)    //��������ܴ�С��4����n��ֵΪ��
			n = buf->totallen << 2;
		if (n < EVBUFFER_MAX_READ)
			n = EVBUFFER_MAX_READ;
	}
#endif	// �����ܶ�Ķ�ȡ
	if (howmuch < 0 || howmuch > n)    //�������Ҫ�����ֽ���С��0�����n����������Ĭ�ϵ�4096���ֽ�
		howmuch = n;     

	/* If we don't have FIONREAD, we might waste some space here */
	if (evbuffer_expand(buf, howmuch) == -1)  //�������4096������
		return (-1);

	/* We can append new data at this point */
	p = buf->buffer + buf->off;    //pָ��free space

#ifndef WIN32
	n = read(fd, p, howmuch);   //ϵͳ����read
#else
	n = recv(fd, p, howmuch, 0);
#endif
	if (n == -1)      //ʧ��-1����
		return (-1);
	if (n == 0)              //���Ϊ0����0������0���ֽ�
		return (0);

	buf->off += n;      //�������ݺ󣬸���off

	/* Tell someone about changes in this buffer */
	if (buf->off != oldoff && buf->cb != NULL)   //������������������˻ص��͵��� 
		(*buf->cb)(buf, oldoff, buf->off, buf->cbarg);

	return (n);   //���ض������ֽ���
}

//��evbuffer������д�뵽�ļ�������fd�ϣ����д��ɹ�������evbuffer_drainɾ����д����
int
evbuffer_write(struct evbuffer *buffer, int fd)  
{
	int n;

#ifndef WIN32
	n = write(fd, buffer->buffer, buffer->off);  //ֱ��д
#else
	n = send(fd, buffer->buffer, buffer->off, 0);
#endif
	if (n == -1)
		return (-1);
	if (n == 0)
		return (0);
	evbuffer_drain(buffer, n);    //д�뵽fd��ɾ��buffer�е����ݣ��൱�����ĵ���

	return (n);
}

//�����ַ���what
u_char *
evbuffer_find(struct evbuffer *buffer, const u_char *what, size_t len)
{
	u_char *search = buffer->buffer, *end = search + buffer->off;
	u_char *p;

	while (search < end &&                                        
	    (p = memchr(search, *what, end - search)) != NULL) {   //��search��end�ķ�Χ�ڲ���*what��ע����ʵ��ƥ���һ���ַ�
		if (p + len > end)     //δ�ҵ�
			break;
		if (memcmp(p, what, len) == 0)   //ƥ�䵽��һ���ַ���Ƚ�len���ȵ��ڴ�����򷵻أ��������������Ҳ����һ���ַ���ƥ���㷨
			return (p);                              //��˵��������㷨������KMP
		search = p + 1;     //����Ⱥ���һ���ַ�����������
	}

	return (NULL);
}

//�������仯ʱ���õĻص��ص�����
void evbuffer_setcb(struct evbuffer *buffer,
    void (*cb)(struct evbuffer *, size_t, size_t, void *),
    void *cbarg)
{
	buffer->cb = cb;
	buffer->cbarg = cbarg;
}
