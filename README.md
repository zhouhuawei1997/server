# 项目简介

## 项目地址：
  http://1.117.171.95/
## 使用技术：
线程池、MySQL连接池、HTTP解析（GET、POST）、socket编程、IO多路复用、单例模式、RAII机制、XSS攻击防范。
## 项目描述：
使用Reactor模型，并使用多线程提高并发度。为避免线程/数据库连接频繁创建和销毁带来的开销，使用线程池/连接池，在程序的开始创建固定数量的线程/连接，并通过信号量协调各线程/连接的竞争。使用epoll作为IO多路复用的实现方式。当前实现功能为：（1）客户端输入特定网址后经由服务器解析POST报文即可访问对应网站；（2）留言板相关功能：（i）写留言（http://1.117.171.95/boardwrite.html ）：通过解析POST报文将用户提交的留言写入MySQL数据库；（ii）读取留言（ http://1.117.171.95/boardview.html ）：通过解析GET报文从MySQL数据库中读取对应数据生成网页返回给用户；（3）日志系统打印服务日志。经压力测试可以实现接近上万的并发连接。
![Image text](https://ftp.bmp.ovh/imgs/2021/07/6fb1d72408fd9950.png)

# server5.0
## 更新内容
  5.0版主要基于3.0版本对留言板相关功能做了更新，引入了**MySQL数据库**管理留言用户的姓名和内容。采用**连接池**避免数据库频繁创建和销毁带来的开销，采用**RAII机制**封装了数据库的连接与释放，实现连接后自动释放。当用户通过**POST提交留言**时，数据会INSERT进数据库（如v3.0所说，还是要进行**xss攻击防范**）；当用户通过**GET访问留言板**时，服务器会从数据库读取数据并写成网页发送给用户。在当前版本中，所有用户访问的界面是一样的，所以其实业务逻辑也可以是用户每次POST提交留言即刷新留言板网页而后用户访问时直接返回该网页文件，这样实际开销会比较小。不过考虑到实际应用中用户访问得是个性化网页，同时也顺便学习一下数据库读取的相关操作，因而采用了每次用户访问都读数据库的业务逻辑。另外，实际的开发中，还要注意**防范MySQL注入攻击**。
## 相关技术
  对数据库的连接和断开的操作资源开销较大，故采用连接池技术与RAII机制：使用单例模式初始化连接池保证有且仅有一个连接池，连接池类中有一个存放MySQL* 类型的链表，初始化时产生MaxConn个连接到数据库的连接并存放于链表中。再定义connectionRAII类对数据库的连接和释放做封装，当某块业务需要和数据库做交互时，采用如下代码：
  
  MYSQL *mysql = NULL;
  connectionRAII mysqlcon(&mysql, connPool);  
  
  通过信号量指示，在仍有数据库连接空闲的情况下获得一个连接并完成和数据库的交互，若暂无空闲连接则等待。
  具体更新内容可于源码中搜索"v5.0"查看。
## 压力测试（结果也和服务器性能有关，此处测试的是每个月一块钱的云服务器。。。）
  ![Image text](https://ftp.bmp.ovh/imgs/2021/07/5cc10d2cfc73e19c.jpg)

# server4.0
## 更新内容
4.0版主要修改了原书中一个**比较大的bug**，找bug靠的是**日志**的输出信息，可见日志的重要性，因此还添加了日志相关功能。
## BUG描述
![Image text](https://ftp.bmp.ovh/imgs/2021/07/30cbb72fe668ca31.jpg)

  书中P307的代码采用的是ET模式，当我测试ET模式时，发现一些问题：
  
  1.如上图所示打开首页，经常出现图片刷不开的现象！而且哪一张刷不开也不一定！
  
  2.之后同一个浏览器再打开网页，可能就会连接不上！
  
  3.但如果再用另一个浏览器（比如手机）连接网页，原先的那个网页就有可能突然刷出来！
## BUG原因
![Image text](https://ftp.bmp.ovh/imgs/2021/07/4696b1c551435085.png)

  对于1：经过对日志的查看，发现网页上因为有图片资源，所以实际上服务器收到的GET请求不止一条，而是多条，但**服务器缺不能保证每一次都收到所有的GET请求，而会丢失**，导致图片整张显示不出（对这张图片的GET请求没得到相应）。
  
![Image text](https://ftp.bmp.ovh/imgs/2021/07/3b901cf6d7a36277.png)

  对于2：经过对日志的查看，发现服务器刚连接上，就被信号**EPOLLRDHUB结束连接**，因此浏览器无法收到响应！
## BUG分析
  对于1：源代码（书P307）用的是ET模式，但main函数中 line140行左右（搜索“//v4.0 bug定位”），**对于listen的处理却只接受一次**，那么就会存在一种可能，例如**两个连接（连接1和连接2）的到来唤醒了一次listen事件，但源代码只处理连接1，连接2被遗留在队列中。而由于采用ET模式，只有新的连接3进来，才可能唤醒listen事件，使连接2连接成功，但连接3可能又滞留了**。
  
  对于2：因为ACCPT是一个三次握手过程，**对同一个浏览器而言，假设发起的是10号请求，服务器收到的却是之前滞留的2号请求，服务器发回的ack是对应的2号请求的syn而不是10号的，这样正在请求10号的客户端收到的ack不对，自然会触发EPOLLRDHUP信号，但是发起的10号也会滞留在服务器中，会使问题一直重复出现**！
## BUG解决  
  对于ET模式，listen模块需要增加**while(true)循环**，将等待accept的所有连接**全都accept**，切勿有任何遗留！ 具体代码可搜索main.cpp中“//v4.0 bug修改 ”查看。
## 心得收获
  发现bug固然使人难受，但还是要冷静下来，一种方法就是通过打印的日志寻找可能的原因，因此，日志打印对于服务器而言是很重要的，当发现bug时，可以去日志文件中找到对应的信息进行分析。因此，v4.0版还加上了日志打印的功能，将关键信息存入日志文件“2021_07_22_ServerLog”中。


# server3.0
## 更新内容
3.0版主要增加了写留言功能（ http://1.117.171.95/boardwrite.html ） 与查看留言板功能（ http://1.117.171.95/boardview.html ），目的是学习POST报文的解析。

## 更新说明
  书中代码不涉及POST报文的解析，我按照GET报文解析的方法写了**POST**解析，能够收到用户提交的word字段与name字段（name=%E5%91%A8%E5%8D%8E%E4%BC%9F&word=%E6%88%91%E7%88%B1%E4%BD%A0111），可以发现，对于中文而言，POST报文会将其转码为UTF-8，并用9个字符（格式为%xx%xx%xx）表示1个汉字，因此此处需要一个**解码函数**将该编码转化为中文（http_conn.cpp Line 125-175）。当解析到POST及其内容是，会调用writeboard（Line 177）对留言板的网页文件进行增加留言的操作。对网页文件的写操作需要一个互斥锁。同时，还需特别注意对**xss攻击**的防范，因此对用户的输入内容会有一个**标签过滤**（Line155）：将"<"替换为"& l t;"，将">"替换为"& g t;"。虽然留言板功能实现了，但这个方法是最原始的，它没有数据管理的功能，且效率低下。因此，后续还会考虑用数据库重写该功能。
  ![Image text](https://ftp.bmp.ovh/imgs/2021/07/90e58b74139d012f.jpg)

# server2.0

## 更新内容
2.0版主要增加了一个定时器以处理**非活动连接**（即规定时间内没再次活动的连接）。

## 更新说明
  书中十一章先介绍了定时器，并给出了对应代码：1.lst_timer(source),它包含一个**定时器升序双向链表节点类以及双向链表类**，SIGALRM信号每次触发就在其信号处理函数中执行一次tick函数，以处理链表上到期的任务；2.nonactive_conn(resource).cpp，它利用alarm函数周期性地触发SIGALRM信号，该信号的信号处理函数利用**管道通知主循环执行定时器链表上的定时任务——关闭非活动的连接**。接下来的工作是将上述源码结合到我们的项目中。
  lst_timer.h包含：（1）client_data类：用户信息（ip，sockfd，缓冲区）以及一个timer指针；（2）util_timer定时器节点类：前后指针、超时时间（绝对时间）、任务回调函数和指向client_data类的指针；（3）sort_timer_lst定时器双向链表：含头尾指针和双向链表的常用操作，该链表按照每个定时器的超时时间从小到大排列，这样遇到一个不超时的节点即可结束循环，因为后面的节点更不可能超时了。
  在main函数中，主要增加的部分为管道、链表的初始化，SIGALRM的产生与捕捉（往管道一端写数据），超时任务的回调函数cb_func（）的功能（删除连接）、管道另一端增添进epoll监听列表、新连接产生时的定时器初始化、超时/异常/读/写事件定时器的相关操作，具体在代码中搜索v2.0有详细注释。
  
## 定时器工作流程
![Image text](https://ftp.bmp.ovh/imgs/2021/07/3247b71a2e7a15e1.jpg)


# server1.1

## 更新内容
1.1版主要解决了一个书里关于大文件传输的bug。

## BUG发现
  在云端搭完服务器后，我很开心地发到好友群里让大家访问欣赏，并且还写了个为这个群定制的网页，如下图：
![Image text](https://ftp.bmp.ovh/imgs/2021/07/a2f3aa6eb35f1767.jpg)
  但我发现左边那个帅哥的照片（不是我哦。。）总是刷到一半就刷不出来，其他的图片倒是每次都能刷出来，我就觉得有点奇怪，看了下图片大小，这个头像是几百k，其他图片是几十k。连几百K的图片都不能成功显示吗？我表示这可不行！为了验证是不是因为文件过大导致刷不出来，我测试了一张6M的照片，如图所示：
 ![Image text](https://ftp.bmp.ovh/imgs/2021/07/9833614b976420cd.jpg)
 图片确实显示不全，而且我还发现其实图片的最右侧是能显示出一条纵向的小条条，给人的感觉就是**图片刚开始传（而且是从右向左传的），就结束了**。这肯定有问题！
 
 ## BUG解决
 经过对源码的阅读，我发现http_conn.cpp Line325:
 int bytes_to_send = m_write_idx;// 将要发送的字节 （m_write_idx）写缓冲区中待发送的字节数 要发送的数据字节只包含了响应行，不包括响应体（要传的文件），所以文件可能就传了一点点就结束了！于是这一行修改为：bytes_to_send = m_write_idx + m_file_stat.st_size; 同时process_write()和write()也做了相应的修改，代码中已注释并说明，搜索v1.1即可！修改完之后运行的效果说明bug已修复：
 ![Image text]( https://ftp.bmp.ovh/imgs/2021/07/4bdc40c8c29daa1d.png)
 
 

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

