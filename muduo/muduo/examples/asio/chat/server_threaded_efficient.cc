#include "codec.h"

#include <muduo/base/Logging.h>
#include <muduo/base/Mutex.h>
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
    codec_(boost::bind(&ChatServer::onStringMessage, this, _1, _2, _3)),
    connections_(new ConnectionList)  //new出来，shared_ptr引用计数为1
  {
    server_.setConnectionCallback(
        boost::bind(&ChatServer::onConnection, this, _1));//写操作
    server_.setMessageCallback(
        boost::bind(&LengthHeaderCodec::onMessage, &codec_, _1, _2, _3));  //读操作
  }

  void setThreadNum(int numThreads)
  {
    server_.setThreadNum(numThreads);
  }

  void start()
  {
    server_.start();
  }

 private:
  void onConnection(const TcpConnectionPtr& conn)
  {
    LOG_INFO << conn->localAddress().toIpPort() << " -> "
        << conn->peerAddress().toIpPort() << " is "
        << (conn->connected() ? "UP" : "DOWN");

    MutexLockGuard lock(mutex_);
    if (!connections_.unique())  //说明引用计数大于1，存在其他线程正在读
    {
      //new ConnectionList(*connections_) 这段代码拷贝了一份 ConnectionList
      //新connections_的引用计数为1，原来的connections_的引用计数减1，因为reset了
      connections_.reset(new ConnectionList(*connections_));
    }
    assert(connections_.unique());

    //在副本上修改，不会影响读者，所以读者在遍历列表的时候，不需要mutex保护
    if (conn->connected())
    {
      connections_->insert(conn);
    }
    else
    {
      connections_->erase(conn);
    }
  }

  typedef std::set<TcpConnectionPtr> ConnectionList;
  typedef boost::shared_ptr<ConnectionList> ConnectionListPtr;

  void onStringMessage(const TcpConnectionPtr&,
                       const string& message,
                       Timestamp)
  {
    //引用计数加1，mutex保护的临界区大大缩短
    ConnectionListPtr connections = getConnectionList();  //临界区仅为getConnectionList内部
    //可能大家会有疑问，不受mutex保护，写者更改了连接列表怎么办?
    //实际上，写者是在另一个副本上修改，所以无需担心。
    for (ConnectionList::iterator it = connections->begin();
        it != connections->end();
        ++it)
    {
      codec_.send(get_pointer(*it), message);
    }
    //当connections这个栈上变量销毁的时候，引用计数减1
    //如果connections在本函数前面获得智能指针后引用计数为2(一个connections和一个connextions_)，写者会采取reset使引用计数减1，
    //再加上在本函数结束时引用计数减1，所以旧的connections_会销毁，写者reset的新的connections_成为"正宗"。
    //也就是说 assert(!connections.unique())，这个断言在此处不一定成立。
  }

  //mutex只保护这一段临界区
  ConnectionListPtr getConnectionList()
  {
    MutexLockGuard lock(mutex_);
    return connections_;
  }

  TcpServer server_;
  LengthHeaderCodec codec_;
  MutexLock mutex_;
  ConnectionListPtr connections_;  //连接的集合
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

