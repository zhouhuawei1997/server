#ifndef LOCKER_H
#define LOCKER_H

#include<pthread.h>
#include<exception>
#include<semaphore.h>
class locker{ //互斥锁类
public:
    locker(){
        if(pthread_mutex_init(&m_mutex, NULL) != 0){ //成功返回0
            throw std::exception();
        }
    }

    ~locker(){
        pthread_mutex_destroy(&m_mutex);
    }

    bool lock(){
        return pthread_mutex_lock(&m_mutex) == 0;  //成功返回0
    }

    bool unlock(){
        return pthread_mutex_unlock(&m_mutex) == 0;
    }

    pthread_mutex_t* get(){
        return &m_mutex;
    }


private:
    pthread_mutex_t m_mutex;
};

class cond{  //条件变量类
public:
    cond(){
        if(pthread_cond_init(&m_cond, NULL) != 0){
            throw std::exception();
        }
    }

    ~cond(){
        pthread_cond_destroy(&m_cond);
    }


/*
无论哪种等待方式，都必须和一个互斥锁配合，以防止多个线程同时请求
pthread_cond_wait()（或pthread_cond_timedwait()，下同）的竞
争条件（Race Condition）。mutex互斥锁必须是普通锁
（PTHREAD_MUTEX_TIMED_NP）或者适应锁（PTHREAD_MUTEX_ADAPTIVE_NP），
且在调用pthread_cond_wait()前必须由本线程加锁（pthread_mutex_lock()），
而在更新条件等待队列以前，mutex保持锁定状态，
并在线程挂起进入等待前解锁。在条件满足从而离开pthread_cond_wait()之前，
mutex将被重新加锁，以与进入pthread_cond_wait()前的加锁动作对应。
阻塞时处于解锁状态。
*/

    bool wait(pthread_mutex_t* mutex){
        return pthread_cond_wait(&m_cond, mutex) == 0;
    }

    bool timewait(pthread_mutex_t* mutex, struct timespec t){
        return pthread_cond_timedwait(&m_cond, mutex, &t) == 0;
    }

    bool signal(){
        return pthread_cond_signal(&m_cond) == 0; //发送一个信号给另外一个正在处于阻塞等待状态的线程,使其脱离阻塞状态,继续执行.如果没有线程处在阻塞等待状态,pthread_cond_signal也会成功返回
    }

    bool broadcast(){
        return  pthread_cond_broadcast(&m_cond) == 0;
    }
private:
    pthread_cond_t m_cond;
};

class sem{ //信号量
public:
    sem(){
        if(sem_init(&m_sem, 0, 0) != 0){  //第二个参数为0代表线程间 不为0代表进程间；第三个参数是初始值
            throw std::exception();
        }
    }

    sem(int num){
        if(sem_init(&m_sem, 0, num) != 0){ 
            throw std::exception();
        }
    }

    ~sem(){
        sem_destroy(&m_sem);
    }

    bool wait(){  //等待信号量 
        return sem_wait(&m_sem) == 0;  //非零时减一 成功返回0
    }

    bool post(){  //增加信号量
        return sem_post(&m_sem) == 0;
    }

private:
    sem_t m_sem;
};

#endif