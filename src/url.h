#ifndef QURL_H
#define QURL_H
 
#include <event.h>
#include <evdns.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <regex.h>
#include <queue>
#include <map>
#include <string>
#include "spider.h"
#include "bloomfilter.h"

using namespace std;

#define MAX_LINK_LEN 128

#define TYPE_HTML  0
#define TYPE_IMAGE 1
//维护url原始字符串
typedef struct Surl {
    char  *url;//指向字符串的指针
    int    level;//url深度
    int    type;//抓取类型
} Surl;
//解析后的Url  用来维护解析后的Url  从域名中解析出IP地址
typedef struct Url {
    char *domain;//域名
    char *path;//路径
    int  port;//端口
    char *ip;//指向ip地址字符串的指针
    int  level;//url种子爬取深度
} Url;
//
typedef struct evso_arg {
    int     fd;
    Url     *url;
} evso_arg;

extern void push_surlqueue(Surl * url);
extern Url * pop_ourlqueue();
extern void * urlparser(void * arg);
extern char * url2fn(const Url * url);
extern char * url_normalized(char *url);
extern void free_url(Url * ourl);
extern int is_ourlqueue_empty();
extern int is_surlqueue_empty();
extern int get_surl_queue_size();
extern int get_ourl_queue_size();
extern int extract_url(regex_t *re, char *str, Url *domain);
extern int iscrawled(char * url);
extern char * attach_domain(char *url, const char *domain);

#endif
