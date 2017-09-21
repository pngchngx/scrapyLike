#include "url.h"
#include "dso.h"

static queue <Surl *> surl_queue;//解析之前的URL
 
static queue<Url *> ourl_queue;//解析后的URL

static map<string, string> host_ip_map;//记录解析后的{主机名：IP地址}达到提高效率的效果

static Url * surl2ourl(Surl *url);
static void dns_callback(int result, char type, int count, int ttl, void *addresses, void *arg);
static int is_bin_url(char *url);
static int surl_precheck(Surl *surl);
static void get_timespec(timespec * ts, int millisecond);

pthread_mutex_t oq_lock = PTHREAD_MUTEX_INITIALIZER;//对已经被解析的url队列surl_queue的锁，避免多个线程取出同一个URL
pthread_mutex_t sq_lock = PTHREAD_MUTEX_INITIALIZER;//对原始未被解析的url队列surl_queue的锁，避免多个线程取出同一个URL
pthread_cond_t  oq_cond = PTHREAD_COND_INITIALIZER;//
pthread_cond_t  sq_cond = PTHREAD_COND_INITIALIZER;//

void push_surlqueue(Surl *url)//原始url出队。  注意要加锁
{
    if (url != NULL && surl_precheck(url)) {
        SPIDER_LOG(SPIDER_LEVEL_DEBUG, "I want this url: %s", url->url);
        pthread_mutex_lock(&sq_lock);
        surl_queue.push(url);
        if (surl_queue.size() == 1)
            pthread_cond_signal(&sq_cond);
        pthread_mutex_unlock(&sq_lock);
    }
}

Url * pop_ourlqueue()//已被解析的URL队列出队   删除已经下载完的url
{
    Url *url = NULL;
    pthread_mutex_lock(&oq_lock);
    if (!ourl_queue.empty()) {
        url = ourl_queue.front();
        ourl_queue.pop();
        pthread_mutex_unlock(&oq_lock);
        return url;
    } else {
        int trynum = 3;
        struct timespec timeout;
        while (trynum-- && ourl_queue.empty()) {
            get_timespec(&timeout, 500); /* 0.5s timeout*/
            pthread_cond_timedwait(&oq_cond, &oq_lock, &timeout);
        }

        if (!ourl_queue.empty()) {
            url = ourl_queue.front();
            ourl_queue.pop();
        }
        pthread_mutex_unlock(&oq_lock);
        return url;
    }
}

static void get_timespec(timespec * ts, int millisecond)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    ts->tv_sec = now.tv_sec;
    ts->tv_nsec = now.tv_usec * 1000;
    ts->tv_nsec += millisecond * 1000;
}

static int surl_precheck(Surl *surl)
{
    unsigned int i;
    for (i = 0; i < modules_pre_surl.size(); i++) {
        if (modules_pre_surl[i]->handle(surl) != MODULE_OK)
            return 0;
    }
    return 1;
}

static void push_ourlqueue(Url * ourl)
{
    pthread_mutex_lock(&oq_lock);
    ourl_queue.push(ourl);
    if (ourl_queue.size() == 1)
        pthread_cond_broadcast(&oq_cond);
    pthread_mutex_unlock(&oq_lock);
}

int is_ourlqueue_empty() 
{
    pthread_mutex_lock(&oq_lock);
    int val = ourl_queue.empty();
    pthread_mutex_unlock(&oq_lock);
    return val;
}

int is_surlqueue_empty() 
{
    pthread_mutex_lock(&sq_lock);
    int val = surl_queue.empty();
    pthread_mutex_unlock(&sq_lock);
    return val;
}

void * urlparser(void *none)//分割切割URL 提取每一部分 http://  文件路径等等
{
    Surl *url = NULL;
    Url  *ourl = NULL;
    map<string, string>::const_iterator itr;
    //event_base * base = event_base_new();
    //evdns_base * dnsbase = evdns_base_new(base, 1);
    //event_base_loop(base,EVLOOP_NONBLOCK);

    while(1) {
        pthread_mutex_lock(&sq_lock);
        while (surl_queue.empty()) {
            pthread_cond_wait(&sq_cond, &sq_lock);
        }
        url = surl_queue.front();
        surl_queue.pop();
        pthread_mutex_unlock(&sq_lock);

        ourl = surl2ourl(url);

        itr = host_ip_map.find(ourl->domain);
        if (itr == host_ip_map.end()) { /* not found */
            /* dns resolve   libevent开源库*/
            event_base * base = event_init();
            evdns_init();
            evdns_resolve_ipv4(ourl->domain, 0, dns_callback, ourl);
            event_dispatch();
            event_base_free(base);

            //evdns_base_resolve_ipv4(dnsbase, ourl->domain, 0, dns_callback, ourl);
            //event_base_loop(base, EVLOOP_ONCE | EVLOOP_NONBLOCK);
        } else {
            ourl->ip = strdup(itr->second.c_str());
            push_ourlqueue(ourl);
        }
    }

    //evdns_base_free(dnsbase, 0);
    //event_base_free(base);
    return NULL;
}

/*
 * 返回最后找到的链接的下一个下标,如果没找到返回 0;
 */
int extract_url(regex_t *re, char *str, Url *ourl)
{
    const size_t nmatch = 2;
    regmatch_t matchptr[nmatch];
    int len;

    char *p = str;
    while (regexec(re, p, nmatch, matchptr, 0) != REG_NOMATCH) {
        len = (matchptr[1].rm_eo - matchptr[1].rm_so);
        p = p + matchptr[1].rm_so;
        char *tmp = (char *)calloc(len+1, 1);
        strncpy(tmp, p, len);
        tmp[len] = '\0';
        p = p + len + (matchptr[0].rm_eo - matchptr[1].rm_eo);

        /* exclude binary file */
        if (is_bin_url(tmp)) {
            free(tmp);
            continue;
        }

        char *url = attach_domain(tmp, ourl->domain);
        if (url != NULL) {
            SPIDER_LOG(SPIDER_LEVEL_DEBUG, "I find a url: %s", url);
            Surl * surl = (Surl *)malloc(sizeof(Surl));
            surl->level = ourl->level + 1;
            surl->type = TYPE_HTML;

            /* normalize url */
            if ((surl->url = url_normalized(url)) == NULL) {
                SPIDER_LOG(SPIDER_LEVEL_WARN, "Normalize url fail");
                free(surl);
                continue;
            }

            if (iscrawled(surl->url)) { /* if is crawled */
                SPIDER_LOG(SPIDER_LEVEL_DEBUG, "I seen this url: %s", surl->url);
                free(surl->url);
                free(surl);
                continue;
            } else {
                push_surlqueue(surl);
            }

        }
    }

    return (p-str);
}

/* if url refer to binary file
 * image: jpg|jpeg|gif|png|ico|bmp
 * flash: swf
 */
static char * BIN_SUFFIXES = ".jpg.jpeg.gif.png.ico.bmp.swf";//二进制文件
static int is_bin_url(char *url)
{
    char *p = NULL;
    if ((p = strrchr(url, '.')) != NULL) {
        if (strstr(BIN_SUFFIXES, p) == NULL)
            return 0;
        else
            return 1;
    }
    return 0;
}

char * attach_domain(char *url, const char *domain)
{
    if (url == NULL)
        return NULL;

    if (strncmp(url, "http", 4) == 0) {
        return url;

    } else if (*url == '/') {
        int i;
        int ulen = strlen(url);
        int dlen = strlen(domain);
        char *tmp = (char *)malloc(ulen+dlen+1);
        for (i = 0; i < dlen; i++)
            tmp[i] = domain[i];
        for (i = 0; i < ulen; i++)
            tmp[i+dlen] = url[i];
        tmp[ulen+dlen] = '\0';
        free(url);
        return tmp;

    } else {
        //do nothing
        free(url);
        return NULL;
    }
}

char * url2fn(const Url * url)
{
    int i = 0;
    int l1 = strlen(url->domain);
    int l2 = strlen(url->path);
    char *fn = (char *)malloc(l1+l2+2);

    for (i = 0; i < l1; i++)
        fn[i] = url->domain[i];

    fn[l1++] = '_';

    for (i = 0; i < l2; i++)
        fn[l1+i] = (url->path[i] == '/' ? '_' : url->path[i]);

    fn[l1+l2] = '\0';

    return fn;
}

int iscrawled(char * url) {
    return search(url); /* use bloom filter algorithm */
}
//DNS解析回调函数
static void dns_callback(int result, char type, int count, int ttl, void *addresses, void *arg) 
{
    Url * ourl = (Url *)arg;
    struct in_addr *addrs = (in_addr *)addresses;

    if (result != DNS_ERR_NONE || count == 0) {
        SPIDER_LOG(SPIDER_LEVEL_WARN, "Dns resolve fail: %s", ourl->domain);
    } else {
        char * ip = inet_ntoa(addrs[0]);//网络字节序转化为主机字节序
        SPIDER_LOG(SPIDER_LEVEL_DEBUG, "Dns resolve OK: %s -> %s", ourl->domain, ip);
        host_ip_map[ourl->domain] = strdup(ip);//ip填入domain对应的IP
        ourl->ip = strdup(ip);//ip填入ourl
        push_ourlqueue(ourl);//加入队列
    }
    event_loopexit(NULL); // not safe for multithreads 
}

static Url * surl2ourl(Surl * surl)
{
    Url *ourl = (Url *)calloc(1, sizeof(Url));
    char *p = strchr(surl->url, '/');
    if (p == NULL) {
        ourl->domain = surl->url;
        ourl->path = surl->url + strlen(surl->url); 
    } else {
        *p = '\0';
        ourl->domain = surl->url;
        ourl->path = p+1;
    }
    // port
/*
 原型：char *strrchr(const char *str, char c);
#include<string.h>
找一个字符c在另一个字符串str中末次出现的位置（也就是从str的右侧开始查找字符c首次出现的位置），并返回从字符串中的这个位置起，一直到字符串结束的所有字符。如果未能找到指定字符，那么函数将返回NULL。
*/
    p = strrchr(ourl->domain, ':');//找到最后一个出现的冒号
    if (p != NULL) {
        *p = '\0';
        ourl->port = atoi(p+1);
        if (ourl->port == 0)
            ourl->port = 80;

    } else {//Url中若没有端口号，则默认的80端口
        ourl->port = 80;
    }
    // level
    ourl->level = surl->level;
    return ourl;
}

char * url_normalized(char *url) 
{
    if (url == NULL) return NULL;

    /* rtrim url */
    int len = strlen(url);
    while (len && isspace(url[len-1]))
        len--;
    url[len] = '\0';

    if (len == 0) {
        free(url);
        return NULL;
    }

    /* remove http(s):// */
    if (len > 7 && strncmp(url, "http", 4) == 0) {
        int vlen = 7;
        if (url[4] == 's') /* https */
            vlen++;

        len -= vlen;
        char *tmp = (char *)malloc(len+1);
        strncpy(tmp, url+vlen, len);
        tmp[len] = '\0';
        free(url);
        url = tmp;
    }

    /* remove '/' at end of url if have */
    if (url[len-1] == '/') {
        url[--len] = '\0';
    }

    if (len > MAX_LINK_LEN) {
        free(url);
        return NULL;
    }

    return url;
}


void free_url(Url * ourl)
{
    free(ourl->domain);
    //free(ourl->path);
    free(ourl->ip);
    free(ourl);
}

int get_surl_queue_size()
{
    return surl_queue.size();
}

int get_ourl_queue_size()
{
    return ourl_queue.size();
}
