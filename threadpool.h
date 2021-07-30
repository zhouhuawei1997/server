#ifndef THREADPOOL_H
#define THREADPOOL_H

#include<pthread.h>
#include<list>
#include"locker.h"
#include<exception>
#include<cstdio>
#include "sql_connection_pool.h"
//线程池类 定义为模版是为了代码复用 T是任务类
template<typename T>
class threadpool{
public:
    /*thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量*/
    //v5.0 线程构造时会带上连接池
    threadpool(connection_pool *connPool, int thread_number = 8, int max_request = 10000);
    ~threadpool();
    bool append(T* request);  //添加任务

private:
    static void* worker(void * arg);
    void run();
private:
    
    int m_thread_number;  //线程数量
    pthread_t * m_threads; //线程池数组，大小为m_thread_number
    int m_max_requests;  //请求队列中最多允许的，等待处理的请求数量
    std::list<T*> m_workqueue; //请求队列
    locker m_queuelocker;  //互斥锁
    sem m_queuestat;  //信号量用来判断是否有任务需要处理
    bool m_stop;  //是否结束线程
    connection_pool *m_connPool;  //v5.0 数据库连接池指针
};


template <typename T>  //v5.0 数据库连接也要初始化
threadpool<T>::threadpool( connection_pool *connPool, int thread_number, int max_requests) : m_thread_number(thread_number), m_max_requests(max_requests), m_stop(false), m_threads(NULL),m_connPool(connPool)
{
    if (thread_number <= 0 || max_requests <= 0)
        throw std::exception();
    m_threads = new pthread_t[m_thread_number];
    if (!m_threads)
        throw std::exception();
    for (int i = 0; i < thread_number; ++i)
    {
        //创建线程，并设置线程分离
        if (pthread_create(m_threads + i, NULL, worker, this) != 0)
        {
            delete[] m_threads;
            throw std::exception();
        }
        if (pthread_detach(m_threads[i]))
        {
            delete[] m_threads;
            throw std::exception();
        }
    }
}

template<typename T>
threadpool<T>::~threadpool(){
    printf("thread pool destruction！\n");
    delete[] m_threads;
    m_stop = true;
}

template<typename T>
bool threadpool<T>::append(T * request){
    printf("thread pool append!\n");
    m_queuelocker.lock();
    if(m_workqueue.size() > m_max_requests){
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}

template<typename T>
void* threadpool<T>::worker(void* arg){
    printf("thread pool worker!\n");
    threadpool* pool = (threadpool*) arg;  //用this指针传参
    pool->run();
    return pool;
}

template<typename T>
void threadpool<T>::run(){
    printf("run()!\n");
    while(!m_stop){
        m_queuestat.wait();  //值为0就阻塞
        m_queuelocker.lock();
        if(m_workqueue.empty()){
            m_queuelocker.unlock();
            continue;
        }
        T* request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if(!request){
            continue;
        }
        printf("thread pool process()!\n");
        connectionRAII mysqlcon(&request->mysql, m_connPool);  //v5.0 线程服务开启后 获得一个数据库的连接
        request->process();

    }
}

#endif