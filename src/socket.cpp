#include <sys/socket.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include "url.h"
#include "socket.h"
#include "threads.h"
#include "qstring.h"
#include "dso.h"
 
/* regex pattern for parsing href */
static const char * HREF_PATTERN = "href=\"\\s*\\([^ >\"]*\\)\\s*\"";

/* convert header string to Header object */
static Header * parse_header(char *header);

/* call modules to check header */
static int header_postcheck(Header *header);

int build_connect(int *fd, char *ip, int port)
{
    struct sockaddr_in server_addr;
    bzero(&server_addr, sizeof(struct sockaddr_in));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    if (!inet_aton(ip, &(server_addr.sin_addr))) {
        return -1;
    }

    if ((*fd = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
        return -1;
    }

    if (connect(*fd, (struct sockaddr *)&server_addr, sizeof(struct sockaddr_in)) < 0) {
        close(*fd);
        return -1;
    }

    return 0;
}
//发送http请求
int send_request(int fd, void *arg)
{
    int need, begin, n;
    char request[1024] = {0};
    Url *url = (Url *)arg;
//组成http头
    sprintf(request, "GET /%s HTTP/1.0\r\n"
            "Host: %s\r\n"
            "Accept: */*\r\n"
            "Connection: Keep-Alive\r\n"
            "User-Agent: Mozilla/5.0 (compatible; Qteqpidspider/1.0;)\r\n"
            "Referer: %s\r\n\r\n", url->path, url->domain, url->domain);

    need = strlen(request);
    begin = 0;
    //向远端服务器发送请求头
    while(need) {
        n = write(fd, request+begin, need);
        if (n <= 0) {
            if (errno == EAGAIN) { //write buffer full, delay retry
                usleep(1000);
                continue;
            }
            SPIDER_LOG(SPIDER_LEVEL_WARN, "Thread %lu send ERROR: %d", pthread_self(), n);
            free_url(url);
            close(fd);
            return -1;
        }
        begin += n;
        need -= n;
    }
    return 0;
}

void set_nonblocking(int fd)
{
    int flag;
    if ((flag = fcntl(fd, F_GETFL)) < 0) {
        SPIDER_LOG(SPIDER_LEVEL_ERROR, "fcntl getfl fail");
    }
    flag |= O_NONBLOCK;
    if ((flag = fcntl(fd, F_SETFL, flag)) < 0) {
        SPIDER_LOG(SPIDER_LEVEL_ERROR, "fcntl setfl fail");
    }
}
//一个HTML分配1M大小的缓冲空间
#define HTML_MAXLEN   1024*1024

void * recv_response(void * arg)
{// epollin 事件到来就调用该函数解析
    begin_thread();//这个函数只是打印线程自身的ID

    int i, n, trunc_head = 0, len = 0;
    char * body_ptr = NULL;
    evso_arg * narg = (evso_arg *)arg;
    Response *resp = (Response *)malloc(sizeof(Response));
    resp->header = NULL;
    resp->body = (char *)malloc(HTML_MAXLEN);
    resp->body_len = 0;
    resp->url = narg->url;

    regex_t re;//regex_t是一个结构体数据类型，用来存放编译后的正则表达式。它的成员re_nsub 用来存储正则表达式中的子正则表达式的个数   子正则表达式就是用圆括号抱起来的部分表达式
    // regcomp(regex_t *compiled, const char *pattern, int cflags)
    //pattern 是指向我们写好的正则表达式的指针
    if (regcomp(&re, HREF_PATTERN, 0) != 0) {/* compile error 匹配错误*/
        SPIDER_LOG(SPIDER_LEVEL_ERROR, "compile regex error");
    }

    SPIDER_LOG(SPIDER_LEVEL_INFO, "Crawling url: %s/%s", narg->url->domain, narg->url->path);

    while(1) {
        /* what if content-length exceeds HTML_MAXLEN?  超过则会一直读，读到没有数据为止*/
    	//              读取后放到 resp->body+len  得到返回数据
        n = read(narg->fd, resp->body + len, 1024);//核心接收函数 n表示读取到的长度，n 小于0表示错误，等于0 表示接收数据完毕，读取完毕后采用正则表达式解析页面
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) { 
                /**
                 * TODO: Why always recv EAGAIN?
                 * should we deal EINTR
                 */
                //SPIDER_LOG(SPIDER_LEVEL_WARN, "thread %lu meet EAGAIN or EWOULDBLOCK, sleep", pthread_self());
                usleep(100000);
                continue;
            } //容错      strerror 返回：指向错误信息的指针即错误的描述字符串
            SPIDER_LOG(SPIDER_LEVEL_WARN, "Read socket fail: %s", strerror(errno));
            break;

        } else if (n == 0) {
            /* finish reading 数据读完*/
            resp->body_len = len;
            if (resp->body_len > 0)
            {
            	//匹配正则表达式，如果是新的会加入原始的队列
            	//编译好的正则表达式，反馈体，原来的url
                extract_url(&re, resp->body, narg->url);//该函数在url.cpp中
            }
            /* deal resp->body 处理响应体*/
            for (i = 0; i < (int)modules_post_html.size(); i++) {
            	SPIDER_LOG(SPIDER_LEVEL_WARN,"保存文件");
                modules_post_html[i]->handle(resp);//此模块就是保存html文件的
            }
            break;
        } else {
            //SPIDER_LOG(SPIDER_LEVEL_WARN, "read socket ok! len=%d", n);
            len += n;//更新已经读取的长度
            resp->body[len] = '\0';

            /*
             //strstr() 函数搜索一个字符串在另一个字符串中的第一次出现。
             //找到所搜索的字符串，则该函数返回第一次匹配的字符串的地址；
             //如果未找到所搜索的字符串，则返回NULL。
             */
            if (!trunc_head) {//还没有截去头部
                if ((body_ptr = strstr(resp->body, "\r\n\r\n")) != NULL) {   //头部与体相差两个\r\n
                    *(body_ptr+2) = '\0';//响应体
                    resp->header = parse_header(resp->body);//解析一下响应头，得到状态码还有类型
                    if (!header_postcheck(resp->header)) {//用模块再次检测
                        goto leave; /* modulues filter fail */
                    }
                    trunc_head = 1;

                    /* cover header */
                    body_ptr += 4;//这部分对比网页的源码去看
                    for (i = 0; *body_ptr; i++) {//保存内容
                        resp->body[i] = *body_ptr;
                        body_ptr++;
                    }
                    resp->body[i] = '\0';
                    len = i;//去除头部的操作应该是发生在第一次的
                } 
                continue;
            }
        }
    }

leave:
    close(narg->fd); /* close socket */
    free_url(narg->url); /* free Url object */
    regfree(&re); /* free regex object */
    /* free resp */
    free(resp->header->content_type);
    free(resp->header);
    free(resp->body);
    free(resp);

    end_thread();//结束任务
    return NULL;
}


static int header_postcheck(Header *header)
{
    unsigned int i;
    for (i = 0; i < modules_post_header.size(); i++) {
        if (modules_post_header[i]->handle(header) != MODULE_OK)
            return 0;
    }
    return 1;
}

static Header * parse_header(char *header)
{
    int c = 0;
    char *p = NULL;
    char **sps = NULL;
    char *start = header;
    Header *h = (Header *)calloc(1, sizeof(Header));

    if ((p = strstr(start, "\r\n")) != NULL) {
        *p = '\0';
        sps = strsplit(start, ' ', &c, 2);
        if (c == 3) {
            h->status_code = atoi(sps[1]);
        } else {
            h->status_code = 600; 
        }
        start = p + 2;
    }

    while ((p = strstr(start, "\r\n")) != NULL) {
        *p = '\0';
        sps = strsplit(start, ':', &c, 1);
        if (c == 2) {
            if (strcasecmp(sps[0], "content-type") == 0) {
                h->content_type = strdup(strim(sps[1]));
            }
        }
        start = p + 2;
    }
    return h;
}
