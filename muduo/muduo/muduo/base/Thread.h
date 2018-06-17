// Use of this source code is governed by a BSD-style license
// that can be found in the License file.
//
// Author: Shuo Chen (chenshuo at chenshuo dot com)

#ifndef MUDUO_BASE_THREAD_H
#define MUDUO_BASE_THREAD_H

#include <muduo/base/Atomic.h>
#include <muduo/base/Types.h>

#include <boost/function.hpp>
#include <boost/noncopyable.hpp>
#include <boost/shared_ptr.hpp>
#include <pthread.h>

namespace muduo
{

class Thread : boost::noncopyable
{
 public:
  typedef boost::function<void ()> ThreadFunc;

  explicit Thread(const ThreadFunc&, const string& name = string());
#ifdef __GXX_EXPERIMENTAL_CXX0X__
  explicit Thread(ThreadFunc&&, const string& name = string()); //��ֵ���ã��ڶ��󷵻ص�ʱ�򲻻´��������ʱ���󣬶��Ǻ���ʱ���󽻻��������Ч��
#endif
  ~Thread();

  void start();
  int join(); // return pthread_join()

  bool started() const { return started_; }
  // pthread_t pthreadId() const { return pthreadId_; }
  pid_t tid() const { return *tid_; }   //����pthread_t��id�ſ�����һ���ģ�������Ҫ��gettid()
  const string& name() const { return name_; }

  static int numCreated() { return numCreated_.get(); }   

 private:
  void setDefaultName();

  bool       started_;
  bool       joined_;
  pthread_t  pthreadId_;
  boost::shared_ptr<pid_t> tid_;    //��������������
  ThreadFunc func_;                        //�̻߳ص�����
  string     name_;

  static AtomicInt32 numCreated_;   //ԭ�Ӳ���
};

}
#endif
