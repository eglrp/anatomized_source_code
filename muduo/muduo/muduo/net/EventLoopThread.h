// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)
//
// This is a public header file, it must only include public header files.

#ifndef MUDUO_NET_EVENTLOOPTHREAD_H
#define MUDUO_NET_EVENTLOOPTHREAD_H

#include <muduo/base/Condition.h>
#include <muduo/base/Mutex.h>
#include <muduo/base/Thread.h>

#include <boost/noncopyable.hpp>

namespace muduo
{
namespace net
{

class EventLoop;

class EventLoopThread : boost::noncopyable
{
 public:
  typedef boost::function<void(EventLoop*)> ThreadInitCallback;

  EventLoopThread(const ThreadInitCallback& cb = ThreadInitCallback(),
                  const string& name = string());
  ~EventLoopThread();
  EventLoop* startLoop();  //������Աthread_�̣߳����߳̾ͳ���I/O�̣߳��ڲ�����thread_.start()

 private:
  void threadFunc();   //�߳����к���

  EventLoop* loop_;        //ָ��һ��EventLoop����һ��I/O�߳�����ֻ��һ��EventLoop����
  bool exiting_;
  Thread thread_;          //���ڶ��󣬰�����һ��thread�����
  MutexLock mutex_;
  Condition cond_;
  ThreadInitCallback callback_;
};

}
}

#endif  // MUDUO_NET_EVENTLOOPTHREAD_H

