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
//�������̳еĳ�Ա����
// This doesn't detect inherited member functions!
// http://stackoverflow.com/questions/1966362/sfinae-to-check-for-inherited-member-functions
template<typename T>
struct has_no_destroy      
{
  template <typename C> static char test(typeof(&C::no_destroy)); // or decltype in C++11
  template <typename C> static int32_t test(...);
  const static bool value = sizeof(test<T>(0)) == 1;    //�ж��������Ļ����Ƿ���no_destroy������
};
}

template<typename T>
class Singleton : boost::noncopyable
{
 public:
  static T& instance()   //�õ�����
  {
    pthread_once(&ponce_, &Singleton::init);   //��һ�ε��û���init�����ڲ�������pthread_once��֤�ú���ֻ������һ�Σ�������
    									   //����pthread_once()�ܱ�֤�̰߳�ȫ��Ч�ʸ���mutex
    assert(value_ != NULL);
    return *value_;    //����pthread_onceֻ����һ�ζ���
  }

 private:
  Singleton();
  ~Singleton();

  static void init()   //�ͻ��˳�ʼ������
  {
    value_ = new T();   //ֱ�ӵ��ù��캯��
    if (!detail::has_no_destroy<T>::value)   //������������û��"no_destroy"�����Ż�ע��atexit��destroy
    {
      ::atexit(destroy);   //�Ǽ�atexitʱ���õ����ٺ�������ֹ�ڴ�й©
    }
  }

  static void destroy()  //����������Զ����øú�������
  {
    //��typedef������һ���������ͣ�����Ĵ�С����Ϊ-1�������������������ǲ���ȫ���ͣ�����׶ξͻᷢ�ִ���
    typedef char T_must_be_complete_type[sizeof(T) == 0 ? -1 : 1];  //Ҫ����������ͣ�������ͱ�������ȫ����
    T_must_be_complete_type dummy; (void) dummy;  //���

    delete value_;   //����
    value_ = NULL;   //����
  }

 private:
  static pthread_once_t ponce_;     //pthread_once�Ĳ���
  static T*             value_;        //ģ��T���͵�ָ��
};

template<typename T>
pthread_once_t Singleton<T>::ponce_ = PTHREAD_ONCE_INIT;   //��ʼ��pthread_once

template<typename T>
T* Singleton<T>::value_ = NULL;    //��̬��Ա�ⲿ���ʼ��Ϊ��

}
#endif

