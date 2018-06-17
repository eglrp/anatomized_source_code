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

Logger::LogLevel initLogLevel()    //��ʼ����־���
{
  if (::getenv("MUDUO_LOG_TRACE"))    //��ȡTRACE��������������У�������
    return Logger::TRACE;
  else if (::getenv("MUDUO_LOG_DEBUG"))  //��ȡDEBUG��������������У�������
    return Logger::DEBUG;
  else
    return Logger::INFO;  //������Ƕ�û�У���ʹ��INFO����
}

Logger::LogLevel g_logLevel = initLogLevel();   //��ʼ����־����

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
class T   //����ʱ��ȡ�ַ������ȵ���
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
  s.append(v.str_, v.len_);   //LogStream�����أ����
  return s;
}

inline LogStream& operator<<(LogStream& s, const Logger::SourceFile& v)
{
  s.append(v.data_, v.size_);
  return s;
}

void defaultOutput(const char* msg, int len)  
{
  size_t n = fwrite(msg, 1, len, stdout);    //Ĭ��������ݵ�stdout
  //FIXME check n
  (void)n;
}

void defaultFlush()   //Ĭ��ˢ��stdout
{
  fflush(stdout);
}

Logger::OutputFunc g_output = defaultOutput;   //Ĭ���������
Logger::FlushFunc g_flush = defaultFlush;   //Ĭ��ˢ�·���
TimeZone g_logTimeZone;   

}

using namespace muduo;
                                                          //�����룬û�оʹ�0
 Logger::Impl::Impl(LogLevel level, int savedErrno, const SourceFile& file, int line)
  : time_(Timestamp::now()),   //��ǰʱ��
    stream_(),      //��ʼ��logger���ĸ���Ա
    level_(level),
    line_(line),
    basename_(file)
{
  formatTime();          //��ʽ��ʱ�䣬���浱ǰ�߳�id
  CurrentThread::tid();  //���浱ǰ�߳�id
  stream_ << T(CurrentThread::tidString(), CurrentThread::tidStringLength());   //��ʽ���߳�tid�ַ���
  stream_ << T(LogLevelName[level], 6);   //��ʽ�����𣬶�Ӧ���ַ������������������
  if (savedErrno != 0)
  {
    stream_ << strerror_tl(savedErrno) << " (errno=" << savedErrno << ") ";  //��������벻Ϊ0����Ҫ������Ӧ��Ϣ
  }
}

void Logger::Impl::formatTime()   //��ʽ��ʱ��
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
    Fmt us(".%06d ", microseconds);   //��ʽ��
    assert(us.length() == 8);
    stream_ << T(t_time, 17) << T(us.data(), 8);
  }
  else
  {
    Fmt us(".%06dZ ", microseconds);
    assert(us.length() == 9);
    stream_ << T(t_time, 17) << T(us.data(), 9);    //��stream���������������<<
  }
}

void Logger::Impl::finish()   //���Ƚ����������������
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
  impl_.stream_ << func << ' ';   //��ʽ���������ƣ�����Ĺ��캯��û�к������ƣ���ͬ�Ĺ��캯��
}

Logger::Logger(SourceFile file, int line, LogLevel level) //ͬ����ʽ������������
  : impl_(level, 0, file, line)
{
}

Logger::Logger(SourceFile file, int line, bool toAbort)  //�Ƿ���ֹ
  : impl_(toAbort?FATAL:ERROR, errno, file, line)
{
}

//���������л����impl_��finish����
Logger::~Logger()
{
  impl_.finish();   //�������������뻺����
  const LogStream::Buffer& buf(stream().buffer());   //�������������÷�ʽ���
  g_output(buf.data(), buf.length());    //����ȫ�����������������������ݣ�Ĭ���������stdout
  if (impl_.level_ == FATAL)
  {
    g_flush();
    abort();
  }
}

void Logger::setLogLevel(Logger::LogLevel level)  //������־����
{
  g_logLevel = level;
}

void Logger::setOutput(OutputFunc out)  //��������������������Ĭ�ϵ�
{
  g_output = out;
}

void Logger::setFlush(FlushFunc flush)  //�������������õ����������ˢ�·���
{
  g_flush = flush;
}

void Logger::setTimeZone(const TimeZone& tz)
{
  g_logTimeZone = tz;
}
