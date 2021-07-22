#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<errno.h>
#include<fcntl.h>
#include<sys/epoll.h>
#include"locker.h"
#include"threadpool.h"
#include <libgen.h>
#include<signal.h>
#include"http_conn.h"
#include<unistd.h>
#include"lst_timer.h"
#include <assert.h>
#include "log.h"

#define MAX_FD 65535 //最大文件描述符个数
#define MAX_EVENT_NUMBER 10000  //一次监听的最大事件数量
#define TIMESLOT 5  //v2.0 时隙长度

static int pipefd[2];
//v2.0 利用lst_timer.h中的升序双向链表来管理定时器
static sort_timer_lst timer_list;
static int epollfd = 0;

void addsig(int sig, void(handler)(int), bool restart = true)
{
    struct sigaction ac;
    memset(&ac, '\0', sizeof(ac));
    ac.sa_handler = handler;
    if(restart)
    {
        ac.sa_flags |= SA_RESTART;//中断系统调用时，返回的时候重新执行该系统调用
    }
    sigfillset(&ac.sa_mask);//sa_mask中的信号只有在该ac指向的信号处理函数执行的时候才会屏蔽的信号集合!
    assert(sigaction(sig, &ac, NULL) != -1);
}

//显示错误信息，关闭该链接的文件描述符
void show_error(int connfd, const char * info)
{
    printf("%s",info);
    send(connfd, info, strlen(info), 0);
    close(connfd);
}


//添加文件描述符到epoll中
extern void addfd(int epollfd, int fd, bool one_shot);
//从epoll中删除文件描述符
extern void removefd(int epollfd, int fd);
//修改文件描述符
extern void modfd(int epollfd, int fd, int ev);


//v2.0 信号处理函数
void sig_handler(int sig)
{
    int save_errno = errno;
    int msg = sig;
    send(pipefd[1], (char*)&msg, 1, 0);
    errno = save_errno;
}

void timer_handler()
{
    //v2.0 定时处理任务，实际上就是调用tick函数
    timer_list.tick();
    //因为一次alarm调用只会引起一次SIGALRM信号，所以我们要重新定时，以不断触发SIGALRM信号
    alarm(TIMESLOT);
}

//v2.0 定时器回调函数，他删除非活动链接socket上的注册事件
void cb_func(client_data* user_data)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    close(user_data->sockfd);
    http_conn::m_user_count--;
    printf("close fd [%d] by time cb_func \n",user_data->sockfd);
    LOG_INFO("close fd [%d] by time cb_func", user_data->sockfd);
    Log::get_instance()->flush();
}
//设置非阻塞IO
int setnonblocking_main(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}


int main(int argc, char* argv[]){
    Log::get_instance()->init("ServerLog", 2000, 800000, 0); //v4.0 同步日志模型 最后一个参数维0代表同步
    if(argc <= 1){
        printf("按照如下格式运行：%s port_number\n", basename(argv[0]));
        exit(-1);
    }
    int port = atoi(argv[1]);  //获取端口号

    //对SIGPIE信号进行处理 https://blog.csdn.net/u010821666/article/details/81841755
    addsig(SIGPIPE, SIG_IGN);  //捕获SIGPIPE信号，忽略它

    //初始化线程池
    threadpool<http_conn>* pool = NULL;
    try{
        pool = new threadpool<http_conn>;
    }catch(...){
        exit(-1);
    }

    //创建一个数组用于保存所有的客户端信息
    http_conn* users = new http_conn[MAX_FD];
    assert(users);

    //创建监听套接字
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);  //AF = Address Family PF = Protocol Family 其实没区别 设计的时候想多了
    assert(listenfd >= 0);

    //设置端口复用
    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    int ret = 0;
    //绑定
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);
    ret = bind(listenfd, (struct sockaddr*)&address, sizeof(address));
    assert(ret >= -1);
    //监听

    ret = listen(listenfd, 5);  //当有多个客户端程序和服务端相连时,表示可以接受的排队长度，超过则connect refuse
    assert(ret >= 0);

    //创建epoll对象，事件数组，添加
    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5);  //参数非0即可
    assert(epollfd != 0);

    //将监听文件描述符添加到epoll对象中
    addfd(epollfd, listenfd, false);
    http_conn::m_epollfd = epollfd;


    //v2.0 定时器相关设置
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);//创建一对sock套接字
    assert(ret != -1);
    setnonblocking_main(pipefd[1]);
    addfd(epollfd, pipefd[0], false);

    //设置信号处理函数
    addsig(SIGALRM, sig_handler, false);
    addsig(SIGTERM, sig_handler, false);
    bool stop_server = false;

    client_data* users_timer = new client_data[MAX_FD];  //存用户信息
    bool timeout = false;
    alarm(TIMESLOT);  //TIMESLOT秒后发出SIGALRM信号


    while(!stop_server){  
        int num = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);  //第四个参数为timeout 0不阻塞 -1阻塞
        if(num < 0 && (errno != EINTR)){
            printf("epoll failure\n");
            LOG_ERROR("%s", "epoll failure");  //v4.0
            break;
        }

        //循环遍历事件数组
        for(int i = 0; i < num; i++){
            int sockfd = events[i].data.fd;
            if(sockfd == listenfd){
                //有客户端连接进来
                struct sockaddr_in client_address;  //含ip和端口号
                socklen_t client_addrlen = sizeof(client_address);
//v4.0 bug定位
//listen ET  //v4.0 bug解决
/*  
                while(1){
                    printf("waiting listen accept!\n");
                    int connfd = accept(listenfd, (struct  sockaddr*)&client_address, &client_addrlen);  //为这个链接分配的fd
                    printf("accept! connfd:[%d]\n",connfd);
                    if (connfd < 0)
                    {
                        LOG_ERROR("%s:errno is:%d", "accept error", errno);  //v4.0
                        printf("test1\n");
                        break;
                    }
                    if(http_conn::m_user_count >= MAX_FD){
                        //目前连接数满了,给客户端写一个信息
                        show_error(connfd,"Internet server busy");
                        printf("test2\n");
                        LOG_ERROR("%s", "Internal server busy");  //v4.0
                        break;
                    }
                    //将新的客户数据初始化，放入数组当中
                    users[connfd].init(connfd, client_address);

                    //v2.0 新客户数据交付给定时器
                    users_timer[connfd].address = client_address;
                    users_timer[connfd].sockfd = connfd;

                    //v2.0 创建定时器，设置回调函数与超时时间，然后绑定定时器与用户数据，最后将定时器添加到链表timer_list中
                    util_timer * timer = new util_timer;
                    timer->user_data = &users_timer[connfd];
                    timer->cb_func = cb_func;
                    time_t cur = time(NULL);
                    timer->expire = cur + 3*TIMESLOT;//超时时间为15s，15无连接则关闭链接
                    users_timer[connfd].timer = timer;  //用户信息加入定时器
                    timer_list.add_timer(timer);  //定时器列表加入定时器
                }
                continue;
*/
                int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlen);
                if (connfd < 0)
                {
                    LOG_ERROR("%s:errno is:%d", "accept error", errno);  //v4.0
                    continue;
                }
                if (http_conn::m_user_count >= MAX_FD)
                {
                    show_error(connfd, "Internal server busy");
                    LOG_ERROR("%s", "Internal server busy");  //v4.0
                    continue;
                }
                users[connfd].init(connfd, client_address);

                //初始化client_data数据
                //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
                users_timer[connfd].address = client_address;
                users_timer[connfd].sockfd = connfd;
                util_timer *timer = new util_timer;
                timer->user_data = &users_timer[connfd];
                timer->cb_func = cb_func;
                time_t cur = time(NULL);
                timer->expire = cur + 3 * TIMESLOT;
                users_timer[connfd].timer = timer;
                timer_list.add_timer(timer);
                

            }
            else if(events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)){
                // 对方异常断开或者错误等事件
                //users[sockfd].close_conn(); 删除定时器的时候顺便把这一步也做了
                //v2.0 异常时删除定时器 断开连接
                util_timer *timer = users_timer[sockfd].timer;
                printf("events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR) cb_func\n");
                LOG_ERROR("%s", "events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR) cb_func");  //v4.0
                timer->cb_func(&users_timer[sockfd]);
                if (timer)
                {
                    timer_list.del_timer(timer);
                }

            }

            //v2.0 一旦信号触发，触发sig_handle函数往pipe[1]中写入数据，此时可以触发epoll
            else if((sockfd == pipefd[0]) && (events[i].events & EPOLLIN))
            {
                int sig;
                char signals[1024];
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if(ret == -1)
                {
                    continue;
                }
                else if(ret == 0)
                {
                    continue;
                }
                else 
                {
                    for(int i = 0; i < ret; ++i)
                    {
                        switch (signals[i])
                        {
                            case SIGALRM:
                            {
                                //用timeout变量标记有定时任务需要处理，但不立即处理定时任务，这是因为定时任务的优先级不是很高，我们优先处理其他更重要的任务
                                timeout = true;
                                break;
                            }
                            case SIGTERM:
                            {
                                stop_server = true;
                            }
                            
                        }
                    }
                }
            }

            //v2.0 定时器相关更新
            else if(events[i].events & EPOLLIN)  //其他读事件发生
            {
                util_timer *timer = users_timer[sockfd].timer;
                if(users[sockfd].read())
                {   
                    LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
                    Log::get_instance()->flush();  //v4.0
                    pool->append(users + sockfd);

                    //更新时间
                    if(timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        LOG_INFO("%s", "adjust timer because of EPOLLIN");  //v4.0
                        Log::get_instance()->flush();
                        timer_list.adjust_timer(timer);
                    }
                }
                else
                {
                    printf("events[i].events & EPOLLIN cb_func\n");
                    timer->cb_func(&users_timer[sockfd]);
                    if (timer)
                    {
                        timer_list.del_timer(timer);
                    }
                    //users[sockfd].close_conn();
                }
            }

            //v2.0 定时器相关更新
            else if(events[i].events & EPOLLOUT)
            {
                //创建定时器，设置其回调函数与超时时间，然后绑定定时器与用户数据，最后将定时器添加到链表timer_list中
                util_timer *timer = users_timer[sockfd].timer;
                if (users[sockfd].write())  //写数据成功
                {
                    LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));  //v4.0
                    Log::get_instance()->flush();

                    //若有数据传输，则将定时器往后延迟3个单位,并对新的定时器在链表上的位置进行调整
                    if (timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        LOG_INFO("%s", "adjust timer because of EPOLLOUT");  //v4.0
                        Log::get_instance()->flush();
                        timer_list.adjust_timer(timer);
                    }
                }
                else
                {
                    timer->cb_func(&users_timer[sockfd]);  //失败了 关闭并删除定时器
                    if (timer)
                    {
                        timer_list.del_timer(timer);
                    }
                }
                
            }
            if(timeout)
            {
                timer_handler();
                timeout = false;
            }


        }
    }

    //v2.0 关闭管道
    close(pipefd[0]);
    close(pipefd[1]);
    close(epollfd);
    close(listenfd);
    delete [] users;
    delete pool;

    return 0;

}