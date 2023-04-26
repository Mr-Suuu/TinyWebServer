#include "http_conn.h"

#include <mysql/mysql.h>
#include <fstream>

// 定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

locker m_lock;
// 用户名和密码
map <string, string> users;

void http_conn::initmysql_result(connection_pool *connPool) {
    // 先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    // 在user表中检索username，passwd数据，浏览器端输入
    //  mysql_query 传入第一个是MYSQL对象，第二个是查询语句
    if (mysql_query(mysql, "SELECT username,passwd FROM user")) {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    // 从表中检索完整的结果集
    //  mysql_store_result(&mysql)告诉句柄mysql，把查询的数据从服务器端取到客户端，然后缓存起来，放在句柄mysql里面
    //  这里将结果通过result指针指出
    MYSQL_RES *result = mysql_store_result(mysql);

    // 返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    // 返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    // 从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result)) {
        // 取出查询结果的每一行，第一个作为用户名，第二个作为密码，并以键值对的形式保存
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

// 对文件描述符设置非阻塞
int setnonblocking(int fd) {
    // F_GETFD获得文件描述符
    int old_option = fcntl(fd, F_GETFL);
    // 设置为非阻塞
    int new_option = old_option | O_NONBLOCK;
    // 设置文件描述符
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

// 将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode) {
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode) {
        // ET边缘触发模式
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    } else {
        // LT水平触发模式
        event.events = EPOLLIN | EPOLLRDHUP;
    }

    if (one_shot) {
        // 判断是否开启EPOLLONESHOT
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);  // 设置为非阻塞
}

// 从内核时间表删除描述符
void removefd(int epollfd, int fd) {
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);  // 关闭套接字
}

// 将事件重置为EPOLLONESHOT
void modfd(int epollfd, int fd, int ev, int TRIGMode) {
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode) {
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    } else {
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
    }
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

// 关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close) {
    if (real_close && (m_sockfd != -1)) {
        printf("close %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

// 初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode, int close_log, string user,
                     string passwd, string sqlname) {
    m_sockfd = sockfd;
    m_address = addr;

    addfd(m_epollfd, sockfd, true, m_TRIGMode);
    m_user_count++;

    // 当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    doc_root = root;
    m_TRIGMode = TRIGMode;
    m_close_log = close_log;

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    init();
}

// 初始化新接受的连接
// check_state默认为分析请求行状态
void http_conn::init() {
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    m_state = 0;
    timer_flag = 0;
    improv = 0;

    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

// 从状态机，用于分析出一行内容
// 返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN

// m_read_idx指向缓冲区m_read_buf的数据末尾的下一个字节
// m_checked_idx指向从状态机当前正在分析的字节
http_conn::LINE_STATUS http_conn::parse_line() {
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx) {
        // temp为将要分析的字节
        temp = m_read_buf[m_checked_idx];

        // 若当前是'\r'字符，则有可能会读取到完整行
        if (temp == '\r') {
            if ((m_checked_idx + 1) == m_read_idx) {
                // 下一个字符达到了buffer结尾，则接受不完整，需要继续接收
                return LINE_OPEN;
            } else if (m_read_buf[m_checked_idx + 1] == '\n') {
                // 下一个字符是'\n'，将'\r\n'改为'\0\0'
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            // 若都不符合，则返回语法错误
            return LINE_BAD;
        } else if (temp == '\n') {
            // 若当前字符是'\n'，也有可能读取到完整行
            // 一般是上次读取到'\r'就到buffer结尾，没有接收完整，再次接受时会出现这种情况
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r') {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    // 若没有找到'\r\n'，则继续接收
    return LINE_OPEN;
}

// 循环读取客户数据，直到无数据可读或对方关闭连接
// 非阻塞ET工作模式下，需要一次性将数据读完
bool http_conn::read_once() {
    if (m_read_idx >= READ_BUFFER_SIZE) {
        // 若超出范围则报错
        return false;
    }
    int bytes_read = 0;

    // LT（水平触发模式）读取数据
    if (0 == m_TRIGMode) {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read;

        if (bytes_read <= 0) {
            return false;
        }

        return true;
    }
        // ET（边缘触发模式）读数据
    else {
        while (true) {
            /**
             * int recv( SOCKET s, char FAR *buf, int len, int flags);
             * 第一个参数指定接收端套接字描述符
             * 第二个参数指明一个缓冲区，该缓冲区用来存放recv函数接收到的数据
             * 第三个参数指明buf的长度
             * 第四个参数一般置0
             *
             * 成功执行时，返回接收到的字节数
             * 另一端已关闭则返回0
             * 失败返回-1
             * **/
            // 从套接字接收数据，存储在m_read_buf缓冲区
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1) {
                // recv失败
                // 非阻塞ET模式下，需要一次性将数据读完
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // 当errno为EAGAIN或EWOULDBLOCK时，表明读取完毕
                    break;
                }
                return false;
            } else if (bytes_read == 0) {
                // 另一端已关闭
                return false;
            }
            // 修改缓冲区最后一个位置，即m_read_idx的读取字节数
            m_read_idx += bytes_read;
        }
        return true;
    }
}

// 解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text) {
    // 在HTTP报文中，请求行用来说明请求类型,要访问的资源以及所使用的HTTP版本，其中各个部分之间通过'\t'或空格分隔
    // 请求行中最先含有空格和'\t'任一字符的位置并返回
    // strpbrk返回 str1 中第一个匹配字符串 str2 中字符的字符下标，若没有则返回null
    m_url = strpbrk(text, " \t");
    // 如果没有空格或'\t'，则报文格式有误
    if (!m_url) {
        return BAD_REQUEST;
    }

    // 将该位置改为'\0'，用于将前面数据取出
    *m_url++ = '\0';

    // 取出数据，并通过与GET和POST比较，以确定请求方式
    char *method = text;
    // 判断是否包含GET或POST
    if (strcasecmp(method, "GET") == 0) {
        m_method = GET;
    } else if (strcasecmp(method, "POST") == 0) {
        m_method = POST;
        cgi = 1;  // 设置标志位
    } else {
        return BAD_REQUEST;
    }
    // m_url此时跳过了第一个空格或'\t'字符，但不知道之后是否还有
    // 将m_url向后偏移，通过查找，继续跳过空格和'\t'字符，指向请求资源的第一个字符
    m_url += strspn(m_url, " \t");

    // 使用与判断请求方式的相同逻辑，判断HTTP版本号
    m_version = strpbrk(m_url, " \t");
    if (!m_version) {
        return BAD_REQUEST;
    }
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");

    // 仅支持HTTP/1.1
    if (strcasecmp(m_version, "HTTP/1.1") != 0) {
        return BAD_REQUEST;
    }

    // 对请求资源前7个字符进行判断
    // 这里主要是有些报文的请求资源中会带有"http://"，这里需要对这种情况进行单独处理
    if (strncasecmp(m_url, "http://", 7) == 0) {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }

    // 同样增加https情况
    if (strncasecmp(m_url, "https://", 8) == 0) {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }

    // 一般的不会带有上述两种符号，直接是单独的'/'或'/'后面带访问资源
    if (!m_url || m_url[0] != '/') {
        return BAD_REQUEST;
    }

    // 当url为'/'时，显示判断界面
    if (strlen(m_url) == 1) {
        strcat(m_url, "judge.html");
    }

    // 请求行处理完毕，将主状态机转移处理请求头
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

// 解析http请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text) {
    // 判断是空行还是请求头
    if (text[0] == '\0') {
        // 是空行
        // 判断是GET(==0)还是POST(!=0)请求
        if (m_content_length != 0) {
            // POST需要跳转到消息体处理状态
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        // 若是GET请求，则报文解析结束
        // GET请求没有消息体，当解析完空行之后，便完成了报文的解析
        return GET_REQUEST;
    } else if (strncasecmp(text, "Connection:", 11) == 0) {
        // strncasecmp：比较两个字符串前11位字符
        // 非空行
        // 解析请求头连接字段
        text += 11;
        // 跳过空格和'\t'字符
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0) {
            // 如果是长连接，则将linger标志设置为true
            m_linger = true;
        }
    } else if (strncasecmp(text, "Content-length:", 15) == 0) {
        // 解析请求头部内容长度字段
        text += 15;
        // 跳过空格和'\t'字符
        text += strspn(text, " \t");
        m_content_length = atol(text);
    } else if (strncasecmp(text, "Host:", 5) == 0) {
        // 解析请求头部HOST字段
        text += 5;
        // 跳过空格和'\t'字符
        text += strspn(text, " \t");
        m_host = text;
    } else {
        LOG_INFO("oop!unknow header: %s", text);
    }
    return NO_REQUEST;
}

// 判断http请求是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(char *text) {
    // m_read_idx 表示读取到的位置
    // 判断buffer中是否读取了消息体
    if (m_read_idx >= (m_content_length + m_checked_idx)) {
        // 若读到结尾
        text[m_content_length] = '\0';
        // POST请求中最后为输入的用户名和密码
        // 将报文赋值给m_string
        m_string = text;     // 存储请求头数据
        return GET_REQUEST;  // 表示完整读入
    }
    return NO_REQUEST;  // 表示完整读入失败
}

http_conn::HTTP_CODE http_conn::process_read() {
    // 初始化从状态机状态、HTTP请求解析结果
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    // 在GET请求报文中，每一行都是以'\r\n'作为结束的，所以仅用从状态机的状态(line_status = parse_line()) == LINE_OK)判断即可
    // POST请求报文中，消息体的末尾没有任何字符，所以不能使用从状态机的状态，所以使用主状态机的状态CHECK_STATE_CONTENT作为循环入口条件
    // 但是POST报文解析完成后，主状态机的状态还是CHECK_STATE_CONTENT，因此应在完成消息体解析后，将line_status变量改为LINE_OPEN，从而跳出循环
    // parse_line为从状态机的具体实现
    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) ||
           ((line_status = parse_line()) == LINE_OK)) {
        text = get_line();  // get_line用于将指针向后偏移，指向未处理的字符

        // m_start_line是每一个数据行在m_read_buf中的起始位置
        // m_checked_idx表示从状态机在m_read_buf中读取的位置
        m_start_line = m_checked_idx;
        LOG_INFO("%s", text);

        // 主状态机的三种状态转移逻辑
        switch (m_check_state) {
            case CHECK_STATE_REQUESTLINE: {
                // 解析请求行
                ret = parse_request_line(text);
                if (ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER: {
                // 解析请求头
                ret = parse_headers(text);
                if (ret == BAD_REQUEST) {
                    // 读取出错
                    return BAD_REQUEST;
                } else if (ret == GET_REQUEST) {
                    // 完整解析GET请求后，跳转到报文响应函数
                    return do_request();
                }
                break;
            }
            case CHECK_STATE_CONTENT: {
                // 解析消息体
                ret = parse_content(text);
                if (ret == GET_REQUEST) {
                    // 完整解析POST请求后，跳转到报文响应函数
                    return do_request();
                }
                // 解析完消息体即完成报文解析，避免再次进入循环，更新line_status
                line_status = LINE_OPEN;
                break;
            }
            default:
                return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;  // 表示读取完成
}

/**
 * 将网站根目录和url文件拼接，然后通过stat判断该文件的属性
 * 通过mmap进行映射，将普通文件映射到内存逻辑地址
 * 该函数的返回值试对请求的文件分析后的结果
 **/
// 处理请求
http_conn::HTTP_CODE http_conn::do_request() {
    // 将doc_root复制到m_real_file
    // 将初始化的m_real_file赋值为网站根目录
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    // printf("m_url:%s\n", m_url);
    // 在m_url中搜索最后一次出现'/'的位置
    // 返回m_url中最后一次出现字符'/'的位置。如果未找到该值，则函数返回一个空指针
    // 找到url中/所在位置，进而判断/后第一个字符
    const char* p = strrchr(m_url, '/');
    // cout << "p" << *p << endl;        // '/'
    // cout << "p" << *(p + 1) << endl;  // 2
    // 处理cgi，实现登录和注册校验
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3')) {
        // 根据标志判断是登录检测(2)还是注册检测(3)
        char flag = m_url[1];

        char* m_url_real = (char*)malloc(sizeof(char) * 200);
        // 将“/”复制到m_url_real
        strcpy(m_url_real, "/");
        // 把m_url + 2所指向的字符串追加到 m_url_real 所指向的字符串的结尾
        strcat(m_url_real, m_url + 2);
        // 把 m_url_real 所指向的字符串复制到 m_real_file + len，最多复制 FILENAME_LEN - len - 1 个字符
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);  // 释放空间

        // 将用户名和密码提取出来
        // user=root&passwd=123456
        char name[100], password[100];
        // cout << m_string << endl;  user=name&password=passwd
        // 以&为分隔符，前面的为用户名
        int i;
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        // 以&为分隔符，后面的为密码
        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

        if (*(p + 1) == '3') {
            // 如果是注册，先检测数据库中是否有重名的
            // 没有重名的，进行增加数据
            char* sql_insert = (char*)malloc(sizeof(char) * 200);
            // 创建SQL语句
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            if (users.find(name) == users.end()) {
                // 若找不到name
                // 向数据库中插入数据时，需要通过锁来同步数据
                // 查询时上锁
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();

                if (!res) {
                    // 若没有查到重名，即校验成功，则进行登录
                    strcpy(m_url, "/log.html");
                } else {
                    // 若重名了则报错
                    strcpy(m_url, "/registerError.html");
                }
            } else {
                // 若在原表中找到重名则报登陆错误
                strcpy(m_url, "/registerError.html");
            }
        } else if (*(p + 1) == '2') {
            // 如果是登录，直接判断
            // 若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
            if (users.find(name) != users.end() && users[name] == password) {
                strcpy(m_url, "/welcome.html");
            } else {
                strcpy(m_url, "/logError.html");
            }
        }
    }

    // 若请求的资源为/0，表示跳转注册界面
    if (*(p + 1) == '0') {
        // 跳转注册页面，GET
        char* m_url_real = (char*)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");

        // 将网站目录和/register.html进行拼接，更新到m_real_file中
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    } else if (*(p + 1) == '1') {
        // 跳转登录页面，GET
        char* m_url_real = (char*)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");

        // 将网站目录和/log.html进行拼接，更新到m_real_file中
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    } else if (*(p + 1) == '5') {
        // 显示图片页面，POST
        char* m_url_real = (char*)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    } else if (*(p + 1) == '6') {
        // 显示视频页面，POST
        char* m_url_real = (char*)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    } else if (*(p + 1) == '7') {
        // 显示关注页面，POST
        char* m_url_real = (char*)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    } else {
        // 否则发送url实际请求的文件
        // 若都不符合，则直接将url和网站目录进行拼接
        // 这里的情况是welcome界面，请求服务器上的一个图片
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
    }

    /**
     * struct stat {
     *      mode_t    st_mode;   // 文件类型和权限
     *      off_t     st_size;   // 文件大小，字节数
     * };
     * **/
    // 通过stat获取请求资源文件信息，成功则将信息更新到m_file_stat结构体
    // 失败则返回NO_RESOURCE状态，表示资源不存在
    if (stat(m_real_file, &m_file_stat) < 0) {
        return NO_RESOURCE;
    }

    // 判断文件权限，即是否可读，不可读则返回FORBIDDEN_REQUEST状态
    if (!(m_file_stat.st_mode & S_IROTH)) {
        return FORBIDDEN_REQUEST;
    }

    // 判断文件类型，如果是目录则返回BAD_REQUEST，表示请求报文你有误
    if (S_ISDIR(m_file_stat.st_mode)) {
        return BAD_REQUEST;
    }

    // 以只读的方式获取文件描述符，通过mmap将该文件映射到内存中
    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char*)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);

    // 关闭资源
    close(fd);
    return FILE_REQUEST;
}

void http_conn::unmap() {
    if (m_file_address) {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

/**
 * 服务器主线程检测写事件，并调用http_conn::write函数将响应报文发送给浏览器端
 * 函数逻辑：
 *      在生成响应报文时初始化byte_to_send，包括头部信息和文件数据大小。通过writev函数循环发送响应报文数据，
 *      根据返回值更新byte_have_send和iovec结构体的指针和长度，并判断响应报文整体是否发送成功
 *      - 若writev单次发送成功，更新byte_to_send和byte_have_send的大小，若响应报文整体发送成功,则取消mmap映射,并判断是否是长连接
 *          - 长连接重置http类实例，注册读事件，不关闭连接
 *          - 短连接直接关闭连接
 *      - 若writev单次发送不成功，判断是否是写缓冲区满了
 *          - 若不是因为缓冲区满了而失败，取消mmap映射，关闭连接
 *          - 若eagain则满了，更新iovec结构体的指针和长度，并注册写事件，等待下一次写事件触发（当写缓冲区从不可写变为可写，触发epollout）
 *            因此在此期间无法立即接收到同一用户的下一请求，但可以保证连接的完整性
 * **/
bool http_conn::write() {
    int temp = 0;

    // 若要发送的数据长度为0
    // 表示响应报文为空，一般不会出现这种情况
    if (bytes_to_send == 0) {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        init();
        return true;
    }

    while (1) {
        /**
         * ssize_t writev(int filedes, const struct iovec *iov, int iovcnt);
         *      - filedes表示文件描述符
         *      - iov为前述io向量机制结构体iovec
         *      - iovcnt为结构体的个数
         *      若成功则返回已写的字节数，若出错则返回-1
         * 循环调用writev时，需要重新处理iovec中的指针和长度，该函数不会对这两个成员做任何处理
         * **/
        // 将响应报文的状态行、消息头、空行和响应正文发送给浏览器端
        temp = writev(m_sockfd, m_iv, m_iv_count);

        // 单次发送失败
        if (temp < 0) {
            // 判断缓冲区是否满了
            if (errno == EAGAIN) {
                // 重新注册写事件
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                return true;
            }
            // 若发送失败不是缓冲区的问题，则取消映射
            unmap();
            return false;
        }

        // 正常发送，temp为发送的字节数
        // 更新已发送字节数
        bytes_have_send += temp;
        // 更新剩余发送字节数
        bytes_to_send -= temp;

        // 这里的代码位置有出入？
        // 第一个iovec头部信息的数据已发送完，发送第二个iovec数据
        if (bytes_have_send >= m_iv[0].iov_len) {
            // 不再继续发送头部信息
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        } else {
            // 继续发送第一个iovec头部信息的数据
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        // 若数据已全部发送完
        if (bytes_to_send <= 0) {
            unmap();
            // 在epoll树上充值EPOLLONESHOT事件
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);

            // 判断浏览器的请求是否为长连接
            if (m_linger) {
                // 重新初始化HTTP对象
                init();
                return true;
            } else {
                return false;
            }
        }
    }
}

bool http_conn::add_response(const char *format, ...) {
    // 若写入内容超出m_write_buf大小则报错
    if (m_write_idx >= WRITE_BUFFER_SIZE) {
        return false;
    }
    // 定义可变参数列表
    va_list arg_list;
    // 将变量arg_list初始化为传入的参数format
    va_start(arg_list, format);

    // 将数据format从可变参数列表写入缓冲区中，返回写入数据的长度
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);

    // 若写入的数据长度超过缓冲区剩余空间，则报错
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx)) {
        va_end(arg_list);
        return false;
    }
    // 更新位置
    m_write_idx += len;
    // 清空可变参数列表
    va_end(arg_list);

    LOG_INFO("request:%s", m_write_buf);

    return true;
}

// 添加状态行
bool http_conn::add_status_line(int status, const char *title) {
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

// 添加消息报头，具体的添加文本长度、连接状态和空行
bool http_conn::add_headers(int content_len) {
    return add_content_length(content_len) && add_linger() && add_blank_line();
}

// 添加Content-Length，表示响应报文的长度
bool http_conn::add_content_length(int content_len) {
    return add_response("Content-Length:%d\r\n", content_len);
}

// 添加文本类型，这里是html
bool http_conn::add_content_type() {
    return add_response("Content-Type:%s\r\n", "text/html");
}

// 添加连接状态，通知浏览器端是保持连接还是关闭
bool http_conn::add_linger() {
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}

// 添加空行
bool http_conn::add_blank_line() {
    return add_response("%s", "\r\n");
}

// 添加文本信息content
bool http_conn::add_content(const char *content) {
    return add_response("%s", content);
}

/**
 * 根据do_request的返回状态，服务器子线程调用process_write向m_write_buf中写入响应报文
 * 响应报文分为两种:
 *      一种是请求文件的存在，通过io向量机制 iovec，声明两个iovec，第一个指向 m_write_buf，第二个指向mmap的地址 m_file_address
 *      一种是请求出错，这时候只申请一个 iovec，指向 m_write_buf
 *      struct iovec {
 *          void      *iov_base;      // starting address of buffer
 *          size_t    iov_len;        // size of buffer
 *      };
 * **/
bool http_conn::process_write(HTTP_CODE ret) {
    switch (ret) {
        // 内部错误，500
        case INTERNAL_ERROR: {
            // 状态行
            add_status_line(500, error_500_title);
            // 消息报头
            add_headers(strlen(error_500_form));
            if (!add_content(error_500_form))
                return false;
            break;
        }
        case BAD_REQUEST: {
            // 报文语法有误，404
            add_status_line(404, error_404_title);
            add_headers(strlen(error_404_form));
            if (!add_content(error_404_form)) {
                return false;
            }
            break;
        }
        case FORBIDDEN_REQUEST: {
            // 资源没有访问权限，403
            add_status_line(403, error_403_title);
            add_headers(strlen(error_403_form));
            if (!add_content(error_403_form)) {
                return false;
            }
            break;
        }
        case FILE_REQUEST: {
            // 文件存在，200
            add_status_line(200, ok_200_title);
            // 若请求资源存在
            if (m_file_stat.st_size != 0) {
                add_headers(m_file_stat.st_size);
                // 第一个iovec指针指向响应报文缓冲区，长度指向m_write_idx
                m_iv[0].iov_base = m_write_buf;  // 记录buffer的起始位置
                m_iv[0].iov_len = m_write_idx;  // 记录buffer的size
                // 第二个iovec指针指向mmap返回的文件指针，长度指向文件大小
                m_iv[1].iov_base = m_file_address;  // 记录m_file_address的起始位置
                m_iv[1].iov_len = m_file_stat.st_size;  // 记录m_file_address的size
                m_iv_count = 2;
                // 发送的全部数据为响应报头信息和文件大小
                bytes_to_send = m_write_idx + m_file_stat.st_size;
                return true;
            } else {
                // 若请求的资源大小为0，则返回空白的html文件
                const char *ok_string = "<html><body></body></html>";
                add_headers(strlen(ok_string));
                if (!add_content(ok_string)) {
                    return false;
                }
            }
        }
        default:
            return false;
    }
    // 除了FILE_REQUEST状态外，其他状态只申请一个iovec，指向响应报文缓冲区
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

void http_conn::process() {
    // process_read是干嘛的？
    HTTP_CODE read_ret = process_read();

    // NO_REQUEST，表示请求不完整，需要继续接受请求数据
    if (read_ret == NO_REQUEST) {
        // 注册并监听读事件
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        return;
    }

    // 调用process_write完成报文响应
    bool write_ret = process_write(read_ret);
    if (!write_ret) {
        close_conn();
    }
    // 注册并监听写事件
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
}
