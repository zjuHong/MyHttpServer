#include "http_conn.h"
#include "../log/log.h"
#include <map>
#include <mysql/mysql.h>
//参考链接 https://mp.weixin.qq.com/s/MNuPqAh7ubbSKz9pqU-chQ
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

//当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
const char *doc_root = "/home/hyd/code/TinyWebServer-master/root";

//创建数据库连接池
connection_pool *connPool = connection_pool::GetInstance("localhost", "root", "000000", "mydb", 3306, 5);

//将表中的用户名和密码放入map
map<string, string> users;
//将数据库中的用户名和密码存入map
void http_conn::initmysql_result()
{
    //先从连接池中取一个连接
    MYSQL *mysql = connPool->GetConnection();

    //在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM users"))
    {
        //printf("INSERT error:%s\n",mysql_error(mysql));
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
        //return BAD_REQUEST;
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
    //将连接归还连接池
    connPool->ReleaseConnection(mysql);
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
void addfd(int epollfd, int fd, bool one_shot)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    if (one_shot)
        event.events |= EPOLLONESHOT;//ET
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//从内核事件表删除描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}
//https://blog.csdn.net/liuhengxiao/article/details/46911129
//将事件重置为EPOLLONESHOT 如果在处理当前的SOCKET则不再重新注册相关事件
void modfd(int epollfd, int fd, int ev)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;//连接用户数
int http_conn::m_epollfd = -1;//efd

//关闭连接，从epoll表删除描述符，客户总量减一，真正的close在定时器的回调函数里调用
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

//初始化连接，私有成员，只能自己调用
void http_conn::init(int sockfd, const sockaddr_in &addr)
{
    m_sockfd = sockfd;
    m_address = addr;
    //int reuse=1;
    //setsockopt(m_sockfd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
    addfd(m_epollfd, sockfd, true);
    m_user_count++;
    init();
}

//初始化新接受的连接 读写buf清零
void http_conn::init()
{
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
    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

//从状态机，用于分析出一行内容
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r')
        {
            if ((m_checked_idx + 1) == m_read_idx)//下一个字符达到了buffer结尾，则接收不完整，需要继续接收
                return LINE_OPEN;
            else if (m_read_buf[m_checked_idx + 1] == '\n')//下一个字符是\n，将\r\n改为\0\0
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;//如果都不符合，则返回语法错误
        }
        else if (temp == '\n')
        {
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;//并没有找到\r\n，需要继续接收
}

//循环读取客户数据，直到无数据可读或对方关闭连接，在非阻塞ET工作模式下，需要一次性将数据读完
bool http_conn::read_once()
{
    if (m_read_idx >= READ_BUFFER_SIZE)//超过缓存了就先不读，下次再读
    {
        return false;
    }
    int bytes_read = 0;
    while (true)//循环recv
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        if (bytes_read == -1)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)//EAGAIN:系统资源不可用，EWOULDBLOCK为win下的宏
                break;//表示已经读完，内核中的缓冲区已经没有内容了
            return false;
        }
        else if (bytes_read == 0)//另一端已关闭
        {
            return false;
        }
        m_read_idx += bytes_read;
    }
    return true;
}

//解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    m_url = strpbrk(text, " \t"); //请求行中最先含有空格和\t任一字符的位置并返回
    if (!m_url)
    {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';
    char *method = text;
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else
        return BAD_REQUEST;
    m_url += strspn(m_url, " \t");
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");//使用与判断请求方式的相同逻辑，判断HTTP版本号
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;
    if (strncasecmp(m_url, "http://", 7) == 0)
    {//这里主要是有些报文的请求资源中会带有http://，这里需要对这种情况进行单独处理
        m_url += 7;
        m_url = strchr(m_url, '/');
    }
    if(strncasecmp(m_url,"https://",8)==0)
    {//同样增加https情况
        m_url+=8;
        m_url=strchr(m_url,'/');
    }
    if (!m_url || m_url[0] != '/')//一般的不会带有上述两种符号，直接是单独的/或/后面带访问资源
        return BAD_REQUEST;
    if (strlen(m_url) == 1)//当url为/时，显示判断界面
        strcat(m_url, "judge.html");
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

//解析http请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{//在报文中，请求头和空行的处理使用的同一个函数，这里通过判断当前的text首位是不是\0字符，
//若是，则表示当前处理的是空行，若不是，则表示当前处理的是请求头。
    if (text[0] == '\0')
    {
        if (m_content_length != 0)
        {//判断是GET还是POST请求 GET和POST请求报文的区别之一是有无消息体部分 POST需要跳转到消息体处理状态 
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;
        }
    }
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else//别的头部暂时不处理，写入日志
    {
        //printf("oop!unknow header: %s\n",text);
        LOG_INFO("oop!unknow header: %s", text);
        Log::get_instance()->flush();
    }
    return NO_REQUEST;
}

//判断http请求是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {//判断buffer中是否读取了消息体
        text[m_content_length] = '\0';
        //POST请求中最后为输入的用户名和密码
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

//
http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;
    //printf("=========请求行头=========\n");
    //LOG_INFO("=========请求行头=========\n");
    //Log::get_instance()->flush();
    // 在GET请求报文中，每一行都是\r\n作为结束，所以对报文进行拆解时仅用从状态机的状态line_status=parse_line())==LINE_OK
    // 语句即可。但，在POST请求报文中，消息体的末尾没有任何字符，所以不能使用从状态机的状态，
    // 这里转而使用主状态机的状态作为循环入口条件。并在完成消息体解析后，将line_status变量更改为LINE_OPEN，
    // 此时可以跳出循环，完成报文解析任务
    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        text = get_line();
        m_start_line = m_checked_idx;
        //printf("======:%s\n",text);
        LOG_INFO("%s", text);
        Log::get_instance()->flush();
        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE:
        {
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;
        }
        case CHECK_STATE_HEADER:
        {
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            else if (ret == GET_REQUEST)
            {
                return do_request();
            }
            break;
        }
        case CHECK_STATE_CONTENT:
        {
            ret = parse_content(text);
            if (ret == GET_REQUEST)
                return do_request();
            line_status = LINE_OPEN;
            break;
        }
        default:
            return INTERNAL_ERROR;
        }
    }
    //printf("=========请求行end=========\n");
    return NO_REQUEST;
}
// m_url为请求报文中解析出的请求资源，以/开头，也就是/xxx，项目中解析后的m_url有5种情况。
// /
// GET请求，跳转到judge.html，即欢迎访问界面
// action属性设置为0和1
// /0
// GET请求，跳转到register.html，即注册界面
// action属性设置为3check.cgi
// /1
// GET请求，跳转到log.html，即登录界面
// action属性设置为2check.cgi
// /2check.cgi
// POST请求，进行登录校验
// 验证成功跳转到welcome.html，即资源请求成功界面
// 验证失败跳转到logError.html，即登录失败界面
// /3check.cgi
// POST请求，进行注册校验
// 注册成功跳转到log.html，即登录界面
// 注册失败跳转到registerError.html，即注册失败界面
// /test.jpg
// GET请求，请求服务器上的图片资源

//对于post命令中body的解析
http_conn::HTTP_CODE http_conn::do_request()
{
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    //printf("m_url:%s\n", m_url);
    const char *p = strrchr(m_url, '/');

    //#if 0
    //处理cgi
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {

        //根据标志判断是登录检测还是注册检测
        char flag = m_url[1];
        //printf("====+++====+++%c\n", flag);

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);

        //将用户名和密码提取出来
        //user=123&passwd=123
        char name[100], password[100];
        int i;
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';
        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

//同步线程登录校验
//#if 0
        pthread_mutex_t lock;
        pthread_mutex_init(&lock, NULL);

        //从连接池中取一个连接
        MYSQL *mysql = connPool->GetConnection();

        //如果是注册，先检测数据库中是否有重名的
        //没有重名的，进行增加数据
        char *sql_insert = (char *)malloc(sizeof(char) * 200);
        strcpy(sql_insert, "INSERT INTO users(username, passwd) VALUES(");
        strcat(sql_insert, "'");
        strcat(sql_insert, name);
        strcat(sql_insert, "', '");
        strcat(sql_insert, password);
        strcat(sql_insert, "')");

        if (*(p + 1) == '3')
        {
            if (users.find(name) == users.end())
            {

                pthread_mutex_lock(&lock);
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                pthread_mutex_unlock(&lock);

                if (!res)
                    strcpy(m_url, "/log.html");
                else
                    strcpy(m_url, "/registerError.html");
            }
            else
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
        connPool->ReleaseConnection(mysql);
//#endif

//CGI多进程登录校验
#if 0

	//fd[0]:读管道，fd[1]:写管道
        pid_t pid;
        int pipefd[2];
        if(pipe(pipefd)<0)
        {
            LOG_ERROR("pipe() error:%d",4);
            return BAD_REQUEST;
        }
        if((pid=fork())<0)
        {
            LOG_ERROR("fork() error:%d",3);
            return BAD_REQUEST;
        }
	
        if(pid==0)
        {
	    //标准输出，文件描述符是1，然后将输出重定向到管道写端
            dup2(pipefd[1],1);
	    //关闭管道的读端
            close(pipefd[0]);
	    //父进程去执行cgi程序，m_real_file,name,password为输入
	    //./check.cgi name password

            execl(m_real_file,&flag,name,password, NULL);
        }
        else{
	    //printf("子进程\n");
	    //子进程关闭写端，打开读端，读取父进程的输出
            close(pipefd[1]);
            char result;
            int ret=read(pipefd[0],&result,1);

            if(ret!=1)
            {
                LOG_ERROR("管道read error:ret=%d",ret);
                return BAD_REQUEST;
            }
	    if(flag == '2'){
		    //printf("登录检测\n");
		    LOG_INFO("%s","登录检测");
    		    Log::get_instance()->flush();
		    //当用户名和密码正确，则显示welcome界面，否则显示错误界面
		    if(result=='1')
			strcpy(m_url, "/welcome.html");
		        //m_url="/welcome.html";
		    else
			strcpy(m_url, "/logError.html");
		        //m_url="/logError.html";
	    }
	    else if(flag == '3'){
		    //printf("注册检测\n");
		    LOG_INFO("%s","注册检测");
    		    Log::get_instance()->flush();
		    //当成功注册后，则显示登陆界面，否则显示错误界面
		    if(result=='1')
			strcpy(m_url, "/log.html");
			//m_url="/log.html";
		    else
			strcpy(m_url, "/registerError.html");
			//m_url="/registerError.html";
	    }
	    //printf("m_url:%s\n", m_url);
	    //回收进程资源
            waitpid(pid,NULL,0);
	    //waitpid(pid,0,NULL);
	    //printf("回收完成\n");
        }
#endif
    }

    if (*(p + 1) == '0')//新用户:action="0" method="post"
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '1')//已有账号:action="1" method="post"
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '4')//已有账号:action="1" method="post"
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/judge.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else//如果以上均不符合，即不是登录和注册，直接将url与网站目录拼接
        //这里的情况是welcome界面，请求服务器上的一个图片
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    if (stat(m_real_file, &m_file_stat) < 0)//通过stat获取请求资源文件信息，成功则将信息更新到m_file_stat结构体
        return NO_RESOURCE;
    if (!(m_file_stat.st_mode & S_IROTH))//判断文件的权限，是否可读，不可读则返回FORBIDDEN_REQUEST状态
        return FORBIDDEN_REQUEST;
    if (S_ISDIR(m_file_stat.st_mode))//判断文件类型，如果是目录，则返回BAD_REQUEST，表示请求报文有误
        return BAD_REQUEST;
    int fd = open(m_real_file, O_RDONLY);//以只读方式获取文件描述符，通过mmap将该文件映射到内存中
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);//文件映射
    close(fd);//避免文件描述符的浪费和占用
    return FILE_REQUEST;
}
void http_conn::unmap()//解除文件映射
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
    int bytes_have_send = 0;
    // int bytes_to_send = m_write_idx;
    int bytes_to_send = 0;
    for (int i = 0; i < m_iv_count; ++i) //将要发送的数据长度初始化为响应报文缓冲区长度
        bytes_to_send += int(m_iv[i].iov_len);
    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        init();
        return true;
    }
    while (1)
    {
        temp = writev(m_sockfd, m_iv, m_iv_count);
        //printf("temp:%d\n",temp);
        if (temp <= -1)
        {
            if (errno == EAGAIN)//若eagain则满了，注册写事件，等待下一次写事件触发（当写缓冲区从不可写变为可写，触发epollout），
            {// 因此在此期间无法立即接收到同一用户的下一请求，但可以保证连接的完整性。
                modfd(m_epollfd, m_sockfd, EPOLLOUT);//重新注册写事件
                return true;
            }
            unmap();
            return false;
        }
        bytes_to_send -= temp;
        bytes_have_send += temp;
        // if (bytes_to_send <= bytes_have_send)//这里有bug，小文件没有暴露出来
        if (bytes_to_send <= 0)
        {
            unmap();
            if (m_linger)//长连接重置http类实例，注册读事件，不关闭连接
            {
                //printf("========================\n");
                //printf("%s\n", "发送响应成功");
                init();
                modfd(m_epollfd, m_sockfd, EPOLLIN);
                return true;
            }
            else //短连接返回false之后就从epoll中移除fd
            {
                modfd(m_epollfd, m_sockfd, EPOLLIN);
                return false;
            }
        }
    }
}
//利用va_list把要输出的部分放入写缓冲区
bool http_conn::add_response(const char *format, ...)
{
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    va_list arg_list;
    va_start(arg_list, format);
    //将数据format从可变参数列表写入缓冲区写，返回写入数据的长度
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))//如果写入的数据长度超过缓冲区剩余空间，则报错
        return false;
    m_write_idx += len;//更新m_write_idx位置
    va_end(arg_list);
    //printf("%s\n",m_write_buf);
    LOG_INFO("request:%s", m_write_buf);
    Log::get_instance()->flush();
    return true;
}
bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
bool http_conn::add_headers(int content_len)
{
    //add_content_type();
    add_content_length(content_len);
    add_linger();
    add_blank_line();
}
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}
bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}
bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}
// 写回调，把要写的内容加入缓冲
bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
    case INTERNAL_ERROR:
    {
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form))
            return false;
        break;
    }
    case BAD_REQUEST:
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }
    case FORBIDDEN_REQUEST:
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }
    case FILE_REQUEST:
    {
        add_status_line(200, ok_200_title);
        if (m_file_stat.st_size != 0)
        {//分两部分写，第一部分是头部，第二个iovec指针指向mmap返回的文件指针，长度指向文件大小
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            return true;
        }
        else
        {
            const char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
                return false;
        }
    }
    default:
        return false;
    }
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;//除FILE_REQUEST状态外，其余状态只申请一个iovec，指向响应报文缓冲区
    return true;
}
//读回调
void http_conn::process()
{
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }
    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT);
}
