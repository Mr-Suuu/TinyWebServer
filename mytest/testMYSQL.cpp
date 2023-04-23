//
// Created by SuuuJ on 2023-04-18.
//


#include "http_conn.h"

#include <mysql/mysql.h>
#include <fstream>

void http_conn::initmysql_result(connection_pool *connPool) {
    //先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    //在user表中检索username，passwd数据，浏览器端输入
    // mysql_query 传入第一个是MYSQL对象，第二个是查询与军
    if (mysql_query(mysql, "SELECT username,passwd FROM user")) {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集
    // mysql_store_result(&mysql)告诉句柄mysql，把查询的数据从服务器端取到客户端，然后缓存起来，放在句柄mysql里面
    // 这里将结果通过result指针指出
    MYSQL_RES *result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result)) {
        // 取出查询结果的每一行，第一个作为用户名，第二个作为密码，并以键值对的形式保存
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}