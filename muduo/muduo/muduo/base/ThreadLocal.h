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
    //���캯���д���key�����ݵ�������destructor������
    MCHECK(pthread_key_create(&pkey_, &ThreadLocal::destructor));  
  }

  ~ThreadLocal()
  {
    //��������������key
    MCHECK(pthread_key_delete(pkey_));
  }

  //��ȡ�߳��ض�����
  T& value()
  {
    T* perThreadValue = static_cast<T*>(pthread_getspecific(pkey_)); //ͨ��key��ȡ�߳��ض�����
    if (!perThreadValue)  //����ǿյģ�˵���ض����ݻ�û�д�������ô�Ϳչ���һ��
    {
      T* newObj = new T();
      MCHECK(pthread_setspecific(pkey_, newObj));  //�����ض�����
      perThreadValue = newObj;   //����
    }
    return *perThreadValue;   //���ض������ã�������Ҫ*
  }

 private:

  static void destructor(void *x)
  {
    T* obj = static_cast<T*>(x);
    typedef char T_must_be_complete_type[sizeof(T) == 0 ? -1 : 1];   //����Ƿ�����ȫ����
    T_must_be_complete_type dummy; (void) dummy; 
    delete obj;   //����ǣ����ǾͿ���ɾ������
  }

 private:
  pthread_key_t pkey_;    //key��������pthread_key_t����
};

}
#endif
