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

#define MAX_FD 65535 //最大文件描述符个数
#define MAX_EVENT_NUMBER 10000  //一次监听的最大事件数量

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

            }
            else if(events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)){
                // 对方异常断开或者错误等事件
                users[sockfd].close_conn();
            }
            else if(events[i].events & EPOLLIN){
                if(users[sockfd].read()){
                    //一次性把所有数据读出来
                    pool->append(users + sockfd);
                }
                else{
                    users[sockfd].close_conn();
                }
            }
            else if(events[i].events & EPOLLOUT){
                if(!users[sockfd].write()){
                    users[sockfd].close_conn();
                }
            }

        }
    }

    close(epollfd);
    close(listenfd);
    delete [] users;
    delete pool;

    return 0;

}