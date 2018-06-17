#include "codec.h"

#include <muduo/base/Logging.h>
#include <muduo/base/Mutex.h>
#include <muduo/base/ThreadLocalSingleton.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/TcpServer.h>

#include <boost/bind.hpp>
#include <boost/shared_ptr.hpp>

#include <set>
#include <stdio.h>

using namespace muduo;
using namespace muduo::net;

class ChatServer : boost::noncopyable
{
 public:
  ChatServer(EventLoop* loop,
             const InetAddress& listenAddr)
  : server_(loop, listenAddr, "ChatServer"),
    codec_(boost::bind(&ChatServer::onStringMessage, this, _1, _2, _3))
  {
    server_.setConnectionCallback(
        boost::bind(&ChatServer::onConnection, this, _1));
    server_.setMessageCallback(
        boost::bind(&LengthHeaderCodec::onMessage, &codec_, _1, _2, _3));
  }

  void setThreadNum(int numThreads)
  {
    server_.setThreadNum(numThreads);
  }

  void start()
  {
    //����ÿ���߳�����ǰ�Ļص��������ú����л�Ϊÿ���߳������ֲ߳̾�ConnectionsListʵ��
    server_.setThreadInitCallback(boost::bind(&ChatServer::threadInit, this, _1));
    server_.start();
  }

 private:
  void onConnection(const TcpConnectionPtr& conn)
  {
    LOG_INFO << conn->localAddress().toIpPort() << " -> "
             << conn->peerAddress().toIpPort() << " is "
             << (conn->connected() ? "UP" : "DOWN");
    //����Ҫ�������õ���ÿ���ֲ߳̾�ʵ��ConnectionsList
    if (conn->connected())
    {
      LocalConnections::instance().insert(conn);
    }
    else
    {
      LocalConnections::instance().erase(conn);
    }
  }

  void onStringMessage(const TcpConnectionPtr&,
                       const string& message,
                       Timestamp)
  {
     //distribuMessage��������ת����Ϣ�����������ָ�I/O�̣߳�������mutex��������ֻ����f�����I/O�̣߳���ת����Ϣ����С���ٽ�������
    EventLoop::Functor f = boost::bind(&ChatServer::distributeMessage, this, message);
    LOG_DEBUG;

    MutexLockGuard lock(mutex_); 
    //���������ٽ�����û��ת����Ϣ��
    //ת����Ϣ�����пͻ��ˣ���Чת��(���߳�ת��),ͨ�������ͻ������ڵ�I/O�߳�
    for (std::set<EventLoop*>::iterator it = loops_.begin();
        it != loops_.end();
        ++it)
    {
      // 1.�ö�Ӧ��I/O�߳���ִ��distributeMessage
      // 2.distributeMessage�ŵ�I/O������ִ�У���ˣ������mutex_����������С
      // 3. distributeMessage����mutex��������Ϊ����TLS
      (*it)->queueInLoop(f);
    }
    LOG_DEBUG;
  }

  typedef std::set<TcpConnectionPtr> ConnectionList;

  void distributeMessage(const string& message)
  {
    LOG_DEBUG << "begin";
    //connections_��TLS���������Բ���Ҫ����
    for (ConnectionList::iterator it = LocalConnections::instance().begin();
        it != LocalConnections::instance().end();
        ++it)
    {
      codec_.send(get_pointer(*it), message);  //������Ϣ
    }
    LOG_DEBUG << "end";
  }

  //�̵߳���֮ǰ���õĻص�����
  void threadInit(EventLoop* loop)
  {
    assert(LocalConnections::pointer() == NULL);

    //�ڴ������ֲ߳̾���������
    LocalConnections::instance();
    assert(LocalConnections::pointer() != NULL);
    MutexLockGuard lock(mutex_);
    loops_.insert(loop);  //����loop��loops_�б���
  }

  TcpServer server_;
  LengthHeaderCodec codec_;
  typedef ThreadLocalSingleton<ConnectionList> LocalConnections;//�ֲ߳̾���������

  MutexLock mutex_;
  std::set<EventLoop*> loops_;
};

int main(int argc, char* argv[])
{
  LOG_INFO << "pid = " << getpid();
  if (argc > 1)
  {
    EventLoop loop;
    uint16_t port = static_cast<uint16_t>(atoi(argv[1]));
    InetAddress serverAddr(port);
    ChatServer server(&loop, serverAddr);
    if (argc > 2)
    {
      server.setThreadNum(atoi(argv[2]));
    }
    server.start();
    loop.loop();
  }
  else
  {
    printf("Usage: %s port [thread_num]\n", argv[0]);
  }
}


