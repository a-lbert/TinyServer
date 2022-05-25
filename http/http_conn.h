#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <map>

#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"
#include "../timer/lst_timer.h"
#include "../log/log.h"

class http_conn
{
public:
    ////设置读取文件的名称m_real_file大小
    static const int FILENAME_LEN = 200;
    //设置读缓冲区m_read_buf大小
    static const int READ_BUFFER_SIZE = 2048;
    //设置写缓冲区m_write_buf大小
    static const int WRITE_BUFFER_SIZE = 1024;
    //报文的请求方法，本项目只用到GET和POST
    enum METHOD
    {
        GET = 0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATH
    };
    //主状态机的状态
    enum CHECK_STATE
    {
        //请求行
        CHECK_STATE_REQUESTLINE = 0,
        //请求头部
        CHECK_STATE_HEADER,
        //请求数据
        CHECK_STATE_CONTENT
    };
    //报文解析的结果
    enum HTTP_CODE
    {
        NO_REQUEST,
        GET_REQUEST,
        BAD_REQUEST,
        NO_RESOURCE,
        FORBIDDEN_REQUEST,
        FILE_REQUEST,
        INTERNAL_ERROR,
        CLOSED_CONNECTION
    };
    //从状态机的状态
    enum LINE_STATUS
    {
        LINE_OK = 0,
        LINE_BAD,
        LINE_OPEN
    };

public:
    http_conn() {}
    ~http_conn() {}

public:
    //初始化套接字地址，函数内部会调用私有方法init
    void init(int sockfd, const sockaddr_in &addr, char *, int, int, string user, string passwd, string sqlname);
    //关闭http连接
    oid close_conn(bool real_close = true);
    //处理过程
    void process();
    //读取浏览器端发来的全部数据
    bool read_once();
    //响应报文写入函数
    bool write();
    sockaddr_in *get_address()
    {
        return &m_address;
    }
    //同步线程初始化数据库读取表
    void initmysql_result(connection_pool *connPool);
    int timer_flag;
    int improv;


private:
    void init();
    
    HTTP_CODE process_read(); //从m_read_buf读取，并处理请求报文
    
    bool process_write(HTTP_CODE ret);  //向m_write_buf写入响应报文数据
    
    HTTP_CODE parse_request_line(char *text); //主状态机解析报文中的请求行数据
    
    HTTP_CODE parse_headers(char *text);    //主状态机解析报文中的请求头数据
    
    HTTP_CODE parse_content(char *text);    //主状态机解析报文中的请求内容
    
    HTTP_CODE do_request(); //生成响应报文

           
    //m_start_line是已经解析的字符,get_line用于将指针向后偏移，指向未处理的字符
    char *get_line() { return m_read_buf + m_start_line; };
    
    LINE_STATUS parse_line();   //从状态机读取一行，分析是请求报文的哪一部分

    void unmap();

    //根据响应报文格式，生成对应8个部分，以下函数均由do_request调用
    bool add_response(const char *format, ...);
    bool add_content(const char *content);
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length);
    bool add_content_type();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

public:
    
    static int m_epollfd;   //用来指定主线程中epoll文件描述符
    static int m_user_count;
    MYSQL *mysql;
    int m_state;  //读为0, 写为1

private:
    int m_sockfd;
    sockaddr_in m_address;  //sockaddr_in用于socket定义和赋值
    
    char m_read_buf[READ_BUFFER_SIZE];  //存储读取的请求报文数据
    
    int m_read_idx; //缓冲区中m_read_buf中数据的最后一个字节的下一个位置；即结束位置，用于判边界；
    
    int m_checked_idx; //m_read_buf读取的位置m_checked_idx，从状态机中检查的位置
    
    int m_start_line;   //m_read_buf中已经解析的字符个数，主状态机中应该读取的位置；

    
    char m_write_buf[WRITE_BUFFER_SIZE];    //存储发出的响应报文数据
    
    int m_write_idx;    //指示buffer中的长度

    
    CHECK_STATE m_check_state;  //主状态机的状态
    
    METHOD m_method;    //请求方法，GET 或者 POST

    //以下为解析请求报文中对应的6个变量，在主状态机中存储解析结果
    char m_real_file[FILENAME_LEN]; //存储读取文件的名称
    char *m_url;    //url
    char *m_version;    //http版本
    char *m_host;   //host字段,给出请求资源所在服务器的域名
    int m_content_length;   //Content-Length字段，说明实现主体的大小。
    bool m_linger;

    
    char *m_file_address;   //读取服务器上的文件地址
    struct stat m_file_stat;
    struct iovec m_iv[2]; //io向量机制iovec
    int m_iv_count;
    int cgi;        //是否启用POST,POST请求时，cgi标志位设置为1
    char *m_string; //存储请消息体中用户密码
    int bytes_to_send; //剩余发送字节数
    int bytes_have_send; //已发送字节数
    char *doc_root;

    map<string, string> m_users;
    int m_TRIGMode;
    int m_close_log;

    char sql_user[100];
    char sql_passwd[100];
    char sql_name[100];
};

#endif
