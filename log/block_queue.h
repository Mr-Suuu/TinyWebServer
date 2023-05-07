/*************************************************************
*循环数组实现的阻塞队列，m_back = (m_back + 1) % m_max_size;  
*线程安全，每个操作前都要先加互斥锁，操作完后，再解锁
**************************************************************/

#ifndef BLOCK_QUEUE_H
#define BLOCK_QUEUE_H

#include <iostream>
#include <stdlib.h>
#include <pthread.h>
#include <sys/time.h>
#include "../lock/locker.h"

using namespace std;

// 当队列为空时，从队列中获取元素的线程将会被挂起；当队列是满时，往队列里添加元素的线程将会挂起
template<class T>
class block_queue {
public:
    // 初始化私有成员
    block_queue(int max_size = 1000) {
        if (max_size <= 0) {
            exit(-1);
        }

        // 构造函数创建循环数组
        m_max_size = max_size;
        m_array = new T[max_size];
        m_size = 0;
        m_front = -1;
        m_back = -1;
    }

    void clear() {
        m_mutex.lock();
        m_size = 0;
        m_front = -1;
        m_back = -1;
        m_mutex.unlock();
    }

    ~block_queue() {
        m_mutex.lock();
        if (m_array != NULL)
            delete[] m_array;

        m_mutex.unlock();
    }

    // 判断队列是否满了
    bool full() {
        m_mutex.lock();
        if (m_size >= m_max_size) {

            m_mutex.unlock();
            return true;
        }
        m_mutex.unlock();
        return false;
    }

    // 判断队列是否为空
    bool empty() {
        m_mutex.lock();
        if (0 == m_size) {
            m_mutex.unlock();
            return true;
        }
        m_mutex.unlock();
        return false;
    }

    // 返回队首元素
    bool front(T &value) {
        m_mutex.lock();
        if (0 == m_size) {
            m_mutex.unlock();
            return false;
        }
        value = m_array[m_front];
        m_mutex.unlock();
        return true;
    }

    // 返回队尾元素
    bool back(T &value) {
        m_mutex.lock();
        if (0 == m_size) {
            m_mutex.unlock();
            return false;
        }
        value = m_array[m_back];
        m_mutex.unlock();
        return true;
    }

    int size() {
        int tmp = 0;

        m_mutex.lock();
        tmp = m_size;

        m_mutex.unlock();
        return tmp;
    }

    int max_size() {
        int tmp = 0;

        m_mutex.lock();
        tmp = m_max_size;

        m_mutex.unlock();
        return tmp;
    }

    // 往队列添加元素，需要将所有使用队列的线程先唤醒
    // 当有元素push进队列,相当于生产者生产了一个元素
    // 若当前没有线程等待条件变量,则唤醒无意义
    bool push(const T &item) {
        // 先上锁
        m_mutex.lock();
        if (m_size >= m_max_size) {
            // 若当前可用个数大于等于最大个数
            // 则直接广播唤醒所有元素并解锁，返回false
            m_cond.broadcast();
            m_mutex.unlock();
            return false;
        }

        // 将新增数据放在循环数组的对应位置
        m_back = (m_back + 1) % m_max_size;
        m_array[m_back] = item;
        m_size++;

        // 唤醒元素
        m_cond.broadcast();
        m_mutex.unlock();
        return true;
    }

    // pop时,如果当前队列没有元素,将会等待条件变量
    bool pop(T &item) {
        // 先上锁
        m_mutex.lock();
        // 多个消费者的时候，这里应该用while而不是if
        while (m_size <= 0) {
            // 资源不足的情况
            // 当重新抢到互斥锁，m_cond.wait会返回0
            if (!m_cond.wait(m_mutex.get())) {
                // 表示发生互斥的情况
                // 解锁
                m_mutex.unlock();
                return false;
            }
        }

        // 当有足够资源的时候
        // 取出队列首的元素，使用循环数组模拟队列
        m_front = (m_front + 1) % m_max_size;
        item = m_array[m_front];
        m_size--;
        m_mutex.unlock();
        return true;
    }

    // 增加了超时处理（项目中没有使用到）
    // 取出队列首的元素，这里需要理解一下，使用循环数组模拟的队列
    // 其他逻辑不变
    bool pop(T &item, int ms_timeout) {
        struct timespec t = {0, 0};
        struct timeval now = {0, 0};
        gettimeofday(&now, NULL);
        m_mutex.lock();
        if (m_size <= 0) {
            t.tv_sec = now.tv_sec + ms_timeout / 1000;
            t.tv_nsec = (ms_timeout % 1000) * 1000;
            if (!m_cond.timewait(m_mutex.get(), t)) {
                m_mutex.unlock();
                return false;
            }
        }

        if (m_size <= 0) {
            m_mutex.unlock();
            return false;
        }

        m_front = (m_front + 1) % m_max_size;
        item = m_array[m_front];
        m_size--;
        m_mutex.unlock();
        return true;
    }

private:
    // 创建互斥锁和同步变量
    locker m_mutex;
    cond m_cond;

    T *m_array;
    int m_size;
    int m_max_size;
    int m_front;
    int m_back;
};

#endif
