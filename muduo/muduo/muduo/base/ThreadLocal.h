// Use of this source code is governed by a BSD-style license
// that can be found in the License file.
//
// Author: Shuo Chen (chenshuo at chenshuo dot com)

#ifndef MUDUO_BASE_THREADLOCAL_H
#define MUDUO_BASE_THREADLOCAL_H

#include <muduo/base/Mutex.h>  // MCHECK

#include <boost/noncopyable.hpp>
#include <pthread.h>

namespace muduo
{

template<typename T>
class ThreadLocal : boost::noncopyable
{
 public:
  ThreadLocal()
  {
    //构造函数中创建key，数据的销毁由destructor来销毁
    MCHECK(pthread_key_create(&pkey_, &ThreadLocal::destructor));  
  }

  ~ThreadLocal()
  {
    //析构函数中销毁key
    MCHECK(pthread_key_delete(pkey_));
  }

  //获取线程特定数据
  T& value()
  {
    T* perThreadValue = static_cast<T*>(pthread_getspecific(pkey_)); //通过key获取线程特定数据
    if (!perThreadValue)  //如果是空的，说明特定数据还没有创建，那么就空构造一个
    {
      T* newObj = new T();
      MCHECK(pthread_setspecific(pkey_, newObj));  //设置特定数据
      perThreadValue = newObj;   //返回
    }
    return *perThreadValue;   //返回对象引用，所以需要*
  }

 private:

  static void destructor(void *x)
  {
    T* obj = static_cast<T*>(x);
    typedef char T_must_be_complete_type[sizeof(T) == 0 ? -1 : 1];   //检测是否是完全类型
    T_must_be_complete_type dummy; (void) dummy; 
    delete obj;   //如果是，我们就可以删除它了
  }

 private:
  pthread_key_t pkey_;    //key的类型是pthread_key_t类型
};

}
#endif
