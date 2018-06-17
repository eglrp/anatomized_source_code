#ifndef MUDUO_EXAMPLES_ASIO_CHAT_CODEC_H
#define MUDUO_EXAMPLES_ASIO_CHAT_CODEC_H

#include <muduo/base/Logging.h>
#include <muduo/net/Buffer.h>
#include <muduo/net/Endian.h>
#include <muduo/net/TcpConnection.h>

#include <boost/function.hpp>
#include <boost/noncopyable.hpp>

class LengthHeaderCodec : boost::noncopyable
{
 public:
  typedef boost::function<void (const muduo::net::TcpConnectionPtr&,
                                const muduo::string& message,
                                muduo::Timestamp)> StringMessageCallback;

  explicit LengthHeaderCodec(const StringMessageCallback& cb)//绑定ChatServer的onStringMessage
    : messageCallback_(cb)
  {
  }

  //在该函数中解析消息，是ChatServer首先调用的函数，通过定长包的形式解决了TCP的粘包问题
  void onMessage(const muduo::net::TcpConnectionPtr& conn,
                 muduo::net::Buffer* buf,
                 muduo::Timestamp receiveTime)
  {
    while (buf->readableBytes() >= kHeaderLen) // kHeaderLen == 4 //判断是否超过包头，如果包头都超不过，那半个消息都算不上
    {
      // FIXME: use Buffer::peekInt32()
      const void* data = buf->peek();  //偷看一下readable的当前首地址
      int32_t be32 = *static_cast<const int32_t*>(data); // SIGBUS   //转化成32位
      const int32_t len = muduo::net::sockets::networkToHost32(be32);  //转换成主机字节序
      if (len > 65536 || len < 0)  //如果消息超过64K，或者长度小于0，不合法，干掉它。
      {
        LOG_ERROR << "Invalid length " << len;
        conn->shutdown();  // FIXME: disable reading
        break;
      }
      else if (buf->readableBytes() >= len + kHeaderLen)  //如果缓冲区可读的数据是否>=len+head，说明是一条完整的消息，取走
      {                                                //len是头部规定的体部长度
        buf->retrieve(kHeaderLen);  //取头部
        muduo::string message(buf->peek(), len);  //取包体
        messageCallback_(conn, message, receiveTime);   //取出包体后就可以处理回调了
        buf->retrieve(len);  //然后把字节取走
      }
      else   //未达到一条完整的消息
      {
        break;  
      }
    }
  }

  // FIXME: TcpConnectionPtr
  void send(muduo::net::TcpConnection* conn,
            const muduo::StringPiece& message)
  {
    muduo::net::Buffer buf;
    buf.append(message.data(), message.size());   //先添加数据
    int32_t len = static_cast<int32_t>(message.size());   //长度
    int32_t be32 = muduo::net::sockets::hostToNetwork32(len);  //长度转换成网络字节序
    buf.prepend(&be32, sizeof be32);  //在缓冲区头部增加4个字节
    conn->send(&buf);  //发送
  }

 private:
  StringMessageCallback messageCallback_;
  const static size_t kHeaderLen = sizeof(int32_t);
};

#endif  // MUDUO_EXAMPLES_ASIO_CHAT_CODEC_H
