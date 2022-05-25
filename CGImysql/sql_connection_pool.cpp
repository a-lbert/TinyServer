#include <mysql/mysql.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <stdlib.h>
#include <list>
#include <pthread.h>
#include <iostream>
#include "sql_connection_pool.h"

using namespace std;

connection_pool::connection_pool()
{
	m_CurConn = 0;
	m_FreeConn = 0;
}

connection_pool *connection_pool::GetInstance()
{
	static connection_pool connPool;
	return &connPool;
}

//构造初始化
void connection_pool::init(string url, string User, string PassWord, string DBName, int Port, int MaxConn, int close_log)
{
	//初始化数据库信息
	m_url = url;
	m_Port = Port;
	m_User = User;
	m_PassWord = PassWord;
	m_DatabaseName = DBName;
	m_close_log = close_log;

	for (int i = 0; i < MaxConn; i++)	//创建MaxConn条数据库连接
	{
		//初始化函数
		// MYSQL *mysql_init(MYSQL *mysql);
		// mysql 可以传 NULL
		// 返回值：mysql句柄 
		MYSQL *con = NULL;
		con = mysql_init(con);

		if (con == NULL)
		{
			LOG_ERROR("MySQL Error");
			exit(1);
		}

		//连接数据库
		// MYSQL *mysql_real_connect(MYSQL *mysql, const char *host, const char *user, 
        //                   const char *passwd, const char *db, unsigned int port, 
        //                   const char *unix_socket, unsigned long client_flag)
		// mysql 初始化的句柄指针
		// host 主机地址
		// user 用户名 – mysql数据库 root
		// passwd 用户的密码
		// db 要连接的数据库
		// port 端口 一般填0,mysql默认端口 3306
		// unix_socket 本地套接字 ,一般填NULL
		// client_flag 连接标志 一般填0
		// 返回值：成功返回 连接句柄,失败返回 NULL

		con = mysql_real_connect(con, url.c_str(), User.c_str(), PassWord.c_str(), DBName.c_str(), Port, NULL, 0);

		if (con == NULL)
		{
			LOG_ERROR("MySQL Error");
			exit(1);
		}
		connList.push_back(con);	//更新连接池和空闲连接数量
		++m_FreeConn;
	}

	reserve = sem(m_FreeConn);	//将信号量初始化为最大连接次数

	m_MaxConn = m_FreeConn;
}


//当有请求时，从数据库连接池中返回一个可用连接，更新使用和空闲连接数
MYSQL *connection_pool::GetConnection()
{
	MYSQL *con = NULL;

	if (0 == connList.size())
		return NULL;

	reserve.wait();	//取出连接，信号量原子减1，为0则等待
	
	lock.lock();	//由于多线程操作连接池，会造成竞争，这里使用互斥锁完成同步

	con = connList.front();
	connList.pop_front();

	--m_FreeConn;	//这里的两个变量，并没有用到，非常鸡肋...
	++m_CurConn;

	lock.unlock();
	return con;
}

//释放当前使用的连接
bool connection_pool::ReleaseConnection(MYSQL *con)
{
	if (NULL == con)
		return false;

	lock.lock();

	connList.push_back(con);
	++m_FreeConn;
	--m_CurConn;

	lock.unlock();

	reserve.post();		//释放连接原子加1
	return true;
}

//销毁数据库连接池
//通过迭代器遍历连接池链表，关闭对应数据库连接，清空链表并重置空闲连接和现有连接数量
void connection_pool::DestroyPool()
{

	lock.lock();
	if (connList.size() > 0)
	{
		list<MYSQL *>::iterator it;	//通过迭代器遍历，关闭数据库连接
		for (it = connList.begin(); it != connList.end(); ++it)
		{
			//mysql_close（MYSQY* conn）;
			//释放conn连接资源
			MYSQL *con = *it;
			mysql_close(con);
		}
		m_CurConn = 0;
		m_FreeConn = 0;
		connList.clear();	//清空list
	}

	lock.unlock();
}

//当前空闲的连接数
int connection_pool::GetFreeConn()
{
	return this->m_FreeConn;
}

connection_pool::~connection_pool()
{
	DestroyPool();
}
//不直接调用获取和释放连接的接口，将其封装起来，通过RAII机制进行获取和释放
//RAII是Resource Acquisition Is Initialization（wiki上面翻译成 “资源获取就是初始化”）的简称，
//是C++语言的一种管理资源、避免泄漏的惯用法。利用的就是C++构造的对象最终会被销毁的原则。
//RAII的做法是使用一个对象，在其构造时获取对应的资源，在对象生命期内控制对资源的访问，使之始终保持有效，
//最后在对象析构的时候，释放构造时获取的资源。

connectionRAII::connectionRAII(MYSQL **SQL, connection_pool *connPool){
	*SQL = connPool->GetConnection();
	
	conRAII = *SQL;
	poolRAII = connPool;
}

connectionRAII::~connectionRAII(){
	poolRAII->ReleaseConnection(conRAII);
}