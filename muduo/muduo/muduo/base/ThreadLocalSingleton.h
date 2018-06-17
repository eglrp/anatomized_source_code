// Use of this source code is governed by a BSD-style license
// that can be found in the License file.
//
// Author: Shuo Chen (chenshuo at chenshuo dot com)

#ifndef MUDUO_BASE_THREADLOCALSINGLETON_H
#define MUDUO_BASE_THREADLOCALSINGLETON_H

#include <boost/noncopyable.hpp>
#include <assert.h>
#include <pthread.h>

namespace muduo
{

template<typename T>
class ThreadLocalSingleton : boost::noncopyable
{
 public:

  static T& instance() //返回单例对象，不需要按照线程安全方式实现，因为本身就是__thread类型
  {
    if (!t_value_)   //如果指针为空创建
    {
      t_value_ = new T();
      deleter_.set(t_value_);    //把t_value_指针暴露给deleter，为了垃圾回收
    }
    return *t_value_;
  }

  static T* pointer()
  {
    return t_value_;
  }

 private:
  ThreadLocalSingleton();
  ~ThreadLocalSingleton();

  static void destructor(void* obj)   //内层类中线程结束时，会调用此函数取清理key指向的数据
  {
    assert(obj == t_value_);
    typedef char T_must_be_complete_type[sizeof(T) == 0 ? -1 : 1];
    T_must_be_complete_type dummy; (void) dummy;
    delete t_value_;
    t_value_ = 0;
  }

  class Deleter
  {
   public:
    Deleter()
    {
      pthread_key_create(&pkey_, &ThreadLocalSingleton::destructor);  //设置外面类的destructor方法，
    }

    ~Deleter()
    {
      pthread_key_delete(pkey_);
    }

    void set(T* newObj)
    {
      assert(pthread_getspecific(pkey_) == NULL);   //保证之前key没有指向数据
      pthread_setspecific(pkey_, newObj);   //现在设置key指向newobj指针，在ThreadLocalSingleton中会传入t_value_指针，
     	//实际上是为了实现垃圾回收，线程结束时，deleter调用destroy，释放key指向的数据，而这个数据正好就是外部类的成员t_value_指针					    
    }

    pthread_key_t pkey_;
  };

  static __thread T* t_value_;   //__thread关键字保证线程局部属性，只能修饰POD类型
  static Deleter deleter_;   //用来销毁T*指针所指对象
};

template<typename T>
__thread T* ThreadLocalSingleton<T>::t_value_ = 0;

template<typename T>
typename ThreadLocalSingleton<T>::Deleter ThreadLocalSingleton<T>::deleter_;

}
#endif
