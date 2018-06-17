// Use of this source code is governed by a BSD-style license
// that can be found in the License file.
//
// Author: Shuo Chen (chenshuo at chenshuo dot com)

#ifndef MUDUO_BASE_SINGLETON_H
#define MUDUO_BASE_SINGLETON_H

#include <boost/noncopyable.hpp>
#include <assert.h>
#include <stdlib.h> // atexit
#include <pthread.h>

namespace muduo
{

namespace detail
{
//不能侦测继承的成员函数
// This doesn't detect inherited member functions!
// http://stackoverflow.com/questions/1966362/sfinae-to-check-for-inherited-member-functions
template<typename T>
struct has_no_destroy      
{
  template <typename C> static char test(typeof(&C::no_destroy)); // or decltype in C++11
  template <typename C> static int32_t test(...);
  const static bool value = sizeof(test<T>(0)) == 1;    //判断如果是类的话，是否有no_destroy方法。
};
}

template<typename T>
class Singleton : boost::noncopyable
{
 public:
  static T& instance()   //得到对象
  {
    pthread_once(&ponce_, &Singleton::init);   //第一次调用会在init函数内部创建，pthread_once保证该函数只被调用一次！！！！
    									   //并且pthread_once()能保证线程安全，效率高于mutex
    assert(value_ != NULL);
    return *value_;    //利用pthread_once只构造一次对象
  }

 private:
  Singleton();
  ~Singleton();

  static void init()   //客户端初始化该类
  {
    value_ = new T();   //直接调用构造函数
    if (!detail::has_no_destroy<T>::value)   //当参数是类且没有"no_destroy"方法才会注册atexit的destroy
    {
      ::atexit(destroy);   //登记atexit时调用的销毁函数，防止内存泄漏
    }
  }

  static void destroy()  //程序结束后自动调用该函数销毁
  {
    //用typedef定义了一个数组类型，数组的大小不能为-1，利用这个方法，如果是不完全类型，编译阶段就会发现错误
    typedef char T_must_be_complete_type[sizeof(T) == 0 ? -1 : 1];  //要销毁这个类型，这个类型必须是完全类型
    T_must_be_complete_type dummy; (void) dummy;  //这个

    delete value_;   //销毁
    value_ = NULL;   //赋空
  }

 private:
  static pthread_once_t ponce_;     //pthread_once的参数
  static T*             value_;        //模板T类型的指针
};

template<typename T>
pthread_once_t Singleton<T>::ponce_ = PTHREAD_ONCE_INIT;   //初始化pthread_once

template<typename T>
T* Singleton<T>::value_ = NULL;    //静态成员外部会初始化为空

}
#endif

