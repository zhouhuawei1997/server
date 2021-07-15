# 项目简介

## 项目地址：
  http://1.117.171.95/zls.html
## 使用技术：
线程池、多线程、HTTP解析，socket编程，IO多路复用。
## 项目描述：
使用Reactor模型，并使用多线程提高并发度。为避免线程频繁创建和销毁带来的开销，使用线程池，在程序的开始创建固定数量的线程。使用epoll作为IO多路复用的实现方式。当前实现功能为客户端输入特定网址后经由服务器解析POST报文即可访问对应网站；后续考虑实现解析GET报文完成网页留言板功能。
![Image text](https://ftp.bmp.ovh/imgs/2021/07/6fb1d72408fd9950.png)

# server1.0

1.0版本主要工作是看着书敲一遍代码，彻彻底底地看懂代码，画出流程框图，添加必要的注释。本地跑通后，上传到腾讯云。同时，考虑了一下几个问题：

1.为什么其他网站不用输入端口号？
HTTP默认端口是80 把程序绑定的端口号设置为80就可以不输入端口号，直接把原先的10001端口号改成80，无效。以为是腾讯云平台的问题，问了客服无解，想着只能用iptables做本地端口转发，查阅资料时偶然发现Linux限制1024一下端口必须有root权限才能开启，所以增加sudo命令，把程序的端口号设置为80，成功！但了解到这样做不是很安全，因为1024前的端口号一般不直接绑定，因此用以下语句做端口转发：
iptables -t nat -A PREROUTING -p tcp --dport 80 -j REDIRECT --to-port 10001
实现需求效果。

2.为什么要捕捉SIGPIPE信号？
对一个对端已经关闭的socket调用两次write，第二次将会生成SIGPIPE信号，该信号默认结束进程。因此，需要捕捉该信号，忽略它（SIG_IGN）。

3.threadpool.h line45:if(pthread_create(m_threads + i, NULL, worker, this) != 0)为什么要求worker一定是静态函数？
worker必须是静态函数C++的类成员函数都有一个默认参数 this 指针，而线程调用的时候，限制了只能有一个参数 void* arg，如果不设置成静态在调用的时候会出现this 和arg都给worker 导致错误。

