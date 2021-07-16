# server2.0

## 更新内容
1.1版主要解决了一个书里关于大文件传输的bug。

## 更新说明
  书中十一章先介绍了定时器，并给出了对应代码：1.lst_timer(source),它是一个定时器升序双向链表节点以及双向类，SIGALRM信号每次触发就在其信号处理函数中执行一次tick函数，以处理链表上到期的任务；2.nonactive_conn(resource).cpp，它利用alarm函数周期性地触发SIGALRM信号，该信号的信号处理函数利用管道通知主循环执行定时器链表上的定时任务——关闭非活动的连接。接下来的工作将上述源码结合到我们的项目中。
 
 ## BUG解决
 经过对源码的阅读，我发现http_conn.cpp Line325:
 int bytes_to_send = m_write_idx;// 将要发送的字节 （m_write_idx）写缓冲区中待发送的字节数 要发送的数据字节只包含了响应行，不包括响应体（要传的文件），所以文件可能就传了一点点就结束了！于是这一行修改为：bytes_to_send = m_write_idx + m_file_stat.st_size; 同时process_write()和write()也做了相应的修改，代码中已注释并说明，搜索v1.1即可！修改完之后运行的效果说明bug已修复：
 ![Image text]( https://ftp.bmp.ovh/imgs/2021/07/4bdc40c8c29daa1d.png)
 
 
