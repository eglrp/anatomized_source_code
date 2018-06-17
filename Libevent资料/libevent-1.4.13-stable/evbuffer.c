/*
 * Copyright (c) 2002-2004 Niels Provos <provos@citi.umich.edu>
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

#include <sys/types.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_STDARG_H
#include <stdarg.h>
#endif

#ifdef WIN32
#include <winsock2.h>
#endif

#include "evutil.h"
#include "event.h"

/* prototypes */

void bufferevent_read_pressure_cb(struct evbuffer *, size_t, size_t, void *);

static int
bufferevent_add(struct event *ev, int timeout)
{
	struct timeval tv, *ptv = NULL;

	if (timeout) {
		evutil_timerclear(&tv); //Ê±¼äÇå¿Õ
		tv.tv_sec = timeout;
		ptv = &tv;
	}

	return (event_add(ev, ptv));  //¼ÓÈë·´Ó¦¶ÑÏàÓ¦¶ÓÁÐ£¬Èç¹ûÃ»ÓÐÉèÖÃtimeout£¬²»»á×¢²átimeoutÊÂ¼þ£¬·µ»Ø0
}

/* 
 * This callback is executed when the size of the input buffer changes.
 * We use it to apply back pressure on the reading side.
 */        //apply back means ÉêÇë»ØÀ´

void
bufferevent_read_pressure_cb(struct evbuffer *buf, size_t old, size_t now,  //¼ÓreadÑ¹Á¦
    void *arg) {
	struct bufferevent *bufev = arg;
	/* 
	 * If we are below the watermark then reschedule reading if it's
	 * still enabled.
	 */
	if (bufev->wm_read.high == 0 || now < bufev->wm_read.high) {  //Èç¹û¸ßË®Î»ÏßÎª0£¬»òÕßµ±Ç°Ð¡ÓÚ¸ßË®Î»Ïß£¬²»»Øµ÷
		evbuffer_setcb(buf, NULL, NULL); //ÎªÊ²Ã´Òªµ÷ÓÃÕâ¸öº¯Êý£¿

		if (bufev->enabled & EV_READ)
			bufferevent_add(&bufev->ev_read, bufev->timeout_read);  //ÈÃÕâ¸öÊ±¼ä¹ýÒ»¶¨Ê±¼äÔÙ¶Á
	}										//timeout_readÔÚbufferevent_settimeoutº¯ÊýÀïÃæ»áÉèÖÃ£¬
}

static void
bufferevent_readcb(int fd, short event, void *arg)  //»º³åÇø¿É¶Á»á´¥·¢¸Ã»Øµ÷
{
	struct bufferevent *bufev = arg;
	int res = 0;
	short what = EVBUFFER_READ;
	size_t len;
	int howmuch = -1;

	/* Note that we only check for event==EV_TIMEOUT. If
	* event==EV_TIMEOUT|EV_READ, we can safely ignore the
	* timeout, since a read has occurred */
	if (event == EV_TIMEOUT) {     //Èç¹ûÊÇ³¬Ê±ÊÂ¼þ£¬ºöÂÔ
		what |= EVBUFFER_TIMEOUT;
		goto error;
	}

	/*
	 * If we have a high watermark configured then we don't want to   // ÊÇ·ñÉèÖÃÁËÊäÈë»º³åÇøµÄ×î´ó´óÐ¡
	 * read more data than would make us reach the watermark.
	 */                              //ÏÈ¼ì²âinput»º³åÇøÒÑÓÐµÄÊý¾Ý
	if (bufev->wm_read.high != 0) {   //Èç¹û¶ÁÈ¡¸ßË®Î»²»Îª0
		howmuch = bufev->wm_read.high - EVBUFFER_LENGTH(bufev->input);   //¸ßË®Î»³¤¶È-Êµ¼ÊÊý¾Ý³¤¶È
		/* we might have lowered the watermark, stop reading */ //ÎÒÃÇ¿ÉÄÜÒÑ¾­½µµÍÁËË®Î»£¬Í£Ö¹ÔÄ¶Á£¬Ë®Î»¸Ä¶¯
		if (howmuch <= 0) {   //Èç¹ûÎª<=0£¬ËµÃ÷ÊäÈë»º³åÇøµÄÊý¾ÝÁ¿´ïµ½ÁËÒ»¶¨¼¶±ð£¬buffereventÍ£Ö¹¶ÁÈ¡
			struct evbuffer *buf = bufev->input;
			event_del(&bufev->ev_read);   //²»ÄÜ¶ÁÁË£¬É¾³ý¿É¶ÁÊÂ¼þ
			evbuffer_setcb(buf,
			    bufferevent_read_pressure_cb, bufev);   
			return;  //Ö±½Ó·µ»Ø£¬²»ÔÙ¶ÁÈ¡
		}
	}

	res = evbuffer_read(bufev->input, fd, howmuch);   //´Ófd¶ÁÈ¡Êý¾Ýµ½bufev->input
	if (res == -1) {
		if (errno == EAGAIN || errno == EINTR)   //EAGAIN×ÖÃæÒâË¼ÊÇÔÙÊÔÒ»´Î£¬±ÈÈç´ò¿ªÒÔnonblock´ò¿ªÒ»¸ösocket£
										//Á¬Ðøreadµ«ÎÞÊý¾Ý¿É¶Á£¬¾Í»á·µ»ØEAGAIN£¬ÒâË¼ÈÃÄãÔÙÊÔÒ»´Î¡£EINTR
			goto reschedule;    //Ìøµ½ÖØÐÂ¼Æ»®
		/* error case */
		what |= EVBUFFER_ERROR;    //·ñÔò¼ÓÉÏEVBUFFER_ERROR
	} else if (res == 0) {
		/* eof case */
		what |= EVBUFFER_EOF;  //»º³åÇø½áÎ²ÁË
	}

	if (res <= 0)
		goto error;

	bufferevent_add(&bufev->ev_read, bufev->timeout_read); //Ìí¼Ó³¬Ê±

	/* See if this callbacks meets the water marks */
	len = EVBUFFER_LENGTH(bufev->input);
	if (bufev->wm_read.low != 0 && len < bufev->wm_read.low)   //Èç¹û×îµÍË®Î»Ïß²»µÈÓÚ0£¬ÇÒÊµ¼Ê×Ö½ÚÐ¡ÓÚ×îµÍË®Î»Ïß£¬²»´¦Àí£¬¼ÌÐø¶ÁÈ¡fd
		return;
	if (bufev->wm_read.high != 0 && len >= bufev->wm_read.high) {//×î¸ßË®Î»Ïß²»Îª0£¬ÇÒ³¤¶È´óÓÚ×î¸ßË®Î»Ïß  //ÕâÊÇ´Ófd¶ÁÈ¡Êý¾ÝÖ®ºóµÄÇé¿ö
		struct evbuffer *buf = bufev->input;  
		event_del(&bufev->ev_read);      //´Ó×¢²áÁ´±íÖÐÉ¾³ý

		/* Now schedule a callback for us when the buffer changes */
		evbuffer_setcb(buf, bufferevent_read_pressure_cb, bufev);   //ÉèÖÃ¶ÁÑ¹»Øµ÷º¯Êý
	}

	/* Invoke the user callback - must always be called last */
	if (bufev->readcb != NULL)
		(*bufev->readcb)(bufev, bufev->cbarg);  
	return;

 reschedule:
	bufferevent_add(&bufev->ev_read, bufev->timeout_read);   //Ò»¶¨Ê±¼äºóÔÙ¶Á
	return;

 error:
	(*bufev->errorcb)(bufev, what, bufev->cbarg);
}

static void
bufferevent_writecb(int fd, short event, void *arg)
{
	struct bufferevent *bufev = arg;
	int res = 0;
	short what = EVBUFFER_WRITE;

	if (event == EV_TIMEOUT) {
		what |= EVBUFFER_TIMEOUT;   //Í¬Ñùignore
		goto error;
	}

	if (EVBUFFER_LENGTH(bufev->output)) {  //Èç¹ûÓÐÊý¾Ý
	    res = evbuffer_write(bufev->output, fd);  //ÓÐÊý¾Ý¾ÍÖ±½ÓÐ´µ½fd
	    if (res == -1) {
#ifndef WIN32
/*todo. evbuffer uses WriteFile when WIN32 is set. WIN32 system calls do not
 *set errno. thus this error checking is not portable*/
		    if (errno == EAGAIN ||
			errno == EINTR ||
			errno == EINPROGRESS)
			    goto reschedule;
		    /* error case */
		    what |= EVBUFFER_ERROR;

#else
				goto reschedule;
#endif

	    } else if (res == 0) {
		    /* eof case */
		    what |= EVBUFFER_EOF;
	    }
	    if (res <= 0)
		    goto error;
	}

	if (EVBUFFER_LENGTH(bufev->output) != 0)  //Èç¹ûÊä³ö»º³åÇø»¹ÓÐÊý¾Ý£¬ÄÇ¾Í¹ý¶ÎÊ±¼äÔÙÊä³ö
		bufferevent_add(&bufev->ev_write, bufev->timeout_write);

	/*
	 * Invoke the user callback if our buffer is drained or below the
	 * low watermark.
	 */
	if (bufev->writecb != NULL &&
	    EVBUFFER_LENGTH(bufev->output) <= bufev->wm_write.low)//µ½´ïµÍË®Î»µÄ»Øµ÷
		(*bufev->writecb)(bufev, bufev->cbarg);

	return;

 reschedule:
	if (EVBUFFER_LENGTH(bufev->output) != 0)  //Èç¹ûÊý¾Ý²»Îª0£¬Ò»¶ÎÊ±¼äºóÖØÐ´
		bufferevent_add(&bufev->ev_write, bufev->timeout_write);
	return;

 error:
	(*bufev->errorcb)(bufev, what, bufev->cbarg);
}

/*
 * Create a new buffered event object.
 *
 * The read callback is invoked whenever we read new data.
 * The write callback is invoked whenever the output buffer is drained.
 * The error callback is invoked on a write/read error or on EOF.
 *
 * Both read and write callbacks maybe NULL.  The error callback is not
 * allowed to be NULL and have to be provided always.
 */

//·ÖÅäbufferevent½á¹¹Ìå
struct bufferevent *
bufferevent_new(int fd, evbuffercb readcb, evbuffercb writecb,
    everrorcb errorcb, void *cbarg)
{
	struct bufferevent *bufev;

	if ((bufev = calloc(1, sizeof(struct bufferevent))) == NULL)
		return (NULL);

	if ((bufev->input = evbuffer_new()) == NULL) {   //·ÖÅäevbuffer
		free(bufev);
		return (NULL);
	}

	if ((bufev->output = evbuffer_new()) == NULL) {
		evbuffer_free(bufev->input);   //Èç¹ûÊ§°Ü£¬Îö¹¹µôÖ®Ç°µÄ£¬±ÜÃâÄÚ´æÐ¹Â©
		free(bufev);
		return (NULL);
	}

	event_set(&bufev->ev_read, fd, EV_READ, bufferevent_readcb, bufev);  //ÉèÖÃbuffereventÄÚ²¿¶ÁÐ´ÊÂ¼þ
	event_set(&bufev->ev_write, fd, EV_WRITE, bufferevent_writecb, bufev);

	bufferevent_setcb(bufev, readcb, writecb, errorcb, cbarg);

	/*
	 * Set to EV_WRITE so that using bufferevent_write is going to
	 * trigger a callback.  Reading needs to be explicitly enabled
	 * because otherwise no data will be available.
	 */                              //×Ô¶¯¿ªÆôEV_WRITE,EV_READÐèÒªÏÔÊ¾ÉùÃ÷
	bufev->enabled = EV_WRITE; // /** Events that are currently enabled: currently EV_READ and EV_WRITE are supported. */

	return (bufev);
}

void
bufferevent_setcb(struct bufferevent *bufev,
    evbuffercb readcb, evbuffercb writecb, everrorcb errorcb, void *cbarg)
{
	bufev->readcb = readcb;
	bufev->writecb = writecb;
	bufev->errorcb = errorcb;

	bufev->cbarg = cbarg;
}

void
bufferevent_setfd(struct bufferevent *bufev, int fd)   //
{
	event_del(&bufev->ev_read);   //ÏÈ½â×¢²áÖ®Ç°µÄÊÂ¼þ
	event_del(&bufev->ev_write);

	event_set(&bufev->ev_read, fd, EV_READ, bufferevent_readcb, bufev);
	event_set(&bufev->ev_write, fd, EV_WRITE, bufferevent_writecb, bufev);
	if (bufev->ev_base != NULL) {
		event_base_set(bufev->ev_base, &bufev->ev_read);
		event_base_set(bufev->ev_base, &bufev->ev_write);
	}

	/* might have to manually trigger event registration */
}

int
bufferevent_priority_set(struct bufferevent *bufev, int priority)
{
	if (event_priority_set(&bufev->ev_read, priority) == -1)   //ÉèÖÃÓÅÏÈ¼¶
		return (-1);
	if (event_priority_set(&bufev->ev_write, priority) == -1)
		return (-1);

	return (0);
}

/* Closing the file descriptor is the responsibility of the caller */

void
bufferevent_free(struct bufferevent *bufev)
{
	event_del(&bufev->ev_read);
	event_del(&bufev->ev_write);

	evbuffer_free(bufev->input);
	evbuffer_free(bufev->output);

	free(bufev);
}

/*
 * Returns 0 on success;
 *        -1 on failure.
 */

//buffereventÐ´ÊÂ¼þ
int
bufferevent_write(struct bufferevent *bufev, const void *data, size_t size)
{
	int res;

	res = evbuffer_add(bufev->output, data, size);   //µ÷ÓÃ×·¼Ódataº¯Êý£¬¼´evbuffer_addº¯Êý

	if (res == -1)
		return (res);

	/* If everything is okay, we need to schedule a write */
	if (size > 0 && (bufev->enabled & EV_WRITE))     //ÉÏÃædata×·¼ÓÍê±Ï£¬ÏÖÔÚ½«bufev->ev_writeÊÂ¼þ¼ÓÈë·´Ó¦¶Ñ£¬µÈºò
		bufferevent_add(&bufev->ev_write, bufev->timeout_write);

	return (res);
}

int
bufferevent_write_buffer(struct bufferevent *bufev, struct evbuffer *buf)
{
	int res;

	res = bufferevent_write(bufev, buf->buffer, buf->off);
	if (res != -1)
		evbuffer_drain(buf, buf->off);  //ÎªÊ²Ã´·¢ËÍÊ§°ÜÒªdrain

	return (res);
}

size_t
bufferevent_read(struct bufferevent *bufev, void *data, size_t size)
{
	struct evbuffer *buf = bufev->input;

	if (buf->off < size)     //Èç¹ûÐ¡ÓÚÒª¶ÁµÄ×Ö½ÚÊý
		size = buf->off;    //¾Í¶ÁÊµ¼ÊÊý¾Ý

	/* Copy the available data to the user buffer */
	memcpy(data, buf->buffer, size);

	if (size)
		evbuffer_drain(buf, size); //¶ÁÍêsize×Ö½Úºóµ÷ÕûÒ»ÏÂ

	return (size);
}

//½«Ä³¸öeventÌí¼Óµ½event_baseÖÐ£¬bufferevent¿ª¹Ø
int
bufferevent_enable(struct bufferevent *bufev, short event)
{
	if (event & EV_READ) {
		if (bufferevent_add(&bufev->ev_read, bufev->timeout_read) == -1)
			return (-1);
	}
	if (event & EV_WRITE) {
		if (bufferevent_add(&bufev->ev_write, bufev->timeout_write) == -1)
			return (-1);
	}

	bufev->enabled |= event;
	return (0);
}

//´Ó¶ÔÓ¦¶ÓÁÐÉ¾³ý
int
bufferevent_disable(struct bufferevent *bufev, short event)
{
	if (event & EV_READ) {
		if (event_del(&bufev->ev_read) == -1)
			return (-1);
	}
	if (event & EV_WRITE) {
		if (event_del(&bufev->ev_write) == -1)
			return (-1);
	}

	bufev->enabled &= ~event;
	return (0);
}

/*
 * Sets the read and write timeout for a buffered event.
 */

//ÉèÖÃ³¬Ê±
void
bufferevent_settimeout(struct bufferevent *bufev,
    int timeout_read, int timeout_write) {
	bufev->timeout_read = timeout_read;
	bufev->timeout_write = timeout_write;

	if (event_pending(&bufev->ev_read, EV_READ, NULL))//¼ì²âÊ±¼äÊÇ·ñ´¦ÓÚÎ´¾ö×´Ì¬
		bufferevent_add(&bufev->ev_read, timeout_read);
	if (event_pending(&bufev->ev_write, EV_WRITE, NULL))
		bufferevent_add(&bufev->ev_write, timeout_write);
}  

/*
 * Sets the water marks
 */
//ÉèÖÃË®Î»Ïß°¡
void
bufferevent_setwatermark(struct bufferevent *bufev, short events,
    size_t lowmark, size_t highmark)
{
	if (events & EV_READ) {
		bufev->wm_read.low = lowmark;
		bufev->wm_read.high = highmark;
	}

	if (events & EV_WRITE) {
		bufev->wm_write.low = lowmark;
		bufev->wm_write.high = highmark;
	}

	/* If the watermarks changed then see if we should call read again */
	bufferevent_read_pressure_cb(bufev->input,
	    0, EVBUFFER_LENGTH(bufev->input), bufev);
}
//ºÍevent_base¹ØÁª
int
bufferevent_base_set(struct event_base *base, struct bufferevent *bufev)
{
	int res;

	bufev->ev_base = base;

	res = event_base_set(base, &bufev->ev_read);
	if (res == -1)
		return (res);

	res = event_base_set(base, &bufev->ev_write);
	return (res);
}
