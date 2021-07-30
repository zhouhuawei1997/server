#include "http_conn.h"
#include <fstream>
#include <string>
#include <stdio.h>
#include <string.h>
#include <vector>
#include <assert.h>
#include <time.h>
#include <iostream>
#include "locker.h"
#include "log.h"
#include <mysql/mysql.h>  //5.0 头文件别忘了
using namespace std;


// 定义HTTP响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";

// 网站的根目录
const char* doc_root = "/home/ubuntu/webserver/resources";


//对文件描述符设置非阻塞
int setnonblocking( int fd ) {
    int old_option = fcntl( fd, F_GETFL );
    int new_option = old_option | O_NONBLOCK;
    fcntl( fd, F_SETFL, new_option );
    return old_option;
}

// 向epoll中添加需要监听的文件描述符
void addfd( int epollfd, int fd, bool one_shot ) {
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLRDHUP ;  // EPOLLRDHUP 事件，代表对端断开连接
    //event.events = EPOLLIN | EPOLLRDHUP | EPOLLET;  //ET模式
    if(one_shot) 
    {
        // 防止同一个通信被不同的线程处理
        event.events |= EPOLLONESHOT;
    }
    printf("addfd！fd:[%d]\n",fd);
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    // 设置文件描述符非阻塞 在读取不到数据或是写入缓冲区已满会马上return，而不会阻塞等待，在读取不到数据时会回传-1，并且设置errno为EAGAIN
    setnonblocking(fd);  
}

// 从epoll中移除文件描述符
void removefd( int epollfd, int fd ) {
    epoll_ctl( epollfd, EPOLL_CTL_DEL, fd, 0 );
    close(fd);
}

// 修改文件描述符，重置socket上的EPOLLONESHOT事件，以确保下一次可读时，EPOLLIN事件能被触发
void modfd(int epollfd, int fd, int ev) {
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLONESHOT | EPOLLRDHUP ;
    //event.events = ev | EPOLLONESHOT | EPOLLRDHUP | EPOLLET;  //ET模式
    epoll_ctl( epollfd, EPOLL_CTL_MOD, fd, &event );
}

// 所有的客户数
int http_conn::m_user_count = 0;
// 所有socket上的事件都被注册到同一个epoll内核事件中，所以设置成静态的
int http_conn::m_epollfd = -1;
// v3.0 留言数量初始化
int http_conn::word_count = 0;
locker http_conn::board_lock;

// 关闭连接
void http_conn::close_conn(bool real_close) {
    if(real_close && m_sockfd != -1) {
        printf("closefd! fd: [%d]\n", m_sockfd);
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--; // 关闭一个连接，将客户总数量-1
    }
}

// 初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in& addr, connection_pool *_connPool){  //v5.0更新 连接池初始化
    m_sockfd = sockfd;
    m_address = addr;
    connPool = _connPool;  //v5.0
    char _ip[64];
    inet_ntop(AF_INET, &m_address.sin_addr, _ip,sizeof(_ip));
    uint16_t port = addr.sin_port;
    printf("http_conn::init ip = [%s] port = [%d] fd = [%d]\n", _ip, port, sockfd);
    // 端口复用
    //int reuse = 1;
    //setsockopt( m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof( reuse ) );
    addfd( m_epollfd, sockfd, true );
    m_user_count++;
    init();
}

void http_conn::init()
{
    mysql = NULL;  //v5.0 mysql初始化
    m_check_state = CHECK_STATE_REQUESTLINE;    // 初始状态为检查请求行
    m_linger = false;       // 默认不保持链接  Connection : keep-alive保持连接

    m_method = GET;         // 默认请求方式为GET
    m_url = 0;              
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    bytes_to_send = 0;  //v1.1 需要发送文件字节数初始化
    bytes_have_send = 0;  //v1.1 已经发送文件字节数初始化
    post_index = 0;  //v3.0  POST默认不启用
    m_string = 0;
    bzero(m_read_buf, READ_BUFFER_SIZE);
    bzero(m_write_buf, READ_BUFFER_SIZE);
    bzero(m_real_file, FILENAME_LEN);
}

//v3.0 写留言板相关函数
unsigned char FromHex(unsigned char x)   //16进制字符转化为数字
{ 
    unsigned char y;
    if (x >= 'A' && x <= 'Z') y = x - 'A' + 10;
    else if (x >= 'a' && x <= 'z') y = x - 'a' + 10;
    else if (x >= '0' && x <= '9') y = x - '0';
    else assert(0);
    return y;
}
 

 
 //输入：%E5%91%A8%E5%8D%8E%E4%BC%9F  输出：周华伟
std::string UrlDecode(const std::string& str)
{
    std::string strTemp = "";
    size_t length = str.length();
    for (size_t i = 0; i < length; i++)
    {
        if (str[i] == '+') strTemp += ' ';
        else if (str[i] == '%')  //以%号为起始标志，观察后两位字符 %E5 这两个字符代表一个字节，一个字符代表4bit 每三个%代表一个汉字
        {
            assert(i + 2 < length);
            unsigned char high = FromHex((unsigned char)str[++i]); //高位是E
            unsigned char low = FromHex((unsigned char)str[++i]);  //低位是5
            strTemp += high*16 + low;  //得到汉字的utf-8编码
        }
        else strTemp += str[i];
    }
    //标签过滤, 防止xss攻击
    int pos = 0;
    while((pos = strTemp.find("<")) != string::npos){  
		strTemp = strTemp.replace(pos, 1, "&lt;"); 
	} 
    
	while((pos = strTemp.find(">")) != string::npos){
		strTemp = strTemp.replace(pos, 1, "&gt;"); 
	}
    return strTemp;
}

//name=%E5%91%A8%E5%8D%8E%E4%BC%9F&word=%E6%88%91%E7%88%B1%E4%BD%A0111
//返回｛周华伟，我爱你111｝
vector<string> decode(string m_text) {
    string text = m_text;
    int word_pos = text.find("&word=");
    string name = text.substr(5, word_pos - 5);  //%E5%91%A8%E5%8D%8E%E4%BC%9F
    string word = text.substr(word_pos + 6);  //%E6%88%91%E7%88%B1%E4%BD%A0111
    return vector<string>{UrlDecode(name), UrlDecode(word)};
}

/* v4.0 直接把数据写到网页
void writeboard(char* content){
    http_conn::word_count++;
    string str = content;
    vector<string> nameWord = decode(str);
    string name = nameWord[0];
    string word = nameWord[1];
    const char* cname = name.c_str();
    const char* cword = word.c_str();
    LOG_INFO("POST succeed! name: %s, word: %s", cname, cword);  //v4.0
    time_t timep;
    time (&timep);
    string str_time = asctime(gmtime(&timep));
    //cout<<"name: "<<name<<"word: "<<word<<endl;
    //printf("open file! \n");
    http_conn::board_lock.lock();  //写文件时锁住
    std::ofstream aa("resources/boardview.html",std::ios::in);
    aa.seekp(-7,std::ios::end);
    string cont = "<p>" +  to_string(http_conn::word_count) + ". " + name + "： “" + word + "”" + "               留言时间： " + str_time + "</p>\n</html>";
    aa<<cont;
    aa.close();
    http_conn::board_lock.unlock();  //写完解锁
    //printf("write succeed! \n");
    return;
}
*/

//v5.0 向mysql数据库中写入数据
void http_conn::writeboard(char* content){
    http_conn::word_count++;
    string str = content;
    vector<string> nameWord = decode(str);
    string nameVal = nameWord[0];
    string wordVal = nameWord[1];
    const char* cname = nameVal.c_str();
    const char* cword = wordVal.c_str();
    char *sql_insert = (char *)malloc(sizeof(char) * 200);
    strcpy(sql_insert, "INSERT INTO board(name, word, time) VALUES(");
    strcat(sql_insert, "'");
    strcat(sql_insert, cname);
    strcat(sql_insert, "','");
    strcat(sql_insert, cword);
    strcat(sql_insert, "',now())");
    //printf("%s\n", sql_insert);
    http_conn::board_lock.lock();
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);  
    mysql_query(mysql, "set names utf8");  //支持中文
    int res = mysql_query(mysql, sql_insert);
    if(!res){
        strcpy(m_url, "/boardsucceed.html");  //向数据库写入数据成功
    }
    else{
        strcpy(m_url, "/boardfailed.html");  //向数据库写入数据失败
    }
    http_conn::board_lock.unlock();  //解锁
    LOG_INFO("boardwriting succeed! name: %s, word: %s", cname, cword);
}

// 循环读取客户数据，直到无数据可读或者对方关闭连接
bool http_conn::read() {
    if( m_read_idx >= READ_BUFFER_SIZE ) {
        return false;
    }
    int bytes_read = 0;
    while(true) {
        // 从m_read_buf + m_read_idx索引出开始保存数据，大小是READ_BUFFER_SIZE - m_read_idx
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, 
        READ_BUFFER_SIZE - m_read_idx, 0 );
        if (bytes_read == -1) {
            if( errno == EAGAIN || errno == EWOULDBLOCK ) {  //这两个一般是一样的
                // 没有数据
                break;
            }
            return false;   
        } else if (bytes_read == 0) {   // 对方关闭连接
            return false;
        }
        m_read_idx += bytes_read;
    }
    return true;
}

// 解析一行，判断依据\r\n
http_conn::LINE_STATUS http_conn::parse_line() {
    char temp;
    for ( ; m_checked_idx < m_read_idx; ++m_checked_idx ) {
        temp = m_read_buf[ m_checked_idx ];
        if ( temp == '\r' ) {
            if ( ( m_checked_idx + 1 ) == m_read_idx ) {
                return LINE_OPEN;
            } else if ( m_read_buf[ m_checked_idx + 1 ] == '\n' ) {
                m_read_buf[ m_checked_idx++ ] = '\0';
                m_read_buf[ m_checked_idx++ ] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        } else if( temp == '\n' )  {
            if( ( m_checked_idx > 1) && ( m_read_buf[ m_checked_idx - 1 ] == '\r' ) ) {
                m_read_buf[ m_checked_idx-1 ] = '\0';
                m_read_buf[ m_checked_idx++ ] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

// 解析HTTP请求行，获得请求方法，目标URL,以及HTTP版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char* text) {
    // GET /index.html HTTP/1.1
    m_url = strpbrk(text, " \t"); // 判断第二个参数中的字符哪个在text中最先出现
    if (! m_url) { 
        return BAD_REQUEST;
    }
    // GET\0/index.html HTTP/1.1
    *m_url++ = '\0';    // 置位空字符，字符串结束符
    char* method = text;
    if ( strcasecmp(method, "GET") == 0 ) { // 忽略大小写比较
        m_method = GET;
    } 
    else if( strcasecmp(method, "POST") == 0){  //V3.0 解析POST请求行
        m_method = POST;
        post_index = 1;
    }
    else {
        return BAD_REQUEST;
    }
    // /index.html HTTP/1.1
    // 检索字符串 str1 中第一个不在字符串 str2 中出现的字符下标。
    m_version = strpbrk( m_url, " \t" );
    if (!m_version) {
        return BAD_REQUEST;
    }
    *m_version++ = '\0';
    if (strcasecmp( m_version, "HTTP/1.1") != 0 ) {
        return BAD_REQUEST;
    }
    /**
     * http://192.168.110.129:10000/index.html
    */
    if (strncasecmp(m_url, "http://", 7) == 0 ) {   
        m_url += 7;
        // 在参数 str 所指向的字符串中搜索第一次出现字符 c（一个无符号字符）的位置。
        m_url = strchr( m_url, '/' );
    }
    if ( !m_url || m_url[0] != '/' ) {
        return BAD_REQUEST;
    }
    //当url为/时，显示主页
    if (strlen(m_url) == 1)
        strcat(m_url, "zls.html");
    m_check_state = CHECK_STATE_HEADER; // 检查状态变成检查头
    return NO_REQUEST;
}

// 解析HTTP请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char* text) {   
    // 遇到空行，表示头部字段解析完毕
    if( text[0] == '\0' ) {
        // 如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体，
        // 状态机转移到CHECK_STATE_CONTENT状态
        if ( m_content_length != 0 ) {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        // 否则说明我们已经得到了一个完整的HTTP请求
        return GET_REQUEST;
    } else if ( strncasecmp( text, "Connection:", 11 ) == 0 ) {
        // 处理Connection 头部字段  Connection: keep-alive
        text += 11;
        text += strspn( text, " \t" );  //size_t strspn(const char *str1, const char *str2) 检索字符串 str1 中第一个不在字符串 str2 中出现的字符下标
        if ( strcasecmp( text, "keep-alive" ) == 0 ) {
            m_linger = true;
        }
    } else if ( strncasecmp( text, "Content-Length:", 15 ) == 0 ) {
        // 处理Content-Length头部字段
        text += 15;
        text += strspn( text, " \t" );
        m_content_length = atol(text);  //字符串转换成长整型数
    } else if ( strncasecmp( text, "Host:", 5 ) == 0 ) {
        // 处理Host头部字段
        text += 5;
        text += strspn( text, " \t" );
        m_host = text;
    } else {
        //LOG_INFO("oop!unknow header: %s", text);
        Log::get_instance()->flush();
    }
    return NO_REQUEST;
}

// 解析HTTP请求的消息体
http_conn::HTTP_CODE http_conn::parse_content( char* text ) {
    if ( m_read_idx >= ( m_content_length + m_checked_idx ) )
    {
        text[ m_content_length ] = '\0';
        //v3.0 记录消息体内容
        m_string = text;
        //printf("%s\n", m_url);
        printf("GET_POST: %s\n", m_string);
        if(post_index && m_string && strcmp(m_url, "/0") == 0){
            printf("entering writeboard function! \n");
            writeboard(m_string);
        }
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

// 主状态机，解析请求
http_conn::HTTP_CODE http_conn::process_read() {
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char* text = 0;
    while (((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK))  
                || ((line_status = parse_line()) == LINE_OK)) {  //后者是说还有行（\r\n为标识）没读
        // 获取一行数据
        text = get_line();
        m_start_line = m_checked_idx;
        printf( "got 1 http line: %s\n", text );

        switch ( m_check_state ) {
            case CHECK_STATE_REQUESTLINE: {
                ret = parse_request_line( text );
                if ( ret == BAD_REQUEST ) {
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER: {
                ret = parse_headers( text );
                if ( ret == BAD_REQUEST ) {
                    return BAD_REQUEST;
                } else if ( ret == GET_REQUEST ) {
                    return do_request();
                }
                break;
            }
            case CHECK_STATE_CONTENT: {
                ret = parse_content( text );  //ret可能为NO_REQUEST（代表根据content-length判断，还没读完），也可能为GET_REQUEST
                if ( ret == GET_REQUEST ) {  
                    return do_request();
                }
                line_status = LINE_OPEN;  //ret为NO_REQUEST
                break;
            }
            default: {
                return INTERNAL_ERROR;
            }
        }
    }
    return NO_REQUEST;
}

// 当得到一个完整、正确的HTTP请求时，我们就分析目标文件的属性，
// 如果目标文件存在、对所有用户可读，且不是目录，则使用mmap将其
// 映射到内存地址m_file_address处，并告诉调用者获取文件成功
http_conn::HTTP_CODE http_conn::do_request()
{
    // doc_root = "/home/zhou/webserver/resources"
    printf("do_request!\n");
    strcpy( m_real_file, doc_root );
    int len = strlen( doc_root ); 
    strncpy( m_real_file + len, m_url, FILENAME_LEN - len - 1 );  
    //char *strncpy(char *dest, const char *src, int n)，表示把src所指向的字符串中以src地址开始的前n个字节复制到dest所指的数组中，并返回被复制后的dest
    // 获取m_real_file文件的相关的状态信息，-1失败，0成功
    printf("request file: %s\n", m_real_file);
    char* temp_board = "/home/ubuntu/webserver/resources/boardview.html";
    if(strcmp(temp_board, m_real_file) == 0){  //v5.0 查看留言板内容，从数据库中读取需要的资料，写成html文件，返回给用户
        string filename = "resources/boardview" + to_string(m_sockfd) + ".html";
        //cout<<filename<<endl;
        std::ofstream aa(filename.c_str(),ios::out|ios::binary);
        string write_content = "<html>\n  <head>\n    <meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\" />\n    <title>留言板</title>\n  </head>\n  <body><br/>\n<br/>\n<div align=\"center\"><font size=\"5\"> <strong>留言板！</strong></font></div>\n<div align=\"center\"></div><p>周华伟： 沙发是我自己的！</p>\n";
        aa<<write_content;
        //先从连接池中取一个连接
        MYSQL *mysql = NULL;
        connectionRAII mysqlcon(&mysql, connPool);
        //在user表中检索name，word数据
        mysql_query(mysql, "set names utf8");  //支持中文
        if (mysql_query(mysql, "SELECT No, name, word, time FROM board"))
        {
            LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
        }

        //从表中检索完整的结果集
        MYSQL_RES *result = mysql_store_result(mysql);

        //返回结果集中的列数
        int num_fields = mysql_num_fields(result);

        //返回所有字段结构的数组
        MYSQL_FIELD *fields = mysql_fetch_fields(result);

        //从结果集中获取下一行，将对应的数据
        while (MYSQL_ROW row = mysql_fetch_row(result))
        {
            string number(row[0]);
            string name(row[1]);
            string word(row[2]);
            string time(row[3]);
            write_content = "<p>" + number + ". " + name + ": " + word + "   留言时间：" + time + "</p>\n";
            aa<<write_content;
        }
        aa<<"</html>";
        aa.close();
        string newfile = "/home/ubuntu/webserver/" + filename;
        //strcpy( m_real_file, filename.c_str());
        memset(m_real_file, 0, sizeof(m_real_file));
        memcpy(m_real_file, newfile.data(), newfile.length());
    }
    if ( stat( m_real_file, &m_file_stat ) < 0 ) {  //通过文件名filename获取文件信息，并保存在buf所指的结构体stat中.执行成功则返回0，失败返回-1，错误代码存于errno
        return NO_RESOURCE;
    }

    // 判断访问权限
    if ( ! ( m_file_stat.st_mode & S_IROTH ) ) {  //S_IROTH所有人可读
        return FORBIDDEN_REQUEST;
    }

    // 判断是否是目录
    if ( S_ISDIR( m_file_stat.st_mode ) ) {
        return BAD_REQUEST;
    }

    // 以只读方式打开文件
    int fd = open( m_real_file, O_RDONLY );
    // 创建内存映射
    m_file_address = ( char* )mmap( 0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0 );
    close(fd);
    return FILE_REQUEST;
}

// 对内存映射区执行munmap操作
void http_conn::unmap() {
    if( m_file_address )
    {
        munmap( m_file_address, m_file_stat.st_size );
        m_file_address = 0;
    }
}

// 写HTTP响应
bool http_conn::write()
{
    printf("http_conn::write()\n");
    int temp = 0;

    /* v1.0删除
    int bytes_have_send = 0;    // 已经发送的字节
    int bytes_to_send = m_write_idx;// 将要发送的字节 （m_write_idx）写缓冲区中待发送的字节数
    */


    if ( bytes_to_send == 0 ) {
        // 将要发送的字节为0，这一次响应结束。
        modfd( m_epollfd, m_sockfd, EPOLLIN ); 
        init();
        return true;
    }

    while(1) {
        // 分散写（从不连续的内存写出去数据）
        temp = writev(m_sockfd, m_iv, m_iv_count);  //返回写字节数
        if ( temp <= -1 ) {
            // 如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件，虽然在此期间，
            // 服务器无法立即接收到同一客户的下一个请求，但可以保证连接的完整性。
            if( errno == EAGAIN ) {
                modfd( m_epollfd, m_sockfd, EPOLLOUT );
                return true;
            }
            unmap();
            return false;
        }
        //读了temp字节数的文件
        bytes_have_send += temp;
        //已发送temp字节数的文件
        bytes_to_send -= temp;
        

        /* v1.0 错误代码
        if ( bytes_to_send <= bytes_have_send ) {
            // 发送HTTP响应成功，根据HTTP请求中的Connection字段决定是否立即关闭连接
            unmap();
            if(m_linger) {
                init();
                modfd( m_epollfd, m_sockfd, EPOLLIN );
                return true;
            } else {
                modfd( m_epollfd, m_sockfd, EPOLLIN );
                return false;
            } 
        }
        */

        // v1.1修改
        if (bytes_have_send >= m_iv[0].iov_len) //判断响应头是否发送完毕，如果可以发送的字节大于报头，证明报头发送完毕
        // bytes_have_send = bytes_have_send + temp;  m_iv[0].iov_len = m_iv[0].iov_len + bytes_have_send;
        //判断条件等价于temp(这一次写入的) >= m_iv[0].iov_len（报头还没写完的），所以这一次写完就说明报头发送完毕
        {
            //头已经发送完毕
            m_iv[0].iov_len = 0;
            /*因为m_write_idx表示为待发送文件的定位点，m_iv[0]指向m_write_buf，
            所以bytes_have_send（已发送的数据量） - m_write_idx（已发送完的报头中的数据量）
            就等于剩余发送文件映射区的起始位置*/
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else
        {
            //如果没有发送完毕，还要修改下次写数据的位置
            m_iv[0].iov_base = m_write_buf + bytes_have_send;  //头还没发送的起始位置
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;  //头还没发送的长度
        }

        if (bytes_to_send <= 0)
        {
            // 没有数据要发送了
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN);

            if (m_linger)
            {
                init();
                return true;
            }
            else
            {
                return false;
            }
        }

    }
}

// 往写缓冲中写入待发送的数据
bool http_conn::add_response( const char* format, ... ) {
    if( m_write_idx >= WRITE_BUFFER_SIZE ) {
        return false;
    }
    va_list arg_list;  //解析参数的
    va_start( arg_list, format );
    int len = vsnprintf( m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list );
    if( len >= ( WRITE_BUFFER_SIZE - 1 - m_write_idx ) ) {
        return false;
    }
    m_write_idx += len;
    va_end( arg_list );
    LOG_INFO("request:%s", m_write_buf);
    Log::get_instance()->flush();
    return true;
}

bool http_conn::add_status_line( int status, const char* title ) {
    return add_response( "%s %d %s\r\n", "HTTP/1.1", status, title );
}

bool http_conn::add_headers(int content_len) {
    add_content_length(content_len);
    add_content_type();
    add_linger();
    add_blank_line();
}

bool http_conn::add_content_length(int content_len) {
    return add_response( "Content-Length: %d\r\n", content_len );
}

bool http_conn::add_linger()
{
    return add_response( "Connection: %s\r\n", ( m_linger == true ) ? "keep-alive" : "close" );
}

bool http_conn::add_blank_line()
{
    return add_response( "%s", "\r\n" );
}

bool http_conn::add_content( const char* content )
{
    return add_response( "%s", content );
}

bool http_conn::add_content_type() {
    return add_response("Content-Type:%s\r\n", "text/html");
}

// 根据服务器处理HTTP请求的结果，决定返回给客户端的内容
bool http_conn::process_write(HTTP_CODE ret) {
    printf("process_write()\n");
    switch (ret)
    {
        case INTERNAL_ERROR:
            add_status_line( 500, error_500_title );
            add_headers( strlen( error_500_form ) );
            if ( ! add_content( error_500_form ) ) {
                return false;
            }
            break;
        case BAD_REQUEST:
            add_status_line( 400, error_400_title );
            add_headers( strlen( error_400_form ) );
            if ( ! add_content( error_400_form ) ) {
                return false;
            }
            break;
        case NO_RESOURCE:
            add_status_line( 404, error_404_title );
            add_headers( strlen( error_404_form ) );
            if ( ! add_content( error_404_form ) ) {
                return false;
            }
            break;
        case FORBIDDEN_REQUEST:
            add_status_line( 403, error_403_title );
            add_headers(strlen( error_403_form));
            if ( ! add_content( error_403_form ) ) {
                return false;
            }
            break;
        case FILE_REQUEST:
            add_status_line(200, ok_200_title );
            add_headers(m_file_stat.st_size);
            m_iv[ 0 ].iov_base = m_write_buf;
            m_iv[ 0 ].iov_len = m_write_idx;
            m_iv[ 1 ].iov_base = m_file_address;
            m_iv[ 1 ].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            bytes_to_send = m_write_idx + m_file_stat.st_size;  //v1.1 响应头的大小+文件的大小，也就是总的要发送的数据,或者说还需要传给socket的数组字节数
            return true;
        default:
            return false;
    }

    m_iv[ 0 ].iov_base = m_write_buf;
    m_iv[ 0 ].iov_len = m_write_idx;
    //v1.1 如果是其他行为，待发送的字节数则就位写缓冲区的数据
    bytes_to_send = m_write_idx;
    m_iv_count = 1;
    return true;
}

// 由线程池中的工作线程调用，这是处理HTTP请求的入口函数
void http_conn::process() {
    // 解析HTTP请求
    printf("process port: %d\n", m_address.sin_port);
    HTTP_CODE read_ret = process_read();
    if ( read_ret == NO_REQUEST ) {
        modfd( m_epollfd, m_sockfd, EPOLLIN );
        return;
    }
    
    // 生成响应
    bool write_ret = process_write( read_ret );
    if ( !write_ret ) {
        close_conn();
    }
    modfd( m_epollfd, m_sockfd, EPOLLOUT);
}