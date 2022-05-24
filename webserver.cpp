#include "webserver.h"

WebServer::WebServer()
{
    //http_conn类对象
    users = new http_conn[MAX_FD];

    //root文件夹路径
    char server_path[200];
    getcwd(server_path, 200);
    char root[6] = "/root";
    m_root = (char *)malloc(strlen(server_path) + strlen(root) + 1);
    strcpy(m_root, server_path);
    strcat(m_root, root);

    //定时器
    users_timer = new client_data[MAX_FD];  //创建连接资源数组
}

WebServer::~WebServer()
{
    close(m_epollfd);
    close(m_listenfd);
    close(m_pipefd[1]);
    close(m_pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete m_pool;
}

void WebServer::init(int port, string user, string passWord, string databaseName, int log_write, 
                     int opt_linger, int trigmode, int sql_num, int thread_num, int close_log, int actor_model)
{
    m_port = port;
    m_user = user;
    m_passWord = passWord;
    m_databaseName = databaseName;
    m_sql_num = sql_num;
    m_thread_num = thread_num;
    m_log_write = log_write;
    m_OPT_LINGER = opt_linger;
    m_TRIGMode = trigmode;
    m_close_log = close_log;
    m_actormodel = actor_model;
}

void WebServer::trig_mode()
{
    //LT + LT
    if (0 == m_TRIGMode)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 0;
    }
    //LT + ET
    else if (1 == m_TRIGMode)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 1;
    }
    //ET + LT
    else if (2 == m_TRIGMode)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 0;
    }
    //ET + ET
    else if (3 == m_TRIGMode)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 1;
    }
}

void WebServer::log_write()
{
    if (0 == m_close_log)
    {
        //初始化日志；单例模式，懒汉模式获取实例
        if (1 == m_log_write)   //异步模式判断是否分文件
            //格式化输出内容，将内容写入阻塞队列，创建一个写线程，从阻塞队列取出内容写入日志文件
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 800);
        else    //同步模式
        //判断是否分文件
        //直接格式化输出内容，将信息写入日志文件
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 0);
    }
}

void WebServer::sql_pool()
{
    //初始化数据库连接池
    m_connPool = connection_pool::GetInstance();
    m_connPool->init("localhost", m_user, m_passWord, m_databaseName, 3306, m_sql_num, m_close_log);

    //初始化数据库读取表
    users->initmysql_result(m_connPool);
}

void WebServer::thread_pool()
{
    //线程池
    m_pool = new threadpool<http_conn>(m_actormodel, m_connPool, m_thread_num);
}

void WebServer::eventListen()
{
    //网络编程基础步骤
    m_listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(m_listenfd >= 0);

    //优雅关闭连接
    if (0 == m_OPT_LINGER)
    {
        struct linger tmp = {0, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }
    else if (1 == m_OPT_LINGER)
    {
        struct linger tmp = {1, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }

    int ret = 0;
    //struct sockaddr 与 struct sockaddr_in
    // struct sockaddr // sockaddr用于函数参数
    //     {
    //         sa_family_t sin_family;//地址族
    //         char sa_data[14]; //14字节，包含套接字中的目标地址和端口信息
    //     }; 
    // struct sockaddr_in   //sockaddr_in用于socket定义和赋值
    //     {
    //         sa_family_t sin_family;//地址族
    //         uint16_t sin_port;//16位TCP/UDP端口号
    //         struct in_addr sin_addr;//32位ip地址
    //         char sin_zero[8];//留用
    //     };
    // sockaddr_in 是internet环境下套接字的地址形式。所以在网络编程中我们会对sockaddr_in结构体进行操作，
    // 使用sockaddr_in来建立所需的信息，最后使用类型转化就可以了。一般先把sockaddr_in变量赋值后，
    // 强制类型转换后传入用sockaddr做参数的函数。
    

    struct sockaddr_in address;

    bzero(&address, sizeof(address)); //内存清0
    address.sin_family = AF_INET; //地址族
    address.sin_addr.s_addr = htonl(INADDR_ANY);    //ip地址
    address.sin_port = htons(m_port); // 端口号

    int flag = 1;
    setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
    ret = bind(m_listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);
    ret = listen(m_listenfd, 5);    //让一个套接字处于监听到来的连接请求的状态
    assert(ret >= 0);

    utils.init(TIMESLOT);

    //epoll创建内核事件表
    epoll_event events[MAX_EVENT_NUMBER];
    m_epollfd = epoll_create(5);
    assert(m_epollfd != -1);

    utils.addfd(m_epollfd, m_listenfd, false, m_LISTENTrigmode);
    http_conn::m_epollfd = m_epollfd;

    //int socketpair(int domain, int type, int protocol, int sv[2]);
    //socketpair()函数用于创建一对无名的、相互连接的套接字
    // domain表示协议族，PF_UNIX或者AF_UNIX
    // type表示协议，可以是SOCK_STREAM或者SOCK_DGRAM，SOCK_STREAM基于TCP，SOCK_DGRAM基于UDP
    // protocol表示类型，只能为0
    // sv[2]表示套节字柄对，该两个句柄作用相同，均能进行读写双向操作
    // 返回结果， 0为创建成功，-1为创建失败,错误码保存于errno中
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd);    //创建管道套接字

    assert(ret != -1);
    utils.setnonblocking(m_pipefd[1]);  //设置管道写端为非阻塞，为什么写端要非阻塞？
    //send是将信息发送给套接字缓冲区，如果缓冲区满了，则会阻塞，这时候会进一步增加信号处理函数的执行时间，
    //为此，将其修改为非阻塞。
    utils.addfd(m_epollfd, m_pipefd[0], false, 0);  //设置管道读端为ET非阻塞
    //没有对非阻塞返回值处理，如果阻塞是不是意味着这一次定时事件失效了？
    //是的，但定时事件是非必须立即处理的事件，可以允许这样的情况发生

    //SIGPIPE信号的缺省处理方法是退出进程，大多数时候这都不是我们期望的。因此我们需要重载这个信号的处理方法。
    utils.addsig(SIGPIPE, SIG_IGN); //忽略SIGPIPE信号，避免进程退出
    utils.addsig(SIGALRM, utils.sig_handler, false);    //传递给主循环的信号值，这里只关注SIGALRM和SIGTERM
    utils.addsig(SIGTERM, utils.sig_handler, false);

    //后续用epoll监听，发生事件是调用dealwithsignal；goto L283；


    // alarm函数是设置一个计时器, 在计时器超时的时候, 产生SIGALRM信号. 
    // alarm也称为闹钟函数，一个进程只能有一个闹钟时间。
    // 如果不忽略或捕捉此信号, 它的默认操作是终止调用该alarm函数的进程
    alarm(TIMESLOT);    //每隔TIMESLOT时间触发SIGALRM信号

    //工具类,信号和描述符基础操作
    Utils::u_pipefd = m_pipefd;
    Utils::u_epollfd = m_epollfd;
    
}

void WebServer::timer(int connfd, struct sockaddr_in client_address)
{   
    
    users[connfd].init(connfd, client_address, m_root, m_CONNTrigmode, m_close_log, m_user, m_passWord, m_databaseName);

    //初始化client_data数据
    //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中

    //初始化该连接对应的连接资源
    users_timer[connfd].address = client_address;
    users_timer[connfd].sockfd = connfd;

    util_timer *timer = new util_timer; //创建定时器临时变量
    timer->user_data = &users_timer[connfd];    //设置定时器对应的连接资源
    timer->cb_func = cb_func;   //设置回调函数

    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT; //设置绝对超时时间；15s；
    users_timer[connfd].timer = timer;  //创建该连接对应的定时器，初始化为前述临时变量
    utils.m_timer_lst.add_timer(timer); //将该定时器添加到链表中
}

//若有数据传输，则将定时器往后延迟3个单位
//并对新的定时器在链表上的位置进行调整
void WebServer::adjust_timer(util_timer *timer)
{
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    utils.m_timer_lst.adjust_timer(timer);

    LOG_INFO("%s", "adjust timer once");
}

void WebServer::deal_timer(util_timer *timer, int sockfd)
{
    timer->cb_func(&users_timer[sockfd]);   //回调函数
    if (timer)
    {
        utils.m_timer_lst.del_timer(timer);   //删除定时器
    }

    LOG_INFO("close fd %d", users_timer[sockfd].sockfd);
}

bool WebServer::dealclinetdata()
{
    struct sockaddr_in client_address;  //初始化客户端连接地址
    socklen_t client_addrlength = sizeof(client_address);

    if (0 == m_LISTENTrigmode)  //触发模式；暂时还不清楚代表啥？
    {   
        //该连接分配的文件描述符
        int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);    
        if (connfd < 0)
        {
            LOG_ERROR("%s:errno is:%d", "accept error", errno);
            return false;
        }
        if (http_conn::m_user_count >= MAX_FD)
        {
            utils.show_error(connfd, "Internal server busy");
            LOG_ERROR("%s", "Internal server busy");
            return false;
        }
        timer(connfd, client_address);
    }

    else
    {
        while (1)
        {
            int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
            if (connfd < 0)
            {
                LOG_ERROR("%s:errno is:%d", "accept error", errno);
                break;
            }
            if (http_conn::m_user_count >= MAX_FD)
            {
                utils.show_error(connfd, "Internal server busy");
                LOG_ERROR("%s", "Internal server busy");
                break;
            }
            timer(connfd, client_address); //初始化定时器、socket等；
        }
        return false;
    }
    return true;
}

bool WebServer::dealwithsignal(bool &timeout, bool &stop_server)
{
    int ret = 0;
    int sig;
    char signals[1024];
    
    

    // 函数原型：int recv(int sockfd, void *buf, int len, int flags)
    // 函数功能：用来接收远程主机通过套接字sockfd发送来的数据，并把这些数据保存到数组buf中。
    // 参数说明：
    // （1） sockfd：建立连接的套接字
    // （2） buf：接收到的数据保存在该数组中
    // （3） len：数组的长度
    // （4） flags：一般设置为0
    //返回值：> 0 : 表示执行成功，返回实际接收到的字符个数
            // = 0 : 另一端关闭此连接
            // < 0 : 执行失败,可以通过errno来捕获错误原因（errno.h）
    ret = recv(m_pipefd[0], signals, sizeof(signals), 0);   //从管道读端读出信号值，成功返回字节数，失败返回-1
    //正常情况下，这里的ret返回值总是1，只有14和15两个ASCII码对应的字符

    //send(u_pipefd[1], (char *)&msg, 1, 0);  //将信号值从管道写端写入，传输字符类型，而非整型
    //读出的是字符；

    if (ret == -1) //失败
    {
        return false;   // handle the error
    }
    else if (ret == 0) //关闭连接
    {
        return false;
    }
    else    //处理信号值对应的逻辑
    {
        for (int i = 0; i < ret; ++i)
        {
            //switch的变量一般为字符或整型，当switch的变量为字符时，case中可以是字符，也可以是字符对应的ASCII码
            switch (signals[i]) //这里面明明是字符
            {   
            case SIGALRM:   //这里是整型
            {
                timeout = true;
                break;
            }
            case SIGTERM:
            {
                stop_server = true;
                break;
            }
            }
        }
    }
    return true;
}

void WebServer::dealwithread(int sockfd)
{
    util_timer *timer = users_timer[sockfd].timer;  //创建定时器临时变量，将该连接对应的定时器取出来

    //reactor
    if (1 == m_actormodel)
    {
        if (timer)
        {
            adjust_timer(timer);
        }

        //若监测到读事件，将该事件放入请求队列
        m_pool->append(users + sockfd, 0);

        while (true)
        {
            if (1 == users[sockfd].improv)
            {
                if (1 == users[sockfd].timer_flag)
                {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }
    else
    {
        //proactor
        if (users[sockfd].read_once())  //若监测到读事件
        {
            LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            
            m_pool->append_p(users + sockfd);   //将该事件放入请求队列

            if (timer)  //若有数据传输，则将定时器往后延迟3个单位,对其在链表上的位置进行调整
            {
                adjust_timer(timer);
            }
        }
        else    //服务器端关闭连接，移除对应的定时器
        {
            deal_timer(timer, sockfd);
        }
    }
}

void WebServer::dealwithwrite(int sockfd)
{
    util_timer *timer = users_timer[sockfd].timer;
    
    if (1 == m_actormodel)  //reactor模式
    {
        if (timer)  //若有数据传输，则将定时器往后延迟3个单位，并对新的定时器在链表上的位置进行调整
        {
            adjust_timer(timer);
        }

        m_pool->append(users + sockfd, 1);

        while (true)
        {
            if (1 == users[sockfd].improv)
            {
                if (1 == users[sockfd].timer_flag)
                {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }
    else    //proactor模式
    {
        
        if (users[sockfd].write())  //检测到写事件
        {
            LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            if (timer)
            {
                adjust_timer(timer);
            }
        }
        else    //服务器端关闭连接，移除对应的定时器
        {
            deal_timer(timer, sockfd);
        }
    }
}

void WebServer::eventLoop()
{
    bool timeout = false;   //超时标志,默认为False
    bool stop_server = false;   //循环条件

    while (!stop_server)
    {
        int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);   //监测发生事件的文件描述符
        if (number < 0 && errno != EINTR)
        {
            LOG_ERROR("%s", "epoll failure");
            break;
        }

        for (int i = 0; i < number; i++)    //轮询文件描述符
        {
             
            
            if (sockfd == m_listenfd)   //处理新到的客户连接
            {
                bool flag = dealclinetdata();   //处理函数
                if (false == flag)
                    continue;
            }
            
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) //处理异常事件
            {
                //服务器端关闭连接，移除对应的定时器
                util_timer *timer = users_timer[sockfd].timer;
                deal_timer(timer, sockfd);
            }
            //处理信号管道读端对应文件描述符发生读事件（处理定时器信号）
            else if ((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN))
            {
                bool flag = dealwithsignal(timeout, stop_server);
                if (false == flag)
                    LOG_ERROR("%s", "dealclientdata failure");
            }
            
            else if (events[i].events & EPOLLIN)    //处理客户连接上接收到的数据
            {
                dealwithread(sockfd);
            }
            else if (events[i].events & EPOLLOUT)   //处理写数据
            {
                dealwithwrite(sockfd);
            }
        }
        if (timeout)    //处理定时器为非必须事件，收到信号并不是立马处理，完成读写事件后，再进行处理
        {
            utils.timer_handler();

            LOG_INFO("%s", "timer tick");

            timeout = false;
        }
    }
}