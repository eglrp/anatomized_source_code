#ifndef MUDUO_BASE_LOGFILE_H
#define MUDUO_BASE_LOGFILE_H

#include <muduo/base/Mutex.h>
#include <muduo/base/Types.h>

#include <boost/noncopyable.hpp>
#include <boost/scoped_ptr.hpp>

namespace muduo
{

namespace FileUtil
{
class AppendFile;
}

class LogFile : boost::noncopyable
{
 public:
  LogFile(const string& basename,
          size_t rollSize,
          bool threadSafe = true,
          int flushInterval = 3,
          int checkEveryN = 1024);   //Ĭ�Ϸָ�����1024
  ~LogFile();

  void append(const char* logline, int len);  //��һ�г���Ϊlen��ӵ���־�ļ���
  void flush();  //ˢ��
  bool rollFile();

 private:
  void append_unlocked(const char* logline, int len); //��������append��ʽ

  static string getLogFileName(const string& basename, time_t* now); //��ȡ��־�ļ�������

  const string basename_;    //��־�ļ�basename
  const size_t rollSize_;        //��־�ļ��ﵽrolsize����һ�����ļ�
  const int flushInterval_;    //��־д����ʱ��
  const int checkEveryN_;

  int count_;  //������������Ƿ���Ҫ�����ļ�

  boost::scoped_ptr<MutexLock> mutex_;  //����
  time_t startOfPeriod_;        //��ʼ��¼��־ʱ�䣨���������ʱ�䣿)
  time_t lastRoll_;                  //��һ�ι�����־�ļ�ʱ��
  time_t lastFlush_;                //��һ����־д���ļ�ʱ��
  boost::scoped_ptr<FileUtil::AppendFile> file_;   //����ָ���ļ�

  const static int kRollPerSeconds_ = 60*60*24;  //��ʱ��һ��
};

}
#endif  // MUDUO_BASE_LOGFILE_H
