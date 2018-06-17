vi// Use of this source code is governed by a BSD-style license
// that can be found in the License file.
//
// Author: Shuo Chen (chenshuo at chenshuo dot com)

#include <muduo/base/ThreadPool.h>

#include <muduo/base/Exception.h>

#include <boost/bind.hpp>
#include <assert.h>
#include <stdio.h>

using namespace muduo;

ThreadPool::ThreadPool(const string& nameArg)
  : mutex_(),
    notEmpty_(mutex_),    //��ʼ����ʱ����Ҫ��condition��mutex��������
    notFull_(mutex_),
    name_(nameArg),
    maxQueueSize_(0),   //��ʼ��0
    running_(false)
{
}

ThreadPool::~ThreadPool()
{
  if (running_)    //����̳߳������У��Ǿ�Ҫ�����ڴ洦����stop()������ִ��
  {
    stop();
  }                  //���û�з�����̣߳��ǾͲ�������Ҫ�ͷŵ��ڴ棬ʲô�������Ϳ�����
}

void ThreadPool::start(int numThreads)
{
  assert(threads_.empty());   //ȷ��δ������
  running_ = true;    //������־
  threads_.reserve(numThreads);   //Ԥ��reserver���ռ�
  for (int i = 0; i < numThreads; ++i)
  {
    char id[32];                  //id�洢�߳�id
    snprintf(id, sizeof id, "%d", i+1);
    threads_.push_back(new muduo::Thread(   //boost::bind�ڰ����ڲ���Աʱ���ڶ����������������ʵ��
          boost::bind(&ThreadPool::runInThread, this), name_+id));//runInThread��ÿ���̵߳��߳����к������߳�Ϊִ����������»�����
    threads_[i].start();    //����ÿ���̣߳����������߳����к�����runInThread�����Ի�������
  }
  if (numThreads == 0 && threadInitCallback_) //����̳߳��߳���Ϊ0���������˻ص�����
  {
    threadInitCallback_();  //init�ص�����
  }
}

void ThreadPool::stop()   //�̳߳�ֹͣ
{
  {
  MutexLockGuard lock(mutex_);   //�ֲ�����
  running_ = false;
  notEmpty_.notifyAll(); //��������notEmpty contition�ϵ������߳�ִ�����
  }
  for_each(threads_.begin(),
           threads_.end(),
           boost::bind(&muduo::Thread::join, _1));   //��ÿ���̵߳��ã�pthread_join(),��ֹ��Դй©
}

size_t ThreadPool::queueSize() const  //thread safe
{
  MutexLockGuard lock(mutex_);
  return queue_.size();
}

//����һ������//����˵�̳߳�����̳߳�ִ�������ǿ�������У��Ͷ���Ҫִ��һ�����񣬱������Ƚ�������push��������У��Ⱥ�����̴߳���
void ThreadPool::run(const Task& task)    
{
  if (threads_.empty())     //����̳߳�Ϊ�գ�˵���̳߳�Ϊ�����߳�
  { 
    task();    //�ɵ�ǰ�߳�ִ��
  }
  else
  {
    MutexLockGuard lock(mutex_);
    while (isFull())    //�������������ʱ��ѭ��
    {
      notFull_.wait();    //һֱ�ȴ�������в���   //�������take()ȡ�������У�ȡ���������δ�������Ѹ���
    }
    assert(!isFull()); 

    queue_.push_back(task);   //��������в������ͰѸ���������̳߳ص��������
    notEmpty_.notify();  //����take()ȡ�����������߳���ȡ����ȡ�������runInThread��ִ������
  }
}

#ifdef __GXX_EXPERIMENTAL_CXX0X__
void ThreadPool::run(Task&& task)
{
  if (threads_.empty())
  {
    task();
  }
  else
  {
    MutexLockGuard lock(mutex_);
    while (isFull())
    {
      notFull_.wait();   
    }
    assert(!isFull());

    queue_.push_back(std::move(task));
    notEmpty_.notify();
  }
}
#endif

//take������ÿ���̶߳�ִ�еģ���Ҫ�����̰߳�ȫ�����Ƕ��߳���ȡ������̰߳�ȫ�ԣ�ֻ�ܴ��л�
ThreadPool::Task ThreadPool::take()     //ȡ������
{
  MutexLockGuard lock(mutex_);   //ע�⣬��������
  // always use a while-loop, due to spurious wakeup     //��ֹ��ȺЧӦ��
  while (queue_.empty() && running_)     //����������Ϊ�գ������̳߳ش�������̬
  {			//����Ϊ��ʱû��ʹ���̳߳�
    notEmpty_.wait();  //�ȴ�������������Ҫ��whileѭ������ֹ��ȺЧӦ��
  } 				   //��Ϊ���е��̶߳��ڵ�ͬһcondition����notempty��ֻ�����߳���wait����ʱ�õ�mutex����������Դ
					  //�����߳���Ȼ��notifyͬ�����أ�����Դ�ѱ����ģ�queueΪ��(��1������Ϊ��)�������߳̾���while�м����ȴ�
  Task task;
  if (!queue_.empty())    //��������ж�ͷ��ȡ����
  {
    task = queue_.front();
    queue_.pop_front();
    if (maxQueueSize_ > 0)    //�������δ���û����0������Ҫ����notFull
    {
      notFull_.notify();    //ȡ��һ������֮����������г��ȴ���0������notfullδ����
    }
  }
  return task;
}

bool ThreadPool::isFull() const   //not thread safe 
{
  mutex_.assertLocked(); //����ȷ����ʹ���߳���ס����ΪisFull��������һ���̰߳�ȫ�ĺ���,�ⲿ����Ҫ����
  return maxQueueSize_ > 0 && queue_.size() >= maxQueueSize_;   //��Ϊdeque��ͨ��push_back�����߳���Ŀ�ģ�����ͨ�����max_queuesize�洢����߳���Ŀ
}

//�߳����к�����������ʱ����������take()��������ʱ����������
void ThreadPool::runInThread()    //�߳����к���
{
  try
  {
    if (threadInitCallback_)
    {
      threadInitCallback_();    //֧��ÿ���߳�����ǰ���Ȼص�����
    }
    while (running_)    //���̳߳ش�������״̬��һֱѭ��
    {
      Task task(take());   //�����������ȡ���������������
      if (task)    //�������ȡ������
      {
        task();    //������
      }
    }
  }
  catch (const Exception& ex)
  {
    fprintf(stderr, "exception caught in ThreadPool %s\n", name_.c_str());
    fprintf(stderr, "reason: %s\n", ex.what());
    fprintf(stderr, "stack trace: %s\n", ex.stackTrace());
    abort();
  }
  catch (const std::exception& ex)
  {
    fprintf(stderr, "exception caught in ThreadPool %s\n", name_.c_str());
    fprintf(stderr, "reason: %s\n", ex.what());
    abort();
  }
  catch (...)
  {
    fprintf(stderr, "unknown exception caught in ThreadPool %s\n", name_.c_str());
    throw; // rethrow
  }
}

