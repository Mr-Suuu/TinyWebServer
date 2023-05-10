#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>
#include "log.h"
#include <pthread.h>

using namespace std;

Log::Log() {
    m_count = 0;
    m_is_async = false;
}

Log::~Log() {
    if (m_fp != NULL) {
        fclose(m_fp);
    }
}

// 异步需要设置阻塞队列的长度，同步不需要设置
// 写入方式通过初始化时是否设置队列大小（表示在队列中可以放几条数据）来判断，若队列大小为0，则为同步，否则为异步
// 参数有：日志文件、日志缓冲区大小、最大行数以及最长日志条队列
bool Log::init(const char *file_name, int close_log, int log_buf_size, int split_lines, int max_queue_size) {
    // 如果设置了max_queue_size,则设置为异步
    if (max_queue_size >= 1) {
        // 表示异步处理
        m_is_async = true;

        // 创建并设置阻塞队列长度
        m_log_queue = new block_queue<string>(max_queue_size);

        // 创建一个子线程完成异步操作
        pthread_t tid;

        // 第三个参数为：以函数指针的方式指明新建线程需要执行的函数
        // flush_log_thread为回调函数,这里表示创建线程异步写日志
        pthread_create(&tid, NULL, flush_log_thread, NULL);
    }

    m_close_log = close_log;  // 这是？
    // 输出内容的长度
    m_log_buf_size = log_buf_size;
    m_buf = new char[m_log_buf_size];
    // memset：复制'\0'到m_buf的前m_log_buf_size个位置
    memset(m_buf, '\0', m_log_buf_size);

    // 日志的最大行数
    m_split_lines = split_lines;

    time_t t = time(NULL);
    struct tm *sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;

    // 从后往前找到第一个/的位置
    const char *p = strrchr(file_name, '/');
    char log_full_name[256] = {0};

    // 相当于自定义日志名
    // 若输入的文件名没有/，则直接将时间+文件名作为日志名
    if (p == NULL) {
        // snprintf：格式化输出字符串log_full_name，最大长度为255.超过会被截断，格式化格式如后所示
        snprintf(log_full_name, 255, "%d_%02d_%02d_%s", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday,
                 file_name);
    } else {
        // 将/的位置后移一个位置，然后复制到log_name中
        strcpy(log_name, p + 1);
        // p - file_name + 1是文件所在路径文件夹的长度，复制到dir_name中
        strncpy(dir_name, file_name, p - file_name + 1);
        // dir_name 相当于 ./
        snprintf(log_full_name, 255, "%s%d_%02d_%02d_%s", dir_name, my_tm.tm_year + 1900, my_tm.tm_mon + 1,
                 my_tm.tm_mday, log_name);
    }

    m_today = my_tm.tm_mday;

    // 打开log_full_name
    m_fp = fopen(log_full_name, "a");
    if (m_fp == NULL) {
        return false;
    }

    return true;
}

void Log::write_log(int level, const char *format, ...) {
    struct timeval now = {0, 0};
    gettimeofday(&now, NULL);
    time_t t = now.tv_sec;
    struct tm *sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;
    char s[16] = {0};
    // 日志分级
    switch (level) {
        case 0:
            strcpy(s, "[debug]:");
            break;
        case 1:
            strcpy(s, "[info]:");
            break;
        case 2:
            strcpy(s, "[warn]:");
            break;
        case 3:
            strcpy(s, "[erro]:");
            break;
        default:
            strcpy(s, "[info]:");
            break;
    }

    // 写入一个log，对m_count++, m_split_lines最大行数
    m_mutex.lock();
    // 更新现有行数
    m_count++;

    // 日志不是今天 或 写入的日志行数是最大行的倍数(即超出最大行限制)
    // m_split_lines 为最大行数
    if (m_today != my_tm.tm_mday || m_count % m_split_lines == 0) { // everyday log
        char new_log[256] = {0};
        fflush(m_fp);
        fclose(m_fp);
        char tail[16] = {0};

        // 格式化日志名中的时间部分
        snprintf(tail, 16, "%d_%02d_%02d_", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday);

        // 如果是时间不是今天，则创建今天的日志，更新m_today和m_count
        if (m_today != my_tm.tm_mday) {
            snprintf(new_log, 255, "%s%s%s", dir_name, tail, log_name);
            m_today = my_tm.tm_mday;
            m_count = 0;
        } else {
            // 超过了最大行，在之前的日志名基础上加后缀, m_count/m_split_lines
            snprintf(new_log, 255, "%s%s%s.%lld", dir_name, tail, log_name, m_count / m_split_lines);
        }
        m_fp = fopen(new_log, "a");
    }

    m_mutex.unlock();

    va_list valst;
    // 将传入的format参数赋值给valst，便于格式化输出
    va_start(valst, format);

    string log_str;
    m_mutex.lock();

    // 写入内容格式：时间 + 内容
    // 时间格式化，snprintf成功返回写字符的总数，其中不包括结尾的null字符
    int n = snprintf(m_buf, 48, "%d-%02d-%02d %02d:%02d:%02d.%06ld %s ",
                     my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday,
                     my_tm.tm_hour, my_tm.tm_min, my_tm.tm_sec, now.tv_usec, s);

    // 内容格式化，用于向字符串中打印数据、数据格式用户自定义，返回写入到字符数组str中的字符个数(不包含终止符)
    int m = vsnprintf(m_buf + n, m_log_buf_size - n - 1, format, valst);
    m_buf[n + m] = '\n';
    m_buf[n + m + 1] = '\0';
    log_str = m_buf;

    m_mutex.unlock();

    // 若m_is_async为true表示异步，默认为同步
    // 若异步,则将日志信息加入阻塞队列,同步则加锁向文件中写
    if (m_is_async && !m_log_queue->full()) {
        // 异步
        m_log_queue->push(log_str);
    } else {
        // 同步
        /**
         * int fputs(const char *str, FILE *stream);
         *      str，一个数组，包含了要写入的以空字符终止的字符序列
         *      stream，指向FILE对象的指针，该FILE对象标识了要被写入字符串的流
         * **/
        m_mutex.lock();
        fputs(log_str.c_str(), m_fp);
        m_mutex.unlock();
    }

    va_end(valst);
}

void Log::flush(void) {
    m_mutex.lock();
    // 强制刷新写入流缓冲区
    fflush(m_fp);
    m_mutex.unlock();
}
