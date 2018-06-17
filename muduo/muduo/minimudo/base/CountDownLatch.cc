// Use of this source code is governed by a BSD-style license
// that can be found in the License file.
//
// Author: Shuo Chen (chenshuo at chenshuo dot com)

#include <muduo/base/CountDownLatch.h>

using namespace muduo;

CountDownLatch::CountDownLatch(int count)
  : mutex_(),
    condition_(mutex_),   //初始化，条件变量用成员锁初始化
    count_(count)
{
}

void CountDownLatch::wait()
{
  MutexLockGuard lock(mutex_);
  while (count_ > 0)    //只要计数值大于0，CountDownLatch类就不工作，知道等待计数值为0
  {
    condition_.wait();
  }
}

void CountDownLatch::countDown()  //倒数，倒计时
{
  MutexLockGuard lock(mutex_);
  --count_;
  if (count_ == 0)
  {
    condition_.notifyAll();
  }
}

int CountDownLatch::getCount() const  //获得次数
{
  MutexLockGuard lock(mutex_);
  return count_;
}

