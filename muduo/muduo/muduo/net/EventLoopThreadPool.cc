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
  : baseLoop_(baseLoop),   //main Reactor���ڵ�I/O�߳�
    name_(nameArg),
    started_(false),
    numThreads_(0),
    next_(0)
{
}

//����Ҫ�ͷ�loop����Ϊ���Ƕ���ջ�϶���
EventLoopThreadPool::~EventLoopThreadPool()
{
  // Don't delete loop, it's stack variable
}

//����EventLoopThread��
void EventLoopThreadPool::start(const ThreadInitCallback& cb)
{
  assert(!started_);
  baseLoop_->assertInLoopThread();

  started_ = true;

  for (int i = 0; i < numThreads_; ++i)
  {
    char buf[name_.size() + 32];
    snprintf(buf, sizeof buf, "%s%d", name_.c_str(), i);
    EventLoopThread* t = new EventLoopThread(cb, buf);   //�������ɸ�I/O�߳�
    threads_.push_back(t);   //ѹ�뵽threads_
    loops_.push_back(t->startLoop());   //����ÿ��EventLoopThread�߳̽���loop()�����Ұѷ��ص�ÿ��EventLoopָ��ѹ�뵽loops_
  }
  if (numThreads_ == 0 && cb)  //����0��Ҳ����û�д���EventLoopThread�߳�
  {
    //ֻ��һ��EventLoop�������EventLoop�����¼�ѭ��֮ǰ������cb
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

  //���loops_Ϊ�գ�˵������û�д�������EventLoopThread��ֻ��һ��main Reactor����ôֱ�ӷ���baseLoop_
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
    return std::vector<EventLoop*>(1, baseLoop_);   //���Ϊ�գ�ֻ����base_loop_
  }
  else
  {
    return loops_;   //��Ϊ��ȫ������
  }
}
