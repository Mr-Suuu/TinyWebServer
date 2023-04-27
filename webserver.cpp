#include "webserver.h"

WebServer::WebServer() {
    // http_conn类对象
    // 创建MAX_FD个http类对象
    users = new http_conn[MAX_FD];

    // root文件夹路径
    char server_path[200];
    getcwd(server_path, 200);
    char root[6] = "/root";
    m_root = (char *) malloc(strlen(server_path) + strlen(root) + 1);
    strcpy(m_root, server_path);
    strcat(m_root, root);

    // 定时器
    users_timer = new client_data[MAX_FD];
}

WebServer::~WebServer() {
    close(m_epollfd);
    close(m_listenfd);
    close(m_pipefd[1]);
    close(m_pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete m_pool;
}

void WebServer::init(int port, string user, string passWord, string databaseName, int log_write,
                     int opt_linger, int trigmode, int sql_num, int thread_num, int close_log, int actor_model) {
    m_port = port;
    m_user = user;
    m_passWord = passWord;
    m_databaseName = databaseName;
    m_sql_num = sql_num;
    m_thread_num = thread_num;
    m_log_write = log_write;
    m_OPT_LINGER = opt_linger;
    m_TRIGMode = trigmode;
    m_close_log = close_log;
    m_actormodel = actor_model;
}

void WebServer::trig_mode() {
    // LT + LT
    if (0 == m_TRIGMode) {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 0;
    }
        // LT + ET
    else if (1 == m_TRIGMode) {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 1;
    }
        // ET + LT
    else if (2 == m_TRIGMode) {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 0;
    }
        // ET + ET
    else if (3 == m_TRIGMode) {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 1;
    }
}

void WebServer::log_write() {
    if (0 == m_close_log) {
        // 初始化日志
        if (1 == m_log_write)
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 800);
        else
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 0);
    }
}

void WebServer::sql_pool() {
    // 初始化数据库连接池
    m_connPool = connection_pool::GetInstance();
    m_connPool->init("localhost", m_user, m_passWord, m_databaseName, 3306, m_sql_num, m_close_log);

    // 初始化数据库读取表
    users->initmysql_result(m_connPool);
}

void WebServer::thread_pool() {
    // 线程池
    m_pool = new threadpool<http_conn>(m_actormodel, m_connPool, m_thread_num);
}

void WebServer::eventListen() {
    // 网络编程基础步骤
    m_listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(m_listenfd >= 0);

    // 优雅关闭连接
    if (0 == m_OPT_LINGER) {
        struct linger tmp = {0, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    } else if (1 == m_OPT_LINGER) {
        struct linger tmp = {1, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }

    int ret = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(m_port);

    int flag = 1;
    setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
    ret = bind(m_listenfd, (struct sockaddr *) &address, sizeof(address));
    assert(ret >= 0);
    ret = listen(m_listenfd, 5);
    assert(ret >= 0);

    utils.init(TIMESLOT);

    // epoll创建内核事件表
    epoll_event events[MAX_EVENT_NUMBER];
    m_epollfd = epoll_create(5);
    assert(m_epollfd != -1);

    // 將m_listenfd放在epoll树上
    utils.addfd(m_epollfd, m_listenfd, false, m_LISTENTrigmode);
    // 将上述m_epollfd赋值给http类对象的m_epollfd属性
    http_conn::m_epollfd = m_epollfd;

    /**
     * 在Linux下，使用socketpair函数能够创建一对套接字进行通信，项目中使用管道通信
     * int socketpair(int domain, int type, int protocol, int sv[2]);
     *      - domain表示协议族，PF_UNIX或者AF_UNIX
     *      - type表示协议，可以是SOCK_STREAM或者SOCK_DGRAM，SOCK_STREAM基于TCP，SOCK_DGRAM基于UDP
     *      - protocol表示类型，只能为0
     *      - sv[2]表示套节字柄对，该两个句柄作用相同，均能进行读写双向操作，创建好的套接字分别是sv[0]和sv[1]
     *      返回值：0为创建成功，-1为创建失败
     * **/
    // 创建管道套接字
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd);
    assert(ret != -1);

    // 设置管道写端为非阻塞
    // 设置为非阻塞的原因：send是将信息发送给套接字缓冲区，如果缓冲区满了，则会阻塞，这时候会进一步增加信号处理函数的执行时间，为此，将其修改为非阻塞
    // 这里没有对非阻塞的返回值进行处理，因为定时事件是非必须立即处理的事件，可以云逊这一次定时事件失效
    utils.setnonblocking(m_pipefd[1]);

    // 设置管道读端为ET非阻塞
    utils.addfd(m_epollfd, m_pipefd[0], false, 0);

    utils.addsig(SIGPIPE, SIG_IGN);
    // 传递给主循环的信号量，这里只关注SIGALRM和SIGTERM
    utils.addsig(SIGALRM, utils.sig_handler, false);
    utils.addsig(SIGTERM, utils.sig_handler, false);

    // 每隔TIMESLOT事件出发SIGALRM信号
    alarm(TIMESLOT);

    // 工具类,信号和描述符基础操作
    Utils::u_pipefd = m_pipefd;
    Utils::u_epollfd = m_epollfd;
}

void WebServer::timer(int connfd, struct sockaddr_in client_address) {
    users[connfd].init(connfd, client_address, m_root, m_CONNTrigmode, m_close_log, m_user, m_passWord, m_databaseName);

    // 初始化client_data数据
    // 创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
    users_timer[connfd].address = client_address;
    users_timer[connfd].sockfd = connfd;
    util_timer *timer = new util_timer;
    timer->user_data = &users_timer[connfd];
    timer->cb_func = cb_func;
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    users_timer[connfd].timer = timer;
    utils.m_timer_lst.add_timer(timer);
}

// 若有数据传输，则将定时器往后延迟3个单位
// 并对新的定时器在链表上的位置进行调整
void WebServer::adjust_timer(util_timer *timer) {
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    utils.m_timer_lst.adjust_timer(timer);

    LOG_INFO("%s", "adjust timer once");
}

void WebServer::deal_timer(util_timer *timer, int sockfd) {
    timer->cb_func(&users_timer[sockfd]);
    if (timer) {
        utils.m_timer_lst.del_timer(timer);
    }

    LOG_INFO("close fd %d", users_timer[sockfd].sockfd);
}

// http 处理用户连接，为有效连接初始化定时器进行后续操作
bool WebServer::dealclinetdata() {
    struct sockaddr_in client_address;
    socklen_t client_addrlength = sizeof(client_address);
    if (0 == m_LISTENTrigmode) {
        int connfd = accept(m_listenfd, (struct sockaddr *) &client_address, &client_addrlength);
        if (connfd < 0) {
            LOG_ERROR("%s:errno is:%d", "accept error", errno);
            return false;
        }
        if (http_conn::m_user_count >= MAX_FD) {
            utils.show_error(connfd, "Internal server busy");
            LOG_ERROR("%s", "Internal server busy");
            return false;
        }
        timer(connfd, client_address);
    } else {
        while (1) {
            int connfd = accept(m_listenfd, (struct sockaddr *) &client_address, &client_addrlength);
            if (connfd < 0) {
                LOG_ERROR("%s:errno is:%d", "accept error", errno);
                break;
            }
            if (http_conn::m_user_count >= MAX_FD) {
                utils.show_error(connfd, "Internal server busy");
                LOG_ERROR("%s", "Internal server busy");
                break;
            }
            timer(connfd, client_address);
        }
        return false;
    }
    return true;
}

bool WebServer::dealwithsignal(bool &timeout, bool &stop_server) {
    int ret = 0;
    int sig;
    char signals[1024];

    // 从管道读端赌除信号量，成功则返回字节数，失败返回-1
    // 正常情况下，这里的ret返回值总是1，只有14和15两个ASCII码对应的字符
    ret = recv(m_pipefd[0], signals, sizeof(signals), 0);
    if (ret == -1) {
        return false;
    } else if (ret == 0) {
        return false;
    } else {
        // 处理信号值对应的逻辑
        for (int i = 0; i < ret; ++i) {
            // 这里面是字符
            switch (signals[i]) {
                // 这里是整型
                // 信号本身是整型数值，管道中传递的是ASCII码表中整型数值对应的字符
                // switch的变量一般为字符或整型，当switch的变量为字符时，case中可以是字符，也可以是字符对应的ASCII码
                case SIGALRM: {
                    timeout = true;
                    break;
                }
                case SIGTERM: {
                    stop_server = true;
                    break;
                }
            }
        }
    }
    return true;
}

void WebServer::dealwithread(int sockfd) {
    util_timer *timer = users_timer[sockfd].timer;

    // reactor
    if (1 == m_actormodel) {
        if (timer) {
            adjust_timer(timer);
        }

        // 若监测到读事件，将该事件放入请求队列
        m_pool->append(users + sockfd, 0);

        while (true) {
            if (1 == users[sockfd].improv) {
                if (1 == users[sockfd].timer_flag) {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    } else {
        // proactor
        if (users[sockfd].read_once()) {
            LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            //若监测到读事件，将该事件放入请求队列
            m_pool->append_p(users + sockfd);

            if (timer) {
                adjust_timer(timer);
            }
        } else {
            deal_timer(timer, sockfd);
        }
    }
}

void WebServer::dealwithwrite(int sockfd) {
    util_timer *timer = users_timer[sockfd].timer;
    // reactor
    if (1 == m_actormodel) {
        if (timer) {
            adjust_timer(timer);
        }

        m_pool->append(users + sockfd, 1);

        while (true) {
            if (1 == users[sockfd].improv) {
                if (1 == users[sockfd].timer_flag) {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    } else {
        // proactor
        if (users[sockfd].write()) {
            LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            if (timer) {
                adjust_timer(timer);
            }
        } else {
            deal_timer(timer, sockfd);
        }
    }
}

void WebServer::eventLoop() {
    bool timeout = false;
    bool stop_server = false;

    while (!stop_server) {
        // 等待所监控文件描述符上有事件的产生
        /**
         * int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout);
         * maxenvents: 最大监听fd
         * timeout: 0，不阻塞立即返回
         *         -1，阻塞直到监听的一个fd上有一个感兴趣事件发生
         * 返回准备好的fd数量
         **/
        // 这里是阻塞等待
        int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
        if (number < 0 && errno != EINTR) {
            LOG_ERROR("%s", "epoll failure");
            break;
        }

        // 对所有就绪事件进行处理
        for (int i = 0; i < number; i++) {
            int sockfd = events[i].data.fd;

            // 处理新到的客户连接
            if (sockfd == m_listenfd) {
                // 当一个新用户访问时，调用dealclinetdata()为该用户初始化http_conn对象加入epoll监听，和初始化定时器对象，加入非活动连接管理
                bool flag = dealclinetdata();
                if (false == flag) {
                    // 初始化失败则跳过
                    continue;
                }
            } else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                // 处理异常事件
                // 服务器端关闭连接，移除对应的定时器
                util_timer *timer = users_timer[sockfd].timer;
                deal_timer(timer, sockfd);
            } else if ((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN)) {
                // 处理定时器信号
                // 接收到SIGALRM信号，timeout设置为True
                bool flag = dealwithsignal(timeout, stop_server);
                if (false == flag) {
                    LOG_ERROR("%s", "dealclientdata failure");
                }
            } else if (events[i].events & EPOLLIN) {
                // 处理客户连接上接收到的数据
                dealwithread(sockfd);
            } else if (events[i].events & EPOLLOUT) {
                // 处理客户连接上send的数据
                dealwithwrite(sockfd);
            }
        }
        // 处理定时器为非必须事件，收到信号并不是立马处理
        // 完成读写事件后，再进行处理
        if (timeout) {
            utils.timer_handler();
            LOG_INFO("%s", "timer tick");
            timeout = false;
        }
    }
}