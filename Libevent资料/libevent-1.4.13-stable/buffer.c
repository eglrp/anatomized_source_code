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
	
	buffer = calloc(1, sizeof(struct evbuffer));  //¶¯Ì¬·ÖÅäÒ»¸öevbuffer

	return (buffer);
}

void
evbuffer_free(struct evbuffer *buffer)
{
	if (buffer->orig_buffer != NULL)    //ÏÈÅÐ¶Ïorig_bufferÊÇ·ñÐèÒªÊÍ·Å£¬·ÀÖ¹ÄÚ´æÐ¹Â©
		free(buffer->orig_buffer);
	free(buffer);
}

/* 
 * This is a destructive add.  The data from one buffer moves into
 * the other buffer.
 */

#define SWAP(x,y) do { \        //Õâ¸öÊÇ´«ËµÖÐµÄµ¥Ïòswap???
	(x)->buffer = (y)->buffer; \
	(x)->orig_buffer = (y)->orig_buffer; \
	(x)->misalign = (y)->misalign; \
	(x)->totallen = (y)->totallen; \
	(x)->off = (y)->off; \
} while (0)

//ÒÆ¶¯Êý¾Ý´ÓÒ»¸öevbufferµ½ÁíÒ»¸öevbuffer
int
evbuffer_add_buffer(struct evbuffer *outbuf, struct evbuffer *inbuf)
{
	int res;

	/* Short cut for better performance */
	if (outbuf->off == 0) {      //Èç¹ûÊä³öoutbufÎÞÓÐÐ§Êý¾Ý£¬Ö±½Ó½»»»£¬ÎÞÊý¾ÝoutbufºÍÓÐÊý¾Ýinbuf½»»»»áÇå³ýinbufÖÐµÄÊý¾Ý
		struct evbuffer tmp;
		size_t oldoff = inbuf->off;

		/* Swap them directly */
		SWAP(&tmp, outbuf);    //ÕâÀï¾ÍÓÃÁËÉÏÃæÄÇ¸öµ¥Ïòswap£¬ÓÃÁË3´Î
		SWAP(outbuf, inbuf);
		SWAP(inbuf, &tmp);

		/* 
		 * Optimization comes with a price; we need to notify the
		 * buffer if necessary of the changes. oldoff is the amount
		 * of data that we transfered from inbuf to outbuf
		 */
		if (inbuf->off != oldoff && inbuf->cb != NULL)     //Èç¹ûinbuf->off!=oldoffËµÃ÷½»»»³É¹¦£¬ÈôÉèÖÃ»Øµ÷¾Íµ÷ÓÃ
			(*inbuf->cb)(inbuf, oldoff, inbuf->off, inbuf->cbarg);
		if (oldoff && outbuf->cb != NULL)   //Èç¹ûÀÏµÄoldoffÓÐ»õ£¬ÇÒÊä³öoutbufÉèÖÃ¾Íµ÷ÓÃ
			(*outbuf->cb)(outbuf, 0, oldoff, outbuf->cbarg);
		
		return (0);
	}

	res = evbuffer_add(outbuf, inbuf->buffer, inbuf->off);   //½«inµÄevbuffer×·¼Óµ½outbufÖÐ£¬ÕâÀï²»ÍêÃÀ£¬Èç¹ûinbuf->offÎª0£¬¾Í²»ÓÃµ÷ÓÃ
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
	evbuffer_expand(buf, 64);    //64²»ÊÇÒªÖ±½ÓÈ¥À©Õ¹64×Ö½Ú£¬¶øÊÇÓÃ64×÷Îª»ù×¼È¥ºâÁ¿ÓÐÃ»ÓÐfree¿Õ¼ä
	for (;;) {                                  //Èç¹ûÁ¬64×Ö½Ú¶¼²»¹»£¬²Å½øÐÐÏàÓ¦µÄÀ©Õ¹
		size_t used = buf->misalign + buf->off;   
		buffer = (char *)buf->buffer + buf->off;
		assert(buf->totallen >= used);
		space = buf->totallen - used;    //¿ÕÏÐ¿Õ¼ä

#ifndef va_copy
#define	va_copy(dst, src)	memcpy(&(dst), &(src), sizeof(va_list))   //va_list¿½±´
#endif
		va_copy(aq, ap);

		sz = evutil_vsnprintf(buffer, space, fmt, aq);   //½»¸ø¸Ãº¯ÊýÊµÏÖ

		va_end(aq);

		if (sz < 0)    //Ê§°Ü·µ»Ø
			return (-1);
		if ((size_t)sz < space) {   //·µ»Ø´óÐ¡Ð¡ÓÚspace
			buf->off += sz;       //¸üÐÂÆ«ÒÆ
			if (buf->cb != NULL)    //Èç¹ûÉèÖÃÁË£¬µ÷ÓÃ
				(*buf->cb)(buf, oldoff, buf->off, buf->cbarg);
			return (sz);
		}
		if (evbuffer_expand(buf, sz + 1) == -1)    //È·±£×Ö·û´®ºÍ\0¶¼±»Ð´ÈëÁËbufferµÄÓÐÐ§µØÖ·£¬·ÀÖ¹\0Ð´ÈëÎ»ÖÃÔ½½ç
			return (-1);

	}
	/* NOTREACHED */
}

//Ìí¼ÓÒ»¸ö¸ñÊ½»¯µÄ×Ö·û´®µ½evbufferÎ²²¿
int
evbuffer_add_printf(struct evbuffer *buf, const char *fmt, ...)  
{
	int res = -1;
	va_list ap;

	va_start(ap, fmt);
	res = evbuffer_add_vprintf(buf, fmt, ap);   //µ÷ÓÃ¸Ãº¯ÊýÊµÏÖ
	va_end(ap);

	return (res);
}

/* Reads data from an event buffer and drains the bytes read */
//¶ÁÈ¡evbuffer»º³åÇøµÄÊý¾Ýµ½dataÖÐ£¬³¤¶ÈÎªdatlen
int
evbuffer_remove(struct evbuffer *buf, void *data, size_t datlen)
{
	size_t nread = datlen;
	if (nread >= buf->off)    //Èç¹û´ó£¬¶ÁÒÑÓÐµÄ
		nread = buf->off;

	memcpy(data, buf->buffer, nread); 
	evbuffer_drain(buf, nread);    //Í¬Ñùµ÷ÓÃÏûºÄº¯Êý£¬Çå³ýÒÑ¶ÁÊý¾Ý
	
	return (nread);
}

/*
 * Reads a line terminated by either '\r\n', '\n\r' or '\r' or '\n'.
 * The returned buffer needs to be freed by the called.
 */
//¶ÁÈ¡ÒÔ\r»ò\n½áÎ²µÄÒ»ÐÐÊý¾Ý
char *
evbuffer_readline(struct evbuffer *buffer)
{
	u_char *data = EVBUFFER_DATA(buffer); //(x)->buffer
	size_t len = EVBUFFER_LENGTH(buffer); //(x)->off,²»ÖªµÀÎªÊ²Ã´Ö»ÓÐ´Ë´¦ÓÃÁËÕâÁ½¸öºê£¬±¾ÎÄ¼þÆäËü¿ÉÓÃµÄµØ·½¶¼Ã»ÓÐÓÃ
	char *line;
	unsigned int i;

	for (i = 0; i < len; i++) {
		if (data[i] == '\r' || data[i] == '\n')
			break;
	}

	if (i == len)   //Ã»ÕÒµ½\r»ò\nÖ±½Ó·µ»ØNULL
		return (NULL);

	if ((line = malloc(i + 1)) == NULL) {
		fprintf(stderr, "%s: out of memory\n", __func__);
		return (NULL);
	}

	memcpy(line, data, i);    //´Óbuffer¿½±´µ½line
	line[i] = '\0';

	/*
	 * Some protocols terminate a line with '\r\n', so check for
	 * that, too.
	 */
	if ( i < len - 1 ) {      //Èç¹ûÕÒµ½µÄÐ¡ÓÚlen-1£¬ÓÐÐ©Ð­Òé¿ÉÄÜ´æÔÚ\r\nÇé¿ö£¬Ò²Òª´¦ÀíÒ»ÏÂ
		char fch = data[i], sch = data[i+1];

		/* Drain one more character if needed */
		if ( (sch == '\r' || sch == '\n') && sch != fch )
			i += 1;    //Èç¹ûÊÇ\r\n£¬ÔÙÍùºó×ßÒ»¸ö
	}

	evbuffer_drain(buffer, i + 1);    //×¢Òâ²ÎÊýi=1£¬ÒòÎªdrainº¯ÊýÖÐÓÃbuffer+=lenµÃµ½ÐÂbufferµÄÎ»ÖÃ

	return (line);   //·µ»ØÊý¾Ý£¬×¢ÒâÕâ¸ölineÊÇÓÃmallocÉêÇëµÄ£¬ÓÃ»§ÐèÒªÊÖ¶¯ÊÍ·Å!!!
}

/* Adds data to an event buffer */
//´Ëº¯ÊýÊÇevbufferµÄµ÷Õûº¯Êý£¬½«evbufferµÄbuffer¶ÎÇ°ÒÆµ½ÆðÊ¼Î»ÖÃorig_buffer
static void
evbuffer_align(struct evbuffer *buf)  
{
	memmove(buf->orig_buffer, buf->buffer, buf->off);    //Ö±½Óµ÷ÓÃmemmove
	buf->buffer = buf->orig_buffer;
	buf->misalign = 0;            //¸üÐÂmisalign
}

/* Expands the available space in the event buffer to at least datlen */

int
evbuffer_expand(struct evbuffer *buf, size_t datlen)
{
	size_t need = buf->misalign + buf->off + datlen;   //ÏÈ¼ÆËã×ÜÐèÇó

	/* If we can fit all the data, then we don't have to do anything */
	if (buf->totallen >= need)    //Èç¹û¹»£¬²»À©Õ¹£¬Ö±½Ó·µ»Ø
		return (0);

	/*
	 * If the misalignment fulfills our data needs, we just force an
	 * alignment to happen.  Afterwards, we have enough space.
	 */
	if (buf->misalign >= datlen) {  //Èç¹ûÇ°Ãæ0->misalign¿Õ¼ä×ã¹»datlen£¬½«evbufferµ÷Õû£¬½«buffer¶ÎÄÚÊý¾ÝÇ°ÒÆ
		evbuffer_align(buf);
	} else {                                     //²»¹»¾ÍÁíÍâ¿ª±ÙÒ»¶Î¿Õ¼ä
		void *newbuf;
		size_t length = buf->totallen;     //×Ü´óÐ¡

		if (length < 256)       //×îÉÙÒ»´ÎÐÔ¿ª±Ù256×Ö½Ú
			length = 256;
		while (length < need)     //Èç¹û×Ü´óÐ¡¶¼Ð¡ÓÚÐèÇó£¬¾ÍÖ±½Ó¿ª±Ù2±¶£¬ÀàËÆÓÚSTLÖÐvectorµÄÄÚ´æ²ßÂÔ
			length <<= 1;

		if (buf->orig_buffer != buf->buffer)    //ÁíÍâ¿ª±ÙÇ°ÏÈ°ÑbufferÇ°ÒÆµ½ÆðÊ¼Î»ÖÃ
			evbuffer_align(buf);
		if ((newbuf = realloc(buf->buffer, length)) == NULL)    //¿ª±ÙÁíÍâµÄÄÚ´æ¿Õ¼ä
			return (-1);

		buf->orig_buffer = buf->buffer = newbuf;   //½«Á½¸öbufferÖ¸Õë¸üÐÂµ½ÐÂµÄÄÚ´æµØÖ·
		buf->totallen = length;    //¸üÐÂ×Ü´óÐ¡
	}

	return (0);
}

//½«data×·¼Óµ½bufferÖÐ   
//Õâ¸öº¯ÊýÓÐ¸öbug£¬ËüÃ»ÓÐÅÐ¶ÏdatlenµÄ´óÐ¡£¬Èç¹ûdatlen=0£¬ÄÇÃ´Ö±½Ó·µ»Ø¾ÍÐÐÁË
int
evbuffer_add(struct evbuffer *buf, const void *data, size_t datlen)
{
	size_t need = buf->misalign + buf->off + datlen;  //Ê×ÏÈÅÐ¶Ï»º³åÇø´óÐ¡£¬buf->offÊÇÒÑ¾­´æ·ÅµÄÓÐÐ§Êý¾Ý³¤¶È
	size_t oldoff = buf->off;

	if (buf->totallen < need) {    //Èç¹û×Ü´óÐ¡ÈÝÄÉ²»ÏÂdatalen´óÐ¡£¬À©³äÈÝÁ¿
		if (evbuffer_expand(buf, datlen) == -1)
			return (-1);
	}

	memcpy(buf->buffer + buf->off, data, datlen);    //½«data×·¼Óµ½buffer+offºó
	buf->off += datlen;    //off³¤¶ÈÔö¼Ó

	if (datlen && buf->cb != NULL)     //Èç¹û³¤¶È´óÓÚ0ÇÒÉèÖÃÁË»Øµ÷º¯Êý£¬Ôòµ÷ÓÃ»Øµ÷º¯Êý
		(*buf->cb)(buf, oldoff, buf->off, buf->cbarg);  //Ã»ÉèÖÃ¾ÍÊ²Ã´Ò²²»×ö

	return (0);
}

//¸Ãº¯ÊýÓÐÁ½¸ö×÷ÓÃ£¬ÆäÒ»ÊÇÍêÈ«Çå³ýÓÐÐ§»º³åÇø,lenÉèÖÃÎª>=off¼´¿É£¬Ïàµ±ÓÚÓÐÐ§»º³åÇøÈ«²¿ÏûºÄµô
//Æä¶þÊÇÏûºÄÒ»²¿·Ö»º³åÇø£¬»º³åÇøÇ°¶ÎÏûºÄµô£¬Ïàµ±ÓÚÏòºóÒÆ¶¯£¬misalignÔö´ó,off¼õÐ¡
void
evbuffer_drain(struct evbuffer *buf, size_t len)
{
	size_t oldoff = buf->off;

	if (len >= buf->off) {  //Èç¹ûÏûºÄµÄlenµÄ³¤¶È´óÓÚµÈÓÚ»º³åÇøoffµÄ³¤¶È£¬Çå¿Õ»º³åÇø
		buf->off = 0;
		buf->buffer = buf->orig_buffer;
		buf->misalign = 0;
		goto done;     //Õâ¸ögotoÎÒÔÝÊ±Ã»¿´³öÀ´ÓÐÊ²Ã´ÓÃ£¬ÎÒÈÏÎª¿ÉÒÔÓÃif-elseÌæ»»
	}

	//Èç¹ûÏûºÄµÄlen²»´óÓÚoff£¬ÓÐÐ§»º³åÇøÏòÇ°ÒÆ¶¯£¬Ç°ÃæµÄÒ»²¿·Ö±»¶ÁÈ¡£¬misalignÔö´ó£¬off¼õÐ¡¡
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

#define EVBUFFER_MAX_READ	4096       //evbufferµÄ×î´ó¿É¶Á×Ö½ÚÊý

//ÖµµÃ×¢Òâevbuffer_read²¢²»ÊÇÏà¶Ôevbuffer_write£¬ËüÊÇ´ÓÃèÊö·ûÍùbuffer¶ÁÊý¾Ý£¬evbuffer_remove²ÅÊÇ¶ÁÈ¡evbufferÊý¾Ý
//´ÓfdÉÏÍùbuffer¶ÁÈ¡Êý¾Ý£¬Èç¹û»º³åÇø²»¹»£¬Ôòexpand
int
evbuffer_read(struct evbuffer *buf, int fd, int howmuch)
{
	u_char *p;
	size_t oldoff = buf->off;
	int n = EVBUFFER_MAX_READ;    //×î´ó×Ö½ÚÊý

#if defined(FIONREAD)  //FIONREAD·µ»Ø»º³åÇøÓÐ¶àÉÙ¸ö×Ö½Ú
#ifdef WIN32
	long lng = n;
	if (ioctlsocket(fd, FIONREAD, &lng) == -1 || (n=lng) <= 0) {
#else
	if (ioctl(fd, FIONREAD, &n) == -1 || n <= 0) {   //·µ»ØfdµÄ¿É¶Á×Ö½ÚÊý°ÉÊ§°Ü£¬»òÕßn=0
#endif
		n = EVBUFFER_MAX_READ;    //Èç¹û»ñÈ¡»º³åÇø×Ö½ÚÊ§°Ü»òn<=0,nÈ¡×î´ó£¬ÒòÎªÒª¾¡¿ÉÄÜµÄ×î´ó¶ÁÈ¡
	} else if (n > EVBUFFER_MAX_READ && n > howmuch) {
		/*
		 * It's possible that a lot of data is available for
		 * reading.  We do not want to exhaust resources
		 * before the reader has a chance to do something
		 * about it.  If the reader does not tell us how much
		 * data we should read, we artifically limit it.  //ÈËÎªÏÞÖÆ
		 */
		if ((size_t)n > buf->totallen << 2)    //Èç¹û´óÓÚ×Ü´óÐ¡µÄ4±¶£¬n¸³ÖµÎªËü
			n = buf->totallen << 2;
		if (n < EVBUFFER_MAX_READ)
			n = EVBUFFER_MAX_READ;
	}
#endif	// ¾¡¿ÉÄÜ¶àµÄ¶ÁÈ¡
	if (howmuch < 0 || howmuch > n)    //Èç¹ûÉèÖÃÒª¶ÁµÄ×Ö½ÚÊýÐ¡ÓÚ0»ò´óÓÚn£¬¾ÍÈÃËü¶ÁÄ¬ÈÏµÄ4096¸ö×Ö½Ú
		howmuch = n;     

	/* If we don't have FIONREAD, we might waste some space here */
	if (evbuffer_expand(buf, howmuch) == -1)  //Èç¹û²»¹»4096¾ÍÀ©³ä
		return (-1);

	/* We can append new data at this point */
	p = buf->buffer + buf->off;    //pÖ¸Ïòfree space

#ifndef WIN32
	n = read(fd, p, howmuch);   //ÏµÍ³µ÷ÓÃread
#else
	n = recv(fd, p, howmuch, 0);
#endif
	if (n == -1)      //Ê§°Ü-1·µ»Ø
		return (-1);
	if (n == 0)              //Èç¹ûÎª0·µ»Ø0£¬¶Áµ½0¸ö×Ö½Ú
		return (0);

	buf->off += n;      //¶Áµ½Êý¾Ýºó£¬¸üÐÂoff

	/* Tell someone about changes in this buffer */
	if (buf->off != oldoff && buf->cb != NULL)   //Èç¹û¶Áµ½Êý¾ÝÇÒÉèÖÃÁË»Øµ÷¾Íµ÷ÓÃ 
		(*buf->cb)(buf, oldoff, buf->off, buf->cbarg);

	return (n);   //·µ»Ø¶Áµ½µÄ×Ö½ÚÊý
}

//°ÑevbufferµÄÊý¾ÝÐ´Èëµ½ÎÄ¼þÃèÊö·ûfdÉÏ£¬Èç¹ûÐ´Èë³É¹¦£¬µ÷ÓÃevbuffer_drainÉ¾³ýÒÑÐ´Êý¾Ý
int
evbuffer_write(struct evbuffer *buffer, int fd)  
{
	int n;

#ifndef WIN32
	n = write(fd, buffer->buffer, buffer->off);  //Ö±½ÓÐ´
#else
	n = send(fd, buffer->buffer, buffer->off, 0);
#endif
	if (n == -1)
		return (-1);
	if (n == 0)
		return (0);
	evbuffer_drain(buffer, n);    //Ð´Èëµ½fdºó£¬É¾³ýbufferÖÐµÄÊý¾Ý£¬Ïàµ±ÓÚÏûºÄµôÁË

	return (n);
}

//²éÕÒ×Ö·û´®what
u_char *
evbuffer_find(struct evbuffer *buffer, const u_char *what, size_t len)
{
	u_char *search = buffer->buffer, *end = search + buffer->off;
	u_char *p;

	while (search < end &&                                        
	    (p = memchr(search, *what, end - search)) != NULL) {   //ÔÚsearchºÍendµÄ·¶Î§ÄÚ²éÕÒ*what£¬×¢ÒâÕâÊµÔÚÆ¥ÅäµÚÒ»¸ö×Ö·û
		if (p + len > end)     //Î´ÕÒµ½
			break;
		if (memcmp(p, what, len) == 0)   //Æ¥Åäµ½µÚÒ»¸ö×Ö·ûºó±È½Ïlen³¤¶ÈµÄÄÚ´æÏàµÈÔò·µ»Ø£¬ÕâÁ½¸öº¯Êý×éºÏÒ²ËãÊÇÒ»ÖÖ×Ö·û´®Æ¥ÅäËã·¨
			return (p);                              //»°ËµÕâ¸ö²éÕÒËã·¨¿ÉÒÔÓÃKMP
		search = p + 1;     //²»ÏàµÈºóÒÆÒ»¸ö×Ö·û£¬¼ÌÐø²éÕÒ
	}

	return (NULL);
}

//»º³åÇø±ä»¯Ê±ÉèÖÃµÄ»Øµ÷»Øµôº¯Êý
void evbuffer_setcb(struct evbuffer *buffer,
    void (*cb)(struct evbuffer *, size_t, size_t, void *),
    void *cbarg)
{
	buffer->cb = cb;
	buffer->cbarg = cbarg;
}
