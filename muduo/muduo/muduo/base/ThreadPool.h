// Use of this source code is governed by a BSD-style license
// that can be found in the License file.
//
// Author: Shuo Chen (chenshuo at chenshuo dot com)

#ifndef MUDUO_BASE_THREADPOOL_H
#define MUDUO_BASE_THREADPOOL_H

#include <muduo/base/Condition.h>
#include <muduo/base/Mutex.h>
#include <muduo/base/Thread.h>
#include <muduo/base/Types.h>

#include <boost/function.hpp>
#include <boost/noncopyable.hpp>
#include <boost/ptr_container/ptr_vector.hpp>

#include <deque>

namespace muduo
{

class ThreadPool : boost::noncopyable
{
 public:
  typedef boost::function<void ()> Task;

  explicit ThreadPool(const string& nameArg = string("ThreadPool"));
  ~ThreadPool();

  // Must be called before start().
  void setMaxQueueSize(int maxSize) { maxQueueSize_ = maxSize; }   //设置最大线程池线程最大数目大小
  void setThreadInitCallback(const Task& cb)    //设置线程执行前的回调函数
  { threadInitCallback_ = cb; }

  void start(int numThreads);
  void stop();

  const string& name() const
  { return name_; }

  size_t queueSize() const;

  // Could block if maxQueueSize > 0
  void run(const Task& f);
#ifdef __GXX_EXPERIMENTAL_CXX0X__
  void run(Task&& f);
#endif

 private:
  bool isFull() const;    //判满
  void runInThread();   //线程池的线程运行函数
  Task take();    //取任务函数

  mutable MutexLock mutex_;   
  Condition notEmpty_;    //不空condition
  Condition notFull_;         //未满condition
  string name_;
  Task threadInitCallback_;    //线程执行前的回调函数
  boost::ptr_vector<muduo::Thread> threads_;  //线程数组
  std::deque<Task> queue_;    //任务队列
  size_t maxQueueSize_;     //因为deque是通过push_back增加线程数目的，所以通过外界max_queuesize存储最多线程数目
  bool running_;    //线程池运行标志
};

}

#endif
