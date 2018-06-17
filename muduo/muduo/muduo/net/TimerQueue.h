// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)
//
// This is an internal header file, you should not include this.

#ifndef MUDUO_NET_TIMERQUEUE_H
#define MUDUO_NET_TIMERQUEUE_H

#include <set>
#include <vector>

#include <boost/noncopyable.hpp>

#include <muduo/base/Mutex.h>
#include <muduo/base/Timestamp.h>
#include <muduo/net/Callbacks.h>
#include <muduo/net/Channel.h>

namespace muduo
{
namespace net
{

class EventLoop;
class Timer;
class TimerId;

///
/// A best efforts timer queue.
/// No guarantee that the callback will be on time.
///
class TimerQueue : boost::noncopyable
{
 public:
  TimerQueue(EventLoop* loop);
  ~TimerQueue();

  ///
  /// Schedules the callback to be run at given time,
  /// repeats if @c interval > 0.0.
  ///
  /// Must be thread safe. Usually be called from other threads.
  //Ò»¶¨ÊÇÏß³Ì°²È«µÄ£¬¿ÉÒÔ¿çÏß³Ìµ÷ÓÃ¡£Í¨³£Çé¿öÏÂ±»ÆäËûÏß³Ìµ÷ÓÃ
  TimerId addTimer(const TimerCallback& cb,
                   Timestamp when,
                   double interval);
#ifdef __GXX_EXPERIMENTAL_CXX0X__
  TimerId addTimer(TimerCallback&& cb,
                   Timestamp when,
                   double interval);
#endif

  void cancel(TimerId timerId);  //¿ÉÒÔ¿çÏß³Ìµ÷ÓÃ

 private:

  // FIXME: use unique_ptr<Timer> instead of raw pointers.
  //ÏÂÃæÁ½¸öset¿ÉÒÔËµ±£´æµÄÊÇÏàÍ¬µÄ¶«Î÷£¬¶¼ÊÇ¶¨Ê±Æ÷£¬Ö»²»¹ıÅÅĞò·½Ê½²»Í¬
  typedef std::pair<Timestamp, Timer*> Entry;  //setµÄkey£¬ÊÇÒ»¸öÊ±¼ä´ÁºÍ¶¨Ê±Æ÷µØÖ·µÄpair
  typedef std::set<Entry> TimerList;    //°´ÕÕÊ±¼ä´ÁÅÅĞò
  typedef std::pair<Timer*, int64_t> ActiveTimer;  //¶¨Ê±Æ÷µØÖ·ºÍĞòºÅ
  typedef std::set<ActiveTimer> ActiveTimerSet;  //°´ÕÕ¶¨Ê±Æ÷µØÖ·ÅÅĞò

  //ÒÔÏÂ³ÉÔ±º¯ÊıÖ»¿ÉÄÜÔÚÆäËùÊôµÄI/OÏß³ÌÖĞµ÷ÓÃ£¬Òò¶ø²»±Ø¼ÓËø
  //·şÎñÆ÷ĞÔÄÜÉ±ÊÖÖ®Ò»¾ÍÊÇËø¾ºÕù£¬Òª¾¡¿ÉÄÜÉÙÊ¹ÓÃËø
  void addTimerInLoop(Timer* timer);
  void cancelInLoop(TimerId timerId);
  // called when timerfd alarms
  void handleRead();  //¶¨Ê±Æ÷ÊÂ¼ş²úÉú»Øµ÷º¯Êı
  // move out all expired timers
  std::vector<Entry> getExpired(Timestamp now);  //·µ»Ø³¬Ê±µÄ¶¨Ê±Æ÷ÁĞ±í
  void reset(const std::vector<Entry>& expired, Timestamp now);   //¶Ô³¬Ê±µÄ¶¨Ê±Æ÷½øĞĞÖØÖÃ£¬ÒòÎª³¬Ê±µÄ¶¨Ê±Æ÷¿ÉÄÜÊÇÖØ¸´µÄ¶¨Ê±Æ÷

  bool insert(Timer* timer);  //²åÈë¶¨Ê±Æ÷

  EventLoop* loop_;    //ËùÊôµÄevent_loop
  const int timerfd_;    //¾ÍÊÇtimefd_create()Ëù´´½¨µÄ¶¨Ê±Æ÷ÃèÊö·û£
  Channel timerfdChannel_;   //ÕâÊÇ¶¨Ê±Æ÷ÊÂ¼şµÄÍ¨µÀ
  // Timer list sorted by expiration
  TimerList timers_;   //¶¨Ê±Æ÷set£¬°´Ê±¼ä´ÁÅÅĞò

  // for cancel()
  ActiveTimerSet activeTimers_;  //»îÔ¾¶¨Ê±Æ÷ÁĞ±í£¬°´¶¨Ê±Æ÷µØÖ·ÅÅĞò
  bool callingExpiredTimers_; /* atomic */  //ÊÇ·ñ´¦ÓÚµ÷ÓÃ´¦Àí³¬Ê±¶¨Ê±Æ÷µ±ÖĞ
  ActiveTimerSet cancelingTimers_;    //±£´æµÄÊÇ±»È¡ÏûµÄ¶¨Ê±Æ÷
};

}
}
#endif  // MUDUO_NET_TIMERQUEUE_H
