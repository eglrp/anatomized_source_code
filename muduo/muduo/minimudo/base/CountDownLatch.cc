// Use of this source code is governed by a BSD-style license
// that can be found in the License file.
//
// Author: Shuo Chen (chenshuo at chenshuo dot com)

#include <muduo/base/CountDownLatch.h>

using namespace muduo;

CountDownLatch::CountDownLatch(int count)
  : mutex_(),
    condition_(mutex_),   //��ʼ�������������ó�Ա����ʼ��
    count_(count)
{
}

void CountDownLatch::wait()
{
  MutexLockGuard lock(mutex_);
  while (count_ > 0)    //ֻҪ����ֵ����0��CountDownLatch��Ͳ�������֪���ȴ�����ֵΪ0
  {
    condition_.wait();
  }
}

void CountDownLatch::countDown()  //����������ʱ
{
  MutexLockGuard lock(mutex_);
  --count_;
  if (count_ == 0)
  {
    condition_.notifyAll();
  }
}

int CountDownLatch::getCount() const  //��ô���
{
  MutexLockGuard lock(mutex_);
  return count_;
}

