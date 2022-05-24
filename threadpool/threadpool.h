#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"

template <typename T>
class threadpool
{
public:
    /*thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量*/
    threadpool(int actor_model, connection_pool *connPool, int thread_number = 8, int max_request = 10000);
    ~threadpool();
    bool append(T *request, int state);
    bool append_p(T *request);

private:
    /*工作线程运行的函数，它不断从工作队列中取出任务并执行之*/
    static void *worker(void *arg);
    void run();

private:
    int m_thread_number;        //线程池中的线程数
    int m_max_requests;         //请求队列中允许的最大请求数
    pthread_t *m_threads;       //描述线程池的数组，其大小为m_thread_number
    std::list<T *> m_workqueue; //请求队列
    locker m_queuelocker;       //保护请求队列的互斥锁
    sem m_queuestat;            //是否有任务需要处理
    connection_pool *m_connPool;  //数据库
    int m_actor_model;          //模型切换
};
//创建线程池、将对象传递给work函数、并脱离线程；
template <typename T>
threadpool<T>::threadpool( int actor_model, connection_pool *connPool, int thread_number, int max_requests) : m_actor_model(actor_model),m_thread_number(thread_number), m_max_requests(max_requests), m_threads(NULL),m_connPool(connPool)
{
    //判数据范围
    if (thread_number <= 0 || max_requests <= 0)
        throw std::exception();
    //线程id初始化
    m_threads = new pthread_t[m_thread_number];
    if (!m_threads)
        throw std::exception();
    for (int i = 0; i < thread_number; ++i)
    {
//         int pthread_create (pthread_t *thread_tid,                 //设置新生成的线程的id
//                     const pthread_attr_t *attr,         //指向线程属性的指针,通常设置为NULL
//                     void * (*start_routine) (void *),   //处理线程函数的地址
//                     void *arg);                         //start_routine()中的参数

//循环创建线程，并将工作线程按要求进行运行
        if (pthread_create(m_threads + i, NULL, worker, this) != 0)
        {
            delete[] m_threads;
            throw std::exception();
        }
        // int pthread_detach(pthread_t thread);    成功:0;失败:错误号

        // 作用：从状态上实现线程分离，注意不是指该线程独自占用地址空间
        // 线程分离状态：指定该状态，线程主动与主控线程断开关系。线程结束后（不会产生僵尸线程），
        // 其退出状态不由其他线程获取，而直接自己自动释放（自己清理掉PCB的残留资源）。网络、多线程服务器常用
        //将线程进行分离后，不用单独对工作线程进行回收
        if (pthread_detach(m_threads[i]))
        {
            delete[] m_threads;
            throw std::exception();
        }
    }
}

template <typename T>
threadpool<T>::~threadpool()
{
    //删除描述数组
    delete[] m_threads;
}

template <typename T>
bool threadpool<T>::append(T *request, int state)
{
    m_queuelocker.lock();
    //根据硬件，预先设置请求队列的最大值
    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    request->m_state = state;
    //添加任务
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    //信号量提醒有任务要处理
    m_queuestat.post();
    return true;
}

template <typename T>
bool threadpool<T>::append_p(T *request)
{
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}

template <typename T>
//接收线程池中线程id、转化为线程池对象，调用run（）方法；
void *threadpool<T>::worker(void *arg)
{
    ////将参数强转为线程池类，调用成员方法
    threadpool *pool = (threadpool *)arg;
    pool->run();
    return pool;
}

template <typename T>
//处理函数，涉及后边实现，后头再细看
void threadpool<T>::run()
{
    while (true)
    {
        //等待信号量
        m_queuestat.wait();
        //被唤醒后先加请求队列互斥锁
        m_queuelocker.lock();
        //判断队列非空
        if (m_workqueue.empty())
        {
            m_queuelocker.unlock();
            continue;
        }
        //取出队列头
        T *request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();//解锁
        if (!request)//判非空
            continue;
        //m_actor_model 控制模型实现
        if (1 == m_actor_model)
        {
            if (0 == request->m_state)
            {
                if (request->read_once())
                {
                    request->improv = 1;
                    //数据库操作
                    connectionRAII mysqlcon(&request->mysql, m_connPool);
                    //process(模板类中的方法,这里是http类)进行处理
                    request->process();
                }
                else
                {
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
            else
            {
                if (request->write())
                {
                    request->improv = 1;
                }
                else
                {
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
        }

        //另一种模式
        else
        {
            //数据库操作
            connectionRAII mysqlcon(&request->mysql, m_connPool);
            //process(模板类中的方法,这里是http类)进行处理
            request->process();
        }
    }
}
#endif
