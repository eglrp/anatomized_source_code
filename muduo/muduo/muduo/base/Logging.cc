#include <muduo/base/Logging.h>

#include <muduo/base/CurrentThread.h>
#include <muduo/base/Timestamp.h>
#include <muduo/base/TimeZone.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <sstream>

namespace muduo
{

/*
class LoggerImpl
{
 public:
  typedef Logger::LogLevel LogLevel;
  LoggerImpl(LogLevel level, int old_errno, const char* file, int line);
  void finish();

  Timestamp time_;
  LogStream stream_;
  LogLevel level_;
  int line_;
  const char* fullname_;
  const char* basename_;
};
*/

__thread char t_errnobuf[512];
__thread char t_time[32];
__thread time_t t_lastSecond;

const char* strerror_tl(int savedErrno)
{
  return strerror_r(savedErrno, t_errnobuf, sizeof t_errnobuf);
}

Logger::LogLevel initLogLevel()    //初始化日志别别
{
  if (::getenv("MUDUO_LOG_TRACE"))    //获取TRACE环境变量，如果有，返回它
    return Logger::TRACE;
  else if (::getenv("MUDUO_LOG_DEBUG"))  //获取DEBUG环境变量，如果有，返回它
    return Logger::DEBUG;
  else
    return Logger::INFO;  //如果它们都没有，就使用INFO级别
}

Logger::LogLevel g_logLevel = initLogLevel();   //初始化日志级别

const char* LogLevelName[Logger::NUM_LOG_LEVELS] =
{
  "TRACE ",
  "DEBUG ",
  "INFO  ",
  "WARN  ",
  "ERROR ",
  "FATAL ",
};

// helper class for known string length at compile time
class T   //编译时获取字符串长度的类
{
 public:
  T(const char* str, unsigned len)
    :str_(str),
     len_(len)
  {
    assert(strlen(str) == len_);
  }

  const char* str_;
  const unsigned len_;
};

inline LogStream& operator<<(LogStream& s, T v)
{
  s.append(v.str_, v.len_);   //LogStream的重载，输出
  return s;
}

inline LogStream& operator<<(LogStream& s, const Logger::SourceFile& v)
{
  s.append(v.data_, v.size_);
  return s;
}

void defaultOutput(const char* msg, int len)  
{
  size_t n = fwrite(msg, 1, len, stdout);    //默认输出内容到stdout
  //FIXME check n
  (void)n;
}

void defaultFlush()   //默认刷新stdout
{
  fflush(stdout);
}

Logger::OutputFunc g_output = defaultOutput;   //默认输出方法
Logger::FlushFunc g_flush = defaultFlush;   //默认刷新方法
TimeZone g_logTimeZone;   

}

using namespace muduo;
                                                          //错误码，没有就传0
 Logger::Impl::Impl(LogLevel level, int savedErrno, const SourceFile& file, int line)
  : time_(Timestamp::now()),   //当前时间
    stream_(),      //初始化logger的四个成员
    level_(level),
    line_(line),
    basename_(file)
{
  formatTime();          //格式化时间，缓存当前线程id
  CurrentThread::tid();  //缓存当前线程id
  stream_ << T(CurrentThread::tidString(), CurrentThread::tidStringLength());   //格式化线程tid字符串
  stream_ << T(LogLevelName[level], 6);   //格式化级别，对应成字符串，先输出到缓冲区
  if (savedErrno != 0)
  {
    stream_ << strerror_tl(savedErrno) << " (errno=" << savedErrno << ") ";  //如果错误码不为0，还要输出相对应信息
  }
}

void Logger::Impl::formatTime()   //格式化时间
{
  int64_t microSecondsSinceEpoch = time_.microSecondsSinceEpoch();
  time_t seconds = static_cast<time_t>(microSecondsSinceEpoch / Timestamp::kMicroSecondsPerSecond);
  int microseconds = static_cast<int>(microSecondsSinceEpoch % Timestamp::kMicroSecondsPerSecond);
  if (seconds != t_lastSecond)
  {
    t_lastSecond = seconds;
    struct tm tm_time;
    if (g_logTimeZone.valid())
    {
      tm_time = g_logTimeZone.toLocalTime(seconds);
    }
    else
    {
      ::gmtime_r(&seconds, &tm_time); // FIXME TimeZone::fromUtcTime
    }

    int len = snprintf(t_time, sizeof(t_time), "%4d%02d%02d %02d:%02d:%02d",
        tm_time.tm_year + 1900, tm_time.tm_mon + 1, tm_time.tm_mday,
        tm_time.tm_hour, tm_time.tm_min, tm_time.tm_sec);
    assert(len == 17); (void)len;
  }

  if (g_logTimeZone.valid())
  {
    Fmt us(".%06d ", microseconds);   //格式化
    assert(us.length() == 8);
    stream_ << T(t_time, 17) << T(us.data(), 8);
  }
  else
  {
    Fmt us(".%06dZ ", microseconds);
    assert(us.length() == 9);
    stream_ << T(t_time, 17) << T(us.data(), 9);    //用stream进行输出，重载了<<
  }
}

void Logger::Impl::finish()   //首先将名字行输进缓冲区
{
  stream_ << " - " << basename_ << ':' << line_ << '\n';
}

Logger::Logger(SourceFile file, int line)
  : impl_(INFO, 0, file, line)
{
}

Logger::Logger(SourceFile file, int line, LogLevel level, const char* func)
  : impl_(level, 0, file, line)
{
  impl_.stream_ << func << ' ';   //格式化函数名称，上面的构造函数没有函数名称，不同的构造函数
}

Logger::Logger(SourceFile file, int line, LogLevel level) //同样格式化这三个参数
  : impl_(level, 0, file, line)
{
}

Logger::Logger(SourceFile file, int line, bool toAbort)  //是否终止
  : impl_(toAbort?FATAL:ERROR, errno, file, line)
{
}

//析构函数中会调用impl_的finish方法
Logger::~Logger()
{
  impl_.finish();   //将名字行数输入缓冲区
  const LogStream::Buffer& buf(stream().buffer());   //将缓冲区以引用方式获得
  g_output(buf.data(), buf.length());    //调用全部输出方法，输出缓冲区内容，默认是输出到stdout
  if (impl_.level_ == FATAL)
  {
    g_flush();
    abort();
  }
}

void Logger::setLogLevel(Logger::LogLevel level)  //设置日志级别
{
  g_logLevel = level;
}

void Logger::setOutput(OutputFunc out)  //设置输出函数，用来替代默认的
{
  g_output = out;
}

void Logger::setFlush(FlushFunc flush)  //用来配套你设置的输出函数的刷新方法
{
  g_flush = flush;
}

void Logger::setTimeZone(const TimeZone& tz)
{
  g_logTimeZone = tz;
}
