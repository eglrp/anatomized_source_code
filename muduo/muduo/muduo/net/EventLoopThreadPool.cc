// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)

#include <muduo/net/EventLoopThreadPool.h>

#include <muduo/net/EventLoop.h>
#include <muduo/net/EventLoopThread.h>

#include <boost/bind.hpp>

#include <stdio.h>

using namespace muduo;
using namespace muduo::net;


EventLoopThreadPool::EventLoopThreadPool(EventLoop* baseLoop, const string& nameArg)
  : baseLoop_(baseLoop),   //main Reactor所在的I/O线程
    name_(nameArg),
    started_(false),
    numThreads_(0),
    next_(0)
{
}

//不需要释放loop，因为它们都是栈上对象
EventLoopThreadPool::~EventLoopThreadPool()
{
  // Don't delete loop, it's stack variable
}

//启动EventLoopThread池
void EventLoopThreadPool::start(const ThreadInitCallback& cb)
{
  assert(!started_);
  baseLoop_->assertInLoopThread();

  started_ = true;

  for (int i = 0; i < numThreads_; ++i)
  {
    char buf[name_.size() + 32];
    snprintf(buf, sizeof buf, "%s%d", name_.c_str(), i);
    EventLoopThread* t = new EventLoopThread(cb, buf);   //创建若干个I/O线程
    threads_.push_back(t);   //压入到threads_
    loops_.push_back(t->startLoop());   //启动每个EventLoopThread线程进入loop()，并且把返回的每个EventLoop指针压入到loops_
  }
  if (numThreads_ == 0 && cb)  //创建0个也就是没有创建EventLoopThread线程
  {
    //只有一个EventLoop，在这个EventLoop进入事件循环之前，调用cb
    cb(baseLoop_);  
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//round-robin
EventLoop* EventLoopThreadPool::getNextLoop()
{
  baseLoop_->assertInLoopThread();
  assert(started_);
  EventLoop* loop = baseLoop_;

  //如果loops_为空，说明我们没有创建其他EventLoopThread，只有一个main Reactor，那么直接返回baseLoop_
  if (!loops_.empty())
  {
    // round-robin   round-robin
    loop = loops_[next_];
    ++next_;
    if (implicit_cast<size_t>(next_) >= loops_.size())
    {
      next_ = 0;
    }
  }
  return loop;
}

//hash
EventLoop* EventLoopThreadPool::getLoopForHash(size_t hashCode)
{
  baseLoop_->assertInLoopThread();
  EventLoop* loop = baseLoop_;

  if (!loops_.empty())
  {
    loop = loops_[hashCode % loops_.size()];
  }
  return loop;
}
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

std::vector<EventLoop*> EventLoopThreadPool::getAllLoops()
{
  baseLoop_->assertInLoopThread();
  assert(started_);
  if (loops_.empty())
  {
    return std::vector<EventLoop*>(1, baseLoop_);   //如果为空，只返回base_loop_
  }
  else
  {
    return loops_;   //不为空全部返回
  }
}
