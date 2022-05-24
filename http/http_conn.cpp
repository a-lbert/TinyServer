#include "http_conn.h"

#include <mysql/mysql.h>
#include <fstream>

//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

locker m_lock;
map<string, string> users;

void http_conn::initmysql_result(connection_pool *connPool)
{
    //先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    //在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

//对文件描述符设置非阻塞
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//从内核时间表删除描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

//将事件重置为EPOLLONESHOT
void modfd(int epollfd, int fd, int ev, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

//关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        printf("close %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

//初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode,
                     int close_log, string user, string passwd, string sqlname)
{
    m_sockfd = sockfd;
    m_address = addr;

    addfd(m_epollfd, sockfd, true, m_TRIGMode);
    m_user_count++;

    //当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    doc_root = root;
    m_TRIGMode = TRIGMode;
    m_close_log = close_log;

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    init();
}

//初始化新接受的连接
//check_state默认为分析请求行状态
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    m_state = 0;
    timer_flag = 0;
    improv = 0;

    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

//从状态机，用于分析出一行内容
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    //m_read_idx指向缓冲区m_read_buf的数据末尾的下一个字节；即结束位置，判边界
    //m_checked_idx指向从状态机当前正在分析的字节
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];   //temp为将要分析的字节
        if (temp == '\r')   //如果当前是\r字符，则有可能会读取到完整行
        {   
            if ((m_checked_idx + 1) == m_read_idx)  //下一个字符达到了buffer结尾，则接收不完整，需要继续接收
                return LINE_OPEN;
            else if (m_read_buf[m_checked_idx + 1] == '\n') //下一个字符是\n，将\r\n改为\0\0
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;    //如果都不符合，则返回语法错误
        }
        else if (temp == '\n')  //如果当前字符是\n，也有可能读取到完整行
        //一般是上次读取到\r就到buffer末尾了，没有接收完整，再次接收时会出现这种情况

        {
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r') //前一个字符是\r，则接收完整
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

//循环读取客户数据，直到无数据可读或对方关闭连接
//非阻塞ET工作模式下，需要一次性将数据读完
bool http_conn::read_once()
{
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytes_read = 0;

    //LT读取数据
    if (0 == m_TRIGMode)
    {
        // int recv(int sockfd, void *buf, int len, int flags)
        // 用来接收远程主机通过套接字sockfd发送来的数据，并把这些数据保存到数组buf中
        // 参数：
        //     （1） sockfd：建立连接的套接字
        //     （2） buf：接收到的数据保存在该数组中
        //     （3） len：数组的长度
        //     （4） flags：一般设置为0
        // 返回值：
        //     > 0 : 表示执行成功，返回实际接收到的字符个数
        //     = 0 : 另一端关闭此连接
        //     < 0 : 执行失败,可以通过errno来捕获错误原因（errno.h）
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read;

        if (bytes_read <= 0)
        {
            return false;
        }

        return true;
    }
    //ET读数据
    else
    {
        while (true)
        {
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                return false;
            }
            else if (bytes_read == 0)
            {
                return false;
            }
            m_read_idx += bytes_read;
        }
        return true;
    }
}

//解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    //在HTTP报文中，请求行用来说明请求类型,要访问的资源以及所使用的HTTP版本，其中各个部分之间通过\t或空格分隔。
    
    //char *strpbrk(const char *str1, const char *str2) 
    //检索字符串 str1 中第一个匹配字符串 str2 中字符的字符，不包含空结束字符;未找到返回null；
    m_url = strpbrk(text, " \t");   //请求行中最先含有空格和\t任一字符的位置并返回
    if (!m_url) //如果没有空格或\t，则报文格式有误
    {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';    //将该位置改为\0，用于将前面数据取出
    char *method = text;    //取出数据，并通过与GET和POST比较，以确定请求方式

    // string s = "aaa\0a";
	// char *p = &s[0];
    // cout << p;       结果：aaa

    //strcasecmp用忽略大小写比较字符串
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else
        return BAD_REQUEST;
    
    //size_t strspn(const char *str1, const char *str2) 
    //检索字符串 str1 中第一个不在字符串 str2 中出现的字符下标
    m_url += strspn(m_url, " \t");  //m_url此时跳过了第一个空格或\t字符，但不知道之后是否还有
    //将m_url向后偏移，通过查找，继续跳过空格和\t字符，指向请求资源的第一个字符
    
    m_version = strpbrk(m_url, " \t");  //使用与判断请求方式的相同逻辑，判断HTTP版本号
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");

    if (strcasecmp(m_version, "HTTP/1.1") != 0) //仅支持HTTP/1.1
        return BAD_REQUEST;
    
    //对请求资源前7个字符进行判断
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        //char *strchr(const char *str, int c) 
        //在参数 str 所指向的字符串中搜索第一次出现字符 c（一个无符号字符）的位置
        m_url = strchr(m_url, '/');
    }

    if (strncasecmp(m_url, "https://", 8) == 0) //同样增加https情况
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }

    if (!m_url || m_url[0] != '/')  //一般的不会带有上述两种符号，直接是单独的/或/后面带访问资源
        return BAD_REQUEST;
    //当url为/时，显示判断界面
    if (strlen(m_url) == 1) //此时长度为1；只有这一种GET方式；其余登录以及注册改用POST方式（避免暴露在请求行中）；
        strcat(m_url, "judge.html");
    m_check_state = CHECK_STATE_HEADER; //请求行处理完毕，将主状态机转移处理请求头
    return NO_REQUEST;
}

//解析http请求的一个头部信息,空行、请求头共用一个函数
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    if (text[0] == '\0') //判断当前的text首位是不是\0字符,是则表示当前处理的是空行，不则表示当前处理的是请求头
    {
        if (m_content_length != 0)  //判断是GET还是POST请求
        //如果不是0，表明是POST请求，则状态转移到CHECK_STATE_CONTENT，否则说明是GET请求，则报文解析结束。
        {
            m_check_state = CHECK_STATE_CONTENT;    //POST需要跳转到消息体处理状态
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    //处理请求头
    else if (strncasecmp(text, "Connection:", 11) == 0) //分析connection字段，是keep-alive还是close，决定是长连接还是短连接
    {
        text += 11;
        text += strspn(text, " \t");    //跳过空格和\t字符
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;    //如果是长连接，则将linger标志设置为true
        }
    }
    else if (strncasecmp(text, "Content-length:", 15) == 0) //分析Content-length字段，读取post请求的消息体长度
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    else if (strncasecmp(text, "Host:", 5) == 0)    //分析Host字段
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else
    {
        LOG_INFO("oop!unknow header: %s", text);
    }
    return NO_REQUEST;
}

//判断http请求是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    //判断buffer中是否读取了消息体
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        //POST请求中最后为输入的用户名和密码
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;
    //判断条件：主状态机转移到CHECK_STATE_CONTENT且line_status为 LINE_OK则解析消息体；
    // 从状态机转移到LINE_OK则解析请求行和请求头部
    //两者为或关系，当条件为真则继续循环，否则退出;while中根据主状态机状态分别处理，成功解析后调用do_request()
    //生成相应，并返回FILE_REQUEST（正确情况，并不处理）   *********注意此处调用parse_line();parse_line为从状态机的具体实现***************
    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        //m_start_line是已经解析的字符,get_line用于将指针向后偏移，指向未处理的字符
        //char *get_line() { return m_read_buf + m_start_line; };
        text = get_line();
        m_start_line = m_checked_idx;   //m_start_line是每一个数据行在m_read_buf中的起始位置
        //m_checked_idx表示从状态机在m_read_buf中读取的位置

        LOG_INFO("%s", text);
        switch (m_check_state)  //主状态机的三种状态转移逻辑
        {
        case CHECK_STATE_REQUESTLINE:   //解析请求行
        {
            ret = parse_request_line(text); 
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;
        }
        case CHECK_STATE_HEADER:    //解析请求头
        {
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)    //HTTP请求报文有语法错误
                return BAD_REQUEST;
            else if (ret == GET_REQUEST)    //获得了完整的HTTP请求
            {
                return do_request();    //完整解析GET请求后，跳转到报文响应函数
            }
            break;
        }
        case CHECK_STATE_CONTENT:   //解析消息体
        {
            ret = parse_content(text);
            if (ret == GET_REQUEST)    //获得了完整的HTTP请求
                return do_request();    //完整解析POST请求后，跳转到报文响应函数
            line_status = LINE_OPEN;    //解析完消息体即完成报文解析，避免再次进入循环，更新line_status（回看while第一个条件）
            break;
        }
        default:
            return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::do_request()
{
//****************************************************************************************************
    strcpy(m_real_file, doc_root);  //将初始化的m_real_file赋值为网站根目录; doc_root初始化为root
    int len = strlen(doc_root);
    //printf("m_url:%s\n", m_url);

    //char *strrchr(const char *str, int c) 
    //在参数 str 所指向的字符串中搜索最后一次出现字符 c（一个无符号字符）的位置
    const char *p = strrchr(m_url, '/');

    //处理cgi，实现登录和注册校验
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {

        //根据标志判断是登录检测还是注册检测
        char flag = m_url[1];   //2是登录；3是注册；

        //以下操作是根据url中信息获得m_real_file，即存储读取文件的名称
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        //char *strcpy(char *dest, const char *src) 把 src 所指向的字符串复制到 dest
        strcpy(m_url_real, "/");
        //char *strcat(char *dest, const char *src) 把 src 所指向的字符串追加到 dest 所指向的字符串的结尾
        strcat(m_url_real, m_url + 2);
        //char *strncpy(char *dest, const char *src, size_t n) 把 src 所指向的字符串复制到 dest，
        //最多复制 n 个字符。当 src 的长度小于 n 时，dest 的剩余部分将用空字节填充
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);

        //将用户名和密码提取出来
        //user=123&passwd=123 ？ 是password吧？
        char name[100], password[100];
        int i;
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';
        //注册
        if (*(p + 1) == '3')
        {
            //如果是注册，先检测数据库中是否有重名的
            //没有重名的，进行增加数据
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");
            //map<string, string> users; 存储用户名、密码；用于用户查重；避免频繁访问数据库；
            if (users.find(name) == users.end())
            {
                m_lock.lock();
                //数据库查询语句；成功返回True；
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();

                if (!res)
                    strcpy(m_url, "/log.html");
                else    //用户已存在
                    strcpy(m_url, "/registerError.html");
            }
            else    //用户已经存在
                strcpy(m_url, "/registerError.html");
        }
        //如果是登录，直接判断
        //若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2')
        {
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html");
            else
                strcpy(m_url, "/logError.html");
        }
    }

    if (*(p + 1) == '0')    //注册界面
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");   //将网站目录和/register.html进行拼接，更新到m_real_file中
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '1')   //登录界面
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");    //将网站目录和/log.html进行拼接，更新到m_real_file中
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '5')   //请求图片
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '6')   //  请求视频
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '7')   //请求关注页面，嵌入图片；
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else            //如果以上均不符合，即不是登录和注册，直接将url与网站目录拼接
    //这里的情况是welcome界面，请求服务器上的一个图片；即judge.html
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
//*************************************************************************************************
//以上**与**之间为根据url内容解析得到m_real_file，即请求文件在服务器的路径；
//以下是判断文件属性以及内存映射；
    //int stat(const char *path, struct stat *buf)
    //获取文件信息（类型、权限、大小）；成功返回0；失败-1；
    // struct stat 
    //     {
    //     mode_t    st_mode;        /* 文件类型和权限 */
    //     off_t     st_size;        /* 文件大小，字节数*/
    //     };

    if (stat(m_real_file, &m_file_stat) < 0)    //通过stat获取请求资源文件信息,成功则将信息更新到m_file_stat结构体
        return NO_RESOURCE; //失败返回NO_RESOURCE状态，表示资源不存在

    if (!(m_file_stat.st_mode & S_IROTH))   //判断文件的权限，是否可读，不可读则返回FORBIDDEN_REQUEST状态
        return FORBIDDEN_REQUEST;

    if (S_ISDIR(m_file_stat.st_mode))   //判断文件类型，如果是目录，则返回BAD_REQUEST，表示请求报文有误
        return BAD_REQUEST;

    int fd = open(m_real_file, O_RDONLY);   //以只读方式获取文件描述符，通过mmap将该文件映射到内存中
    
        //void* mmap(void* start,size_t length,int prot,int flags,int fd,off_t offset);
        // start：映射区的开始地址，设置为0时表示由系统决定映射区的起始地址
        // length：映射区的长度
        // prot：期望的内存保护标志，不能与文件的打开模式冲突
            // PROT_READ 表示页内容可以被读取
        // flags：指定映射对象的类型，映射选项和映射页是否可以共享
            // MAP_PRIVATE 建立一个写入时拷贝的私有映射，内存区域的写入不会影响到原文件
        // fd：有效的文件描述符，一般是由open()函数返回
        // off_toffset：被映射对象内容的起点
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);  //避免文件描述符的浪费和占用

    return FILE_REQUEST;    //表示请求文件存在，且可以访问
}

void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}
bool http_conn::write()
{
    int temp = 0;

    if (bytes_to_send == 0) //若要发送的数据长度为0,表示响应报文为空，一般不会出现这种情况
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        init();
        return true;
    }

    while (1)
    {   //writev将多个数据存储在一起，将驻留在两个或更多的不连接的缓冲区中的数据一次写出去
        //返回值：传输字节数，出错时返回-1
        temp = writev(m_sockfd, m_iv, m_iv_count);  //将响应报文的状态行、消息头、空行和响应正文发送给浏览器端

        if (temp < 0)   //非正常发送，temp为-1
        {
            if (errno == EAGAIN)    //判断缓冲区是否满了
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);   //重新注册写事件
                return true;
            }
            unmap();    //如果发送失败，但不是缓冲区问题，取消映射
            return false;
        }
        //正常发送，temp为发送的字节数
        bytes_have_send += temp;
        bytes_to_send -= temp;
        //根据写的字节数更新不同区域的起始地址以及大小；
        if (bytes_have_send >= m_iv[0].iov_len)
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        if (bytes_to_send <= 0) //判断条件，数据已全部发送完
        {
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);    //在epoll树上重置EPOLLONESHOT事件

            if (m_linger)   //浏览器的请求为长连接
            {
                init(); //重新初始化HTTP对象，不关闭连接   
                return true;
            }
            else
            {
                return false;   //直接关闭连接
            }
        }
    }
}
bool http_conn::add_response(const char *format, ...)
{
    if (m_write_idx >= WRITE_BUFFER_SIZE)   //如果写入内容超出m_write_buf大小则报错
        return false;
    
    //va_list 解决变参问题的一组宏，所在头文件：#include <stdarg.h>，用于获取不确定个数的参数。
    va_list arg_list;   //定义可变参数列表;一个字符指针，可以理解为指向当前参数的一个指针，取参必须通过这个指针进行
    //void va_start ( va_list ap, prev_param )
    //对ap进行初始化，让ap指向可变参数表里面的第一个参数;
    //第一个参数是 ap 本身，第二个参数是在变参表前面紧挨着的一个变量，即“...”之前的那个参数
    va_start(arg_list, format); //将变量arg_list初始化为传入参数

    //将数据format从可变参数列表写入缓冲区写，返回写入数据的长度
    //int vsnprintf(char* str, size_t size, const char* format, va_list ap) 将可变参数格式化输出到一个字符数组
    
    // char *str [out],把生成的格式化的字符串存放在这里.
    // size_t size [in], str可接受的最大字符数 [1]  (非字节数，UNICODE一个字符两个字节),防止产生数组越界.
    // const char *format [in], 指定输出格式的字符串，它决定了你需要提供的可变参数的类型、个数和顺序。
    // va_list ap [in], va_list变量. va:variable-argument:可变参数

    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))   //如果写入的数据长度超过缓冲区剩余空间，则报错
    {
        va_end(arg_list);
        return false;
    }
    m_write_idx += len; //更新m_write_idx位置

    //void va_end ( va_list ap );
    //va_end：释放指针，将输入的参数 ap 置为 NULL。通常va_start和va_end是成对出现
    va_end(arg_list);   //清空可变参列表

    LOG_INFO("request:%s", m_write_buf);

    return true;
}
bool http_conn::add_status_line(int status, const char *title)//添加状态行
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
bool http_conn::add_headers(int content_len)    //添加消息报头，具体的添加文本长度、连接状态和空行
{
    return add_content_length(content_len) && add_linger() &&
           add_blank_line();
}
bool http_conn::add_content_length(int content_len) //添加Content-Length，表示响应报文的长度
{
    return add_response("Content-Length:%d\r\n", content_len);
}
bool http_conn::add_content_type()  //添加文本类型，这里是html
{
    return add_response("Content-Type:%s\r\n", "text/html");
}
bool http_conn::add_linger()    //添加连接状态，通知浏览器端是保持连接还是关闭
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}
bool http_conn::add_blank_line()    //添加空行
{
    return add_response("%s", "\r\n");
}
bool http_conn::add_content(const char *content)    //添加文本content
{
    return add_response("%s", content);
}
bool http_conn::process_write(HTTP_CODE ret)
{
    // 响应报文分为两种，一种是请求文件的存在，case FORBIDDEN_REQUEST，通过io向量机制iovec，声明两个iovec，
        // 第一个指向m_write_buf，第二个指向mmap的地址m_file_address；
    // 一种是请求出错，这时候只申请一个iovec，指向m_write_buf。
    switch (ret)
    {
    case INTERNAL_ERROR:    //服务器内部错误，500
    {
        add_status_line(500, error_500_title);  //状态行
        add_headers(strlen(error_500_form));    //消息报头
        if (!add_content(error_500_form))
            return false;
        break;
    }
    case BAD_REQUEST:   //报文语法有误，404
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }
    case FORBIDDEN_REQUEST:     //资源没有访问权限，403
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }
    case FILE_REQUEST:  //请求资源正常访问，200
    {
        add_status_line(200, ok_200_title); //增加状态行
        if (m_file_stat.st_size != 0)   //如果请求的资源存在
        {
            //写的时候，先写状态行、报头、状态消息，即m_iv[0];再写文件映射区，即m_iv[1];二者合成响应报文；
            add_headers(m_file_stat.st_size);   //增加头部及空行
            m_iv[0].iov_base = m_write_buf; //第一个iovec指针指向响应报文缓冲区，长度指向m_write_idx
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address;  //第二个iovec指针指向mmap返回的文件指针，长度指向文件大小
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            bytes_to_send = m_write_idx + m_file_stat.st_size;  //发送的全部数据为响应报文头部信息和文件大小
            return true;
        }
        else
        {
            //如果请求的资源大小为0，则返回空白html文件
            const char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
                return false;
        }
    }
    default:
        return false;
    }
    //除FILE_REQUEST状态外，其余状态只申请一个iovec，指向响应报文缓冲区
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}
void http_conn::process()
{
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST) //NO_REQUEST，表示请求不完整，需要继续接收请求数据
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);    //注册并监听读事件
        return;
    }
    bool write_ret = process_write(read_ret);   //根据do_request的返回状态，服务器子线程调用process_write向m_write_buf中写入响应报文
    if (!write_ret)
    {
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);   //注册并监听写事件
}
