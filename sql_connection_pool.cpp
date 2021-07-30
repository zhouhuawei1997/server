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

connection_pool::connection_pool()  //构造函数
{
	this->CurConn = 0;  //已使用连接数
	this->FreeConn = 0;  //空闲连接数
}

connection_pool *connection_pool::GetInstance()  //单例模式
{
	static connection_pool connPool;
	return &connPool;
}

//初始化
void connection_pool::init(string url, string User, string PassWord, string DBName, int Port, unsigned int MaxConn)
{
	this->url = url;  //主机地址
	this->Port = Port;  //端口
	this->User = User;  //用户
	this->PassWord = PassWord;  //密码
	this->DatabaseName = DBName;  //库名

	lock.lock();
	for (int i = 0; i < MaxConn; i++)  //MaxConn为最大连接数 创建MaxConn条连接
	{
		cout<<"初始化第"<<i<<"个连接！"<<endl;
		MYSQL *con = NULL;
		con = mysql_init(con);

		if (con == NULL)
		{
			cout << "Error:" << mysql_error(con);
			exit(1);
		}
		con = mysql_real_connect(con, url.c_str(), User.c_str(), PassWord.c_str(), DBName.c_str(), Port, NULL, 0);  //连接到数据库

		if (con == NULL)
		{
			cout << "Error: " << mysql_error(con);
			exit(1);
		}
		connList.push_back(con);  //连接池 list
		++FreeConn;  //空闲连接数 +1
	}

	reserve = sem(FreeConn);  //将信号量初始化为最大连接次数

	this->MaxConn = FreeConn;  //最大连接数
	
	lock.unlock();
}


//当有请求时，从数据库连接池中返回一个可用连接，更新使用和空闲连接数
MYSQL *connection_pool::GetConnection()
{
	MYSQL *con = NULL;

	if (0 == connList.size())  
		return NULL;

	reserve.wait();  //还有剩余才能成功返回
	
	lock.lock();

	con = connList.front();
	connList.pop_front();

	--FreeConn;
	++CurConn;

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
	++FreeConn;
	--CurConn;

	lock.unlock();

	reserve.post();
	return true;
}

//销毁数据库连接池
void connection_pool::DestroyPool()
{

	lock.lock();
	if (connList.size() > 0)
	{
		list<MYSQL *>::iterator it;
		for (it = connList.begin(); it != connList.end(); ++it)
		{
			MYSQL *con = *it;
			mysql_close(con);
		}
		CurConn = 0;
		FreeConn = 0;
		connList.clear();

		lock.unlock();
	}

	lock.unlock();
}

//当前空闲的连接数
int connection_pool::GetFreeConn()
{
	return this->FreeConn;
}

connection_pool::~connection_pool()  //通过RAII机制来完成自动释放
{
	DestroyPool();
}

connectionRAII::connectionRAII(MYSQL **SQL, connection_pool *connPool){  //connectionRAII构造函数
	*SQL = connPool->GetConnection();  //获得连接
	
	conRAII = *SQL;
	poolRAII = connPool;
}

connectionRAII::~connectionRAII(){  //connectionRAII析构函数
	poolRAII->ReleaseConnection(conRAII);
}