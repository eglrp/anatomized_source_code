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

  explicit LengthHeaderCodec(const StringMessageCallback& cb)//��ChatServer��onStringMessage
    : messageCallback_(cb)
  {
  }

  //�ڸú����н�����Ϣ����ChatServer���ȵ��õĺ�����ͨ������������ʽ�����TCP��ճ������
  void onMessage(const muduo::net::TcpConnectionPtr& conn,
                 muduo::net::Buffer* buf,
                 muduo::Timestamp receiveTime)
  {
    while (buf->readableBytes() >= kHeaderLen) // kHeaderLen == 4 //�ж��Ƿ񳬹���ͷ�������ͷ�����������ǰ����Ϣ���㲻��
    {
      // FIXME: use Buffer::peekInt32()
      const void* data = buf->peek();  //͵��һ��readable�ĵ�ǰ�׵�ַ
      int32_t be32 = *static_cast<const int32_t*>(data); // SIGBUS   //ת����32λ
      const int32_t len = muduo::net::sockets::networkToHost32(be32);  //ת���������ֽ���
      if (len > 65536 || len < 0)  //�����Ϣ����64K�����߳���С��0�����Ϸ����ɵ�����
      {
        LOG_ERROR << "Invalid length " << len;
        conn->shutdown();  // FIXME: disable reading
        break;
      }
      else if (buf->readableBytes() >= len + kHeaderLen)  //����������ɶ��������Ƿ�>=len+head��˵����һ����������Ϣ��ȡ��
      {                                                //len��ͷ���涨���岿����
        buf->retrieve(kHeaderLen);  //ȡͷ��
        muduo::string message(buf->peek(), len);  //ȡ����
        messageCallback_(conn, message, receiveTime);   //ȡ�������Ϳ��Դ���ص���
        buf->retrieve(len);  //Ȼ����ֽ�ȡ��
      }
      else   //δ�ﵽһ����������Ϣ
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
    buf.append(message.data(), message.size());   //���������
    int32_t len = static_cast<int32_t>(message.size());   //����
    int32_t be32 = muduo::net::sockets::hostToNetwork32(len);  //����ת���������ֽ���
    buf.prepend(&be32, sizeof be32);  //�ڻ�����ͷ������4���ֽ�
    conn->send(&buf);  //����
  }

 private:
  StringMessageCallback messageCallback_;
  const static size_t kHeaderLen = sizeof(int32_t);
};

#endif  // MUDUO_EXAMPLES_ASIO_CHAT_CODEC_H
