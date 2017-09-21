#include <stdio.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <getopt.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include "spider.h"
#include "threads.h"
#include "qstring.h"
 
int g_epfd;
Config *g_conf;
extern int g_cur_thread_num;

static int set_nofile(rlim_t limit);
static void daemonize();
static void stat(int sig);
static int set_ticker(int second);

static void version()
{
    printf("Version: spider 1.0\n");
    exit(1);
}

static void usage()
{
    printf("Usage: ./spider [Options]\n"
            "\nOptions:\n"
            "  -h\t: this help\n"
            "  -v\t: print spiderq's version\n"
            "  -d\t: run program as a daemon process\n\n");
    exit(1);
}

int main(int argc, void *argv[]) 
{
    struct epoll_event events[10];
    int daemonized = 0;
    char ch;

    /* 解析命令行参数 */
    while ((ch = getopt(argc, (char* const*)argv, "vhd")) != -1) {
        switch(ch) {
            case 'v':
                version();
                break;
            case 'd':
                daemonized = 1;
                break;
            case 'h':
            case '?':
            default:
                usage();
        }
    }

    /* 解析日志 */
    g_conf = initconfig();
    loadconfig(g_conf);

    /* s设置 fd num to 1024 */
    set_nofile(1024); 

    /* 载入处理模块 */
    vector<char *>::iterator it = g_conf->modules.begin();//保存模块名称，从配置文件中得来的
    for(; it != g_conf->modules.end(); it++) {
        dso_load(g_conf->module_path, *it); 
    } 

    /* 添加爬虫种子 */
    if (g_conf->seeds == NULL) {
        SPIDER_LOG(SPIDER_LEVEL_ERROR, "We have no seeds!");
    } else {
        int c = 0;
        char ** splits = strsplit(g_conf->seeds, ',', &c, 0);//种子解析 strsplit 在工具函数 qstring文件里
        while (c--) {
            Surl * surl = (Surl *)malloc(sizeof(Surl));
            surl->url = url_normalized(strdup(splits[c]));
            surl->level = 0;
            surl->type = TYPE_HTML;
            if (surl->url != NULL)
                push_surlqueue(surl);
        }
    }	

    /* 守护进程模式 */
    if (daemonized)
        daemonize();//也是一个函数

    /* 设定下载路径 */
    chdir("download"); 

    /* 启动用于解析DNS的线程 */
    int err = -1;        //urlparser  存在于 url.cpp文件里  做分割切割URL
    if ((err = create_thread(urlparser, NULL, NULL, NULL)) < 0) {
        SPIDER_LOG(SPIDER_LEVEL_ERROR, "Create urlparser thread fail: %s", strerror(err));
    }

    /* waiting seed ourl ready 等待种子解析成功*/
    int try_num = 1;
    //
    while(try_num < 8 && is_ourlqueue_empty())
        usleep((10000 << try_num++));

    if (try_num >= 8) {
        SPIDER_LOG(SPIDER_LEVEL_ERROR, "NO ourl! DNS parse error?");//输出这一行程序就终止了
    }

    /* set ticker  设置处理超时的一个设定*/
    if (g_conf->stat_interval > 0) {
        signal(SIGALRM, stat);
        set_ticker(g_conf->stat_interval);
    }

    /* begin create epoll to run */
    int ourl_num = 0;
    g_epfd = epoll_create(g_conf->max_job_num);

    while(ourl_num++ < g_conf->max_job_num) {
        if (attach_epoll_task() < 0)//epoll 任务管理函数
            break;
    }

    /* epoll wait */
    int n, i;
    while(1) {//一直在等待 守护进程
        n = epoll_wait(g_epfd, events, 10, 2000);//等待有抓取过程过来
        printf("epoll:%d\n",n);
        if (n == -1)
            printf("epoll errno:%s\n",strerror(errno));
        fflush(stdout);
        //容错的一些东西
//取出一个URL 之后给到socket   socket.cpp
        if (n <= 0) {
            if (g_cur_thread_num <= 0 && is_ourlqueue_empty() && is_surlqueue_empty()) {
                sleep(1);
                if (g_cur_thread_num <= 0 && is_ourlqueue_empty() && is_surlqueue_empty())
                    break;
            }
        }
//event 可以和epoll完美的结合
        for (i = 0; i < n; i++) {
            evso_arg * arg = (evso_arg *)(events[i].data.ptr);
            if ((events[i].events & EPOLLERR) ||
                (events[i].events & EPOLLHUP) ||
                (!(events[i].events & EPOLLIN))) {
                SPIDER_LOG(SPIDER_LEVEL_WARN, "epoll fail, close socket %d",arg->fd);
                close(arg->fd);
                continue;
            }
//删除
            epoll_ctl(g_epfd, EPOLL_CTL_DEL, arg->fd, &events[i]); /* del event */

            printf("hello epoll:event=%d\n",events[i].events);
            fflush(stdout);
            create_thread(recv_response, arg, NULL, NULL);
        }
    }
    SPIDER_LOG(SPIDER_LEVEL_DEBUG, "Task done!");
    close(g_epfd);//关闭epoll句柄  epoll是由内核管理的必须关闭否则系统资源不释放
    return 0;
}

//epoll关联的任务处理过程
int attach_epoll_task()
{
    struct epoll_event ev;
    int sock_rv;
    int sockfd;
    Url * ourl = pop_ourlqueue();//取出一个URL来
    if (ourl == NULL) {//容错处理
        SPIDER_LOG(SPIDER_LEVEL_WARN, "Pop ourlqueue fail!");
        return -1;
    }

    /* connect socket and get sockfd  得到URL之后用SOCKET 进行连接*/
    if ((sock_rv = build_connect(&sockfd, ourl->ip, ourl->port)) < 0) {
        SPIDER_LOG(SPIDER_LEVEL_WARN, "Build socket connect fail: %s", ourl->ip);
        return -1;
    }

    set_nonblocking(sockfd);//非阻塞模式
//
    if ((sock_rv = send_request(sockfd, ourl)) < 0) {
        SPIDER_LOG(SPIDER_LEVEL_WARN, "Send socket request fail: %s", ourl->ip);
        return -1;
    } 

    evso_arg * arg = (evso_arg *)calloc(1, sizeof(evso_arg));
    arg->fd = sockfd;
    arg->url = ourl;
    ev.data.ptr = arg;
    ev.events = EPOLLIN | EPOLLET;//边沿触发模式
    if (epoll_ctl(g_epfd, EPOLL_CTL_ADD, sockfd, &ev) == 0) {/* add event socket句柄注册给epoll */
        SPIDER_LOG(SPIDER_LEVEL_DEBUG, "Attach an epoll event success!");
    } else {
        SPIDER_LOG(SPIDER_LEVEL_WARN, "Attach an epoll event fail!");
        return -1;
    }

    g_cur_thread_num++; //当前正在执行抓取的任务数量，一旦超过事先设定的值就会停止
    return 0;
}

static int set_nofile(rlim_t limit)//维护抓取数量
{
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) < 0) {
        SPIDER_LOG(SPIDER_LEVEL_WARN, "getrlimit fail");
        return -1;
    }
    if (limit > rl.rlim_max) {
        SPIDER_LOG(SPIDER_LEVEL_WARN, "limit should NOT be greater than %lu", rl.rlim_max);
        return -1;
    }
    rl.rlim_cur = limit;
    if (setrlimit(RLIMIT_NOFILE, &rl) < 0) {
        SPIDER_LOG(SPIDER_LEVEL_WARN, "setrlimit fail");
        return -1;
    }
    return 0;
}

/**/
static void daemonize()
{
    int fd;
    if (fork() != 0) exit(0);
    setsid();
    SPIDER_LOG(SPIDER_LEVEL_INFO, "Daemonized...pid=%d", (int)getpid());	

    /* redirect stdin|stdout|stderr to /dev/null */
    if ((fd = open("/dev/null", O_RDWR, 0)) != -1) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO)
            close(fd);
    }

    /* redirect stdout to logfile */
    if (g_conf->logfile != NULL && (fd = open(g_conf->logfile, O_RDWR | O_APPEND | O_CREAT, 0)) != -1) {
        dup2(fd, STDOUT_FILENO);
        if (fd > STDERR_FILENO)
            close(fd);
    }

}

static int set_ticker(int second)//设置抓取超时
{
    struct itimerval itimer;
    itimer.it_interval.tv_sec = (long)second;
    itimer.it_interval.tv_usec = 0;
    itimer.it_value.tv_sec = (long)second;
    itimer.it_value.tv_usec = 0;

    return setitimer(ITIMER_REAL, &itimer, NULL);
}

static void stat(int sig)//得到当前进行的任务的一个信息
{
    SPIDER_LOG(SPIDER_LEVEL_DEBUG, 
    		//打印出信息    SPIDER_LOG宏就像一个加强版的printf
    		//你可以控制它输出的方向，是输出到日志文件里还是控制终端上，
    		//同时还有一个级别
            "cur_thread_num=%d\tsurl_num=%d\tourl_num=%d",
            g_cur_thread_num,//线程数
            get_surl_queue_size(),//抓取队列数
            get_ourl_queue_size());//已经抓取的队列
}
