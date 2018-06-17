#ifndef MUDUO_BASE_LOGSTREAM_H
#define MUDUO_BASE_LOGSTREAM_H

#include <muduo/base/StringPiece.h>
#include <muduo/base/Types.h>
#include <assert.h>
#include <string.h> // memcpy
#ifndef MUDUO_STD_STRING
#include <string>
#endif
#include <boost/noncopyable.hpp>

namespace muduo
{

namespace detail
{

//»º³åÇø´óĞ¡µÄÅäÖÃ
const int kSmallBuffer = 4000;
const int kLargeBuffer = 4000*1000;

template<int SIZE>
class FixedBuffer : boost::noncopyable
{
 public:
  FixedBuffer()
    : cur_(data_)
  {
    setCookie(cookieStart);  //ÉèÖÃcookie£¬muduo¿âÕâ¸öº¯ÊıÄ¿Ç°»¹Ã»¼ÓÈë¹¦ÄÜ£¬ËùÒÔ¿ÉÒÔ²»ÓÃ¹Ü
  }

  ~FixedBuffer()
  {
    setCookie(cookieEnd);
  }

  void append(const char* /*restrict*/ buf, size_t len)  //Ìí¼ÓÊı¾İ
  {
    // FIXME: append partially
    if (implicit_cast<size_t>(avail()) > len)  //Èç¹û¿ÉÓÃÊı¾İ×ã¹»£¬¾Í¿½±´¹ıÈ¥£¬Í¬Ê±ÒÆ¶¯µ±Ç°Ö¸Õë¡£
    {
      memcpy(cur_, buf, len);
      cur_ += len;
    }
  }

  const char* data() const { return data_; }  //·µ»ØÊ×µØÖ·
  int length() const { return static_cast<int>(cur_ - data_); }   //·µ»Ø»º³åÇøÒÑÓĞÊı¾İ³¤¶È

  // write to data_ directly
  char* current() { return cur_; }  //·µ»Øµ±Ç°Êı¾İÄ©¶ËµØÖ·
  int avail() const { return static_cast<int>(end() - cur_); }  //·µ»ØÊ£Óà¿ÉÓÃµØÖ·
  void add(size_t len) { cur_ += len; }  //curÇ°ÒÆ

  void reset() { cur_ = data_; }   //ÖØÖÃ£¬²»ÇåÊı¾İ£¬Ö»ĞèÒªÈÃcurÖ¸»ØÊ×µØÖ·¼´¿É
  void bzero() { ::bzero(data_, sizeof data_); }  //ÇåÁã
  
  // for used by GDB
  const char* debugString();
  void setCookie(void (*cookie)()) { cookie_ = cookie; }
  // for used by unit test
  string toString() const { return string(data_, length()); }
  StringPiece toStringPiece() const { return StringPiece(data_, length()); }  //·µ»ØstringÀàĞÍ

 private:
  const char* end() const { return data_ + sizeof data_; } //·µ»ØÎ²Ö¸Õë
  // Must be outline function for cookies.
  static void cookieStart();
  static void cookieEnd();

  void (*cookie_)();
  char data_[SIZE];   //»º³åÇøÊı×é
  char* cur_;      //curÓÀÔ¶Ö¸ÏòÒÑÓĞÊı¾İµÄ×îÓÒ¶Ë£¬data->cur->end½á¹¹£
};

}

class LogStream : boost::noncopyable
{
  typedef LogStream self;
 public:
  typedef detail::FixedBuffer<detail::kSmallBuffer> Buffer;  //»º³åÇø£¬Ê¹ÓÃsmallbuffer

  //Õë¶Ô²»Í¬ÀàĞÍÖØÔØÁËoperator<<
  self& operator<<(bool v)   //±ğµÄÀàÈç¹ûµ÷ÓÃLogStreamµÄ<<Êµ¼ÊÉÏÊÇ°ÑÄÚÈİ×·¼Óµ½LogStreamµÄ»º³åÇø¡£
  {
    buffer_.append(v ? "1" : "0", 1);  //×·¼Ó
    return *this;
  }
  self& operator<<(short);
  self& operator<<(unsigned short);
  self& operator<<(int);
  self& operator<<(unsigned int);
  self& operator<<(long);
  self& operator<<(unsigned long);
  self& operator<<(long long);
  self& operator<<(unsigned long long);

  self& operator<<(const void*);

  self& operator<<(float v)
  {
    *this << static_cast<double>(v);    //°ÑfloatÀàĞÍ×ª»¯ÎªdoubleÀàĞÍ£¬µ÷ÓÃÏÂÃæµÄÖØÔØº¯Êı
    return *this;
  }
  self& operator<<(double);
  // self& operator<<(long double);

  self& operator<<(char v)
  {
    buffer_.append(&v, 1);
    return *this;
  }

  // self& operator<<(signed char);
  // self& operator<<(unsigned char);

  self& operator<<(const char* str)
  {
    if (str)
    {
      buffer_.append(str, strlen(str));
    }
    else
    {
      buffer_.append("(null)", 6);
    }
    return *this;
  }

  self& operator<<(const unsigned char* str)
  {
    return operator<<(reinterpret_cast<const char*>(str));
  }

  self& operator<<(const string& v)
  {
    buffer_.append(v.c_str(), v.size());
    return *this;
  }

#ifndef MUDUO_STD_STRING
  self& operator<<(const std::string& v)
  {
    buffer_.append(v.c_str(), v.size());
    return *this;
  }
#endif

  self& operator<<(const StringPiece& v)
  {
    buffer_.append(v.data(), v.size());
    return *this;
  }

  self& operator<<(const Buffer& v)
  {
    *this << v.toStringPiece();
    return *this;
  }

  void append(const char* data, int len) { buffer_.append(data, len); }
  const Buffer& buffer() const { return buffer_; }
  void resetBuffer() { buffer_.reset(); }

 private:
  void staticCheck();

  template<typename T>
  void formatInteger(T);

  Buffer buffer_;

  static const int kMaxNumericSize = 32;
};

class Fmt // : boost::noncopyable
{
 public:
  template<typename T>
  Fmt(const char* fmt, T val);   //°ÑÕûÊı°´ÕÕTÀàĞÍ¸ñÊ½»¯µ½bufferÖĞ

  const char* data() const { return buf_; }
  int length() const { return length_; }  
 private:
  char buf_[32];
  int length_;
};

inline LogStream& operator<<(LogStream& s, const Fmt& fmt)
{
  s.append(fmt.data(), fmt.length());
  return s;
}

}
#endif  // MUDUO_BASE_LOGSTREAM_H

