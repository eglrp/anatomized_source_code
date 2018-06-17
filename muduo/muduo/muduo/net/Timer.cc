// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)

#include <muduo/net/Timer.h>

using namespace muduo;
using namespace muduo::net;

AtomicInt64 Timer::s_numCreated_;

void Timer::restart(Timestamp now)   //ÖØÆô
{
  if (repeat_)   //Èç¹ûÊÇÖØ¸´µÄ£¬ÄÇÃ´¾Í´Óµ±Ç°Ê±¼ä¼ÆËãÏÂÒ»´ÎµÄ³¬Ê±Ê±¿Ì
  {
    expiration_ = addTime(now, interval_);  //µ±Ç°Ê±¼ä¼ÓÉÏÊ±¼ä¼ä¸ô¡
  }
  else
  {
    expiration_ = Timestamp::invalid();   //»ñÈ¡Ò»¸öÎŞĞ§ÊÂ¼ş´Á£¬¼´ÖµÎª0
  }
}
