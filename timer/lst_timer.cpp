#include "lst_timer.h"
#include "../http/http_conn.h"

sort_timer_lst::sort_timer_lst() {
    head = NULL;
    tail = NULL;
}

sort_timer_lst::~sort_timer_lst() {
    util_timer *tmp = head;
    while (tmp) {
        head = tmp->next;
        delete tmp;
        tmp = head;
    }
}

void sort_timer_lst::add_timer(util_timer *timer) {
    if (!timer) {
        return;
    }
    if (!head) {
        head = tail = timer;
        return;
    }
    if (timer->expire < head->expire) {
        timer->next = head;
        head->prev = timer;
        head = timer;
        return;
    }
    add_timer(timer, head);
}

void sort_timer_lst::adjust_timer(util_timer *timer) {
    if (!timer) {
        return;
    }
    util_timer *tmp = timer->next;
    if (!tmp || (timer->expire < tmp->expire)) {
        return;
    }
    if (timer == head) {
        head = head->next;
        head->prev = NULL;
        timer->next = NULL;
        add_timer(timer, head);
    } else {
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        add_timer(timer, timer->next);
    }
}

void sort_timer_lst::del_timer(util_timer *timer) {
    if (!timer) {
        return;
    }
    if ((timer == head) && (timer == tail)) {
        delete timer;
        head = NULL;
        tail = NULL;
        return;
    }
    if (timer == head) {
        head = head->next;
        head->prev = NULL;
        delete timer;
        return;
    }
    if (timer == tail) {
        tail = tail->prev;
        tail->next = NULL;
        delete timer;
        return;
    }
    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
    delete timer;
}

void sort_timer_lst::tick() {
    if (!head) {
        return;
    }

    time_t cur = time(NULL);
    util_timer *tmp = head;
    while (tmp) {
        if (cur < tmp->expire) {
            break;
        }
        tmp->cb_func(tmp->user_data);
        head = tmp->next;
        if (head) {
            head->prev = NULL;
        }
        delete tmp;
        tmp = head;
    }
}

void sort_timer_lst::add_timer(util_timer *timer, util_timer *lst_head) {
    util_timer *prev = lst_head;
    util_timer *tmp = prev->next;
    while (tmp) {
        if (timer->expire < tmp->expire) {
            prev->next = timer;
            timer->next = tmp;
            tmp->prev = timer;
            timer->prev = prev;
            break;
        }
        prev = tmp;
        tmp = tmp->next;
    }
    if (!tmp) {
        prev->next = timer;
        timer->prev = prev;
        timer->next = NULL;
        tail = timer;
    }
}

void Utils::init(int timeslot) {
    m_TIMESLOT = timeslot;
}

//对文件描述符设置非阻塞
int Utils::setnonblocking(int fd) {
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void Utils::addfd(int epollfd, int fd, bool one_shot, int TRIGMode) {
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

// 信号处理函数
void Utils::sig_handler(int sig) {
    // 为保证函数的可重入性，保留原来的errno
    // 可重入性表示中断后再次进入该函数，环境变量与之前相同，不会丢失数据
    int save_errno = errno;
    int msg = sig;

    // 将信号量从管道写端写入，传输字符类型，而非整型
    send(u_pipefd[1], (char *) &msg, 1, 0);

    // 将原来的errno赋值为当前的errno
    errno = save_errno;
}

// 设置信号函数
void Utils::addsig(int sig, void(handler)(int), bool restart) {
    // 创建sigaction结构体变量
    struct sigaction sa;
    // 复制字符 '\0' 到 sa 所指向的字符串的前 sizeof(sa) 个字符
    memset(&sa, '\0', sizeof(sa));

    // 信号处理函数中仅仅发送信号量，不做对应逻辑处理
    sa.sa_handler = handler;

    // SA_RESTART，使被信号打断的系统调用自动重新发起
    // 一旦给信号设置了SA_RESTART标记，那么当执行某个阻塞系统调用时，收到该信号时，进程不会返回，而是重新执行该系统调用
    if (restart) {
        sa.sa_flags |= SA_RESTART;
    }
    // 将所有信号添加到信号集中
    sigfillset(&sa.sa_mask);
    // 执行sigaction函数
    assert(sigaction(sig, &sa, NULL) != -1);
}

//定时处理任务，重新定时以不断触发SIGALRM信号
void Utils::timer_handler() {
    m_timer_lst.tick();
    alarm(m_TIMESLOT);
}

void Utils::show_error(int connfd, const char *info) {
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int *Utils::u_pipefd = 0;
int Utils::u_epollfd = 0;

class Utils;

void cb_func(client_data *user_data) {
    epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    close(user_data->sockfd);
    http_conn::m_user_count--;
}
