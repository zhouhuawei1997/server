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

#define MAX_FD 65535 //最大文件描述符个数
#define MAX_EVENT_NUMBER 10000  //一次监听的最大事件数量
#define TIMESLOT 5  //v2.0 时隙长度

static int pipefd[2];
//v2.0 利用lst_timer.h中的升序双向链表来管理定时器
static sort_timer_lst timer_list;
static int epollfd = 0;

void addsig(int sig, void(*handler)(int)){  //添加信号捕捉
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    sigfillset(&sa.sa_mask);  //sa_mask为临时阻塞信号集（在处理该信号时暂时将sa_mask 指定的信号集搁置）  sa_mask所有位置1 在调用信号处理程序时就能阻塞某些信号。注意仅仅是在信号处理程序正在执行时才能阻塞某些信号，如果信号处理程序执行完了，那么依然能接收到这些信号。
    sigaction(sig, &sa, NULL);  //注册信号捕捉
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

    //创建监听套接字
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);  //AF = Address Family PF = Protocol Family 其实没区别 设计的时候想多了

    //设置端口复用
    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    //绑定
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);
    bind(listenfd, (struct sockaddr*)&address, sizeof(address));

    //监听
    listen(listenfd, 5);  //当有多个客户端程序和服务端相连时,表示可以接受的排队长度，超过则connect refuse

    //创建epoll对象，事件数组，添加
    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5);  //参数非0即可

    //将监听文件描述符添加到epoll对象中
    addfd(epollfd, listenfd, false);
    http_conn::m_epollfd = epollfd;


    //v2.0 定时器相关设置
    socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);//创建一对sock套接字
    setnonblocking_main(pipefd[1]);
    addfd(epollfd, pipefd[0], false);

    //设置信号处理函数
    addsig(SIGALRM, sig_handler);
    addsig(SIGTERM, sig_handler);
    bool stop_server = false;

    client_data* users_timer = new client_data[MAX_FD];  //存用户信息
    bool timeout = false;
    alarm(TIMESLOT);  //TIMESLOT秒后发出SIGALRM信号


    while(true){
        int num = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);  //第四个参数为timeout 0不阻塞 -1阻塞
        if(num < 0 && (errno != EINTR)){
            printf("epoll failure\n");
            break;
        }

        //循环遍历事件数组
        for(int i = 0; i < num; i++){
            int sockfd = events[i].data.fd;
            if(sockfd == listenfd){
                //有客户端连接进来
                struct sockaddr_in client_address;  //含ip和端口号
                socklen_t client_addrlen = sizeof(client_address);
                int connfd = accept(listenfd, (struct  sockaddr*)&client_address, &client_addrlen);  //为这个链接分配的fd
                if(http_conn::m_user_count >= MAX_FD){
                    //目前连接数满了,给客户端写一个信息
                    close(connfd);
                    continue;
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
            else if(events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)){
                // 对方异常断开或者错误等事件
                //users[sockfd].close_conn(); 删除定时器的时候顺便把这一步也做了
                //v2.0 异常时删除定时器 断开连接
                util_timer *timer = users_timer[sockfd].timer;
                timer->cb_func(&users_timer[sockfd]);
                if (timer)
                {
                    timer_list.del_timer(timer);
                }

            }

            //v2.0 一旦信号触发，触发sig_headle函数往pipe[1]中写入数据，此时可以触发epoll
            else if((sockfd == pipefd[0]) && (events[i].events & EPOLLIN))
            {
                int sig;
                char signals[1024];
                int ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if(ret == -1)
                {
                    //handle the error
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
                    pool->append(users + sockfd);

                    //更新时间
                    if(timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        //printf("adjust timer by write!\n");
                        timer_list.adjust_timer(timer);
                    }
                }
                else
                {
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
                
                util_timer * timer = users_timer[sockfd].timer;
                if(timer)
                {
                    time_t cur = time(NULL);
                    timer->expire = cur + 3 * TIMESLOT;
                    //printf("adjust timer by write!\n");
                    timer_list.adjust_timer(timer);
                }
                

                if(!users[sockfd].write())
                {
                    users[sockfd].close_conn();
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