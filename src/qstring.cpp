#include <strings.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include "qstring.h"

 //增强版本的字符串连接函数                                                                                                                 //n个字符串连接
char * strcat2(int argc, const char *str1, const char * str2, ...) 
{
    int tmp = 0;
    char *dest = NULL;
    char *cur = NULL;
    va_list va_ptr;
    size_t len = strlen(str1) + strlen(str2);

    argc -= 2;
    tmp = argc;

    va_start(va_ptr, str2);
    while(argc-- && (cur = va_arg(va_ptr,char *))) {
        len += strlen(cur);
    }
    va_end(va_ptr);

    dest = (char *)malloc(len+1);
    dest[0] = '\0';
    strcat(dest, str1);
    strcat(dest, str2);

    argc = tmp;
    va_start(va_ptr, str2);
    while(argc-- && (cur = va_arg(va_ptr,char *))) {
        strcat(dest, cur);
    }
    va_end(va_ptr);

    return dest;
}
//去除空格（算法仔细看一下）
char * strim(char *str)
{
    char *end, *sp, *ep;
    size_t len;

    sp = str;
    end = ep = str+strlen(str)-1;
    while(sp <= end && isspace(*sp)) sp++;
    while(ep >= sp && isspace(*ep)) ep--;
    len = (ep < sp) ? 0 : (ep-sp)+1;
    sp[len] = '\0';
    return sp;
}
// 切割字符串的  传入一个字符串在传入一个切割字符串的依据   切割后的字符串返回
//函数内申请内存，函数外释放
char ** strsplit(char *line, char delimeter, int *count, int limit)
{
    char *ptr = NULL, *str = line;
    char **vector = NULL;

    *count = 0;
    while((ptr = strchr(str, delimeter))) {
        *ptr = '\0';
        vector = (char **)realloc(vector,((*count)+1)*sizeof(char *));
        vector[*count] = strim(str);
        str = ptr+1;
        (*count)++;	
        if (--limit == 0) break;
    }
    if (*str != '\0') {
        vector = (char **)realloc(vector,((*count)+1)*sizeof(char *));
        vector[*count] = strim(str);
        (*count)++;
    }
    return vector;
}
// 判断 strcasecmp 传进去的字符是yes还是no 我们检查传进去的str 把他转化成bool类型，用在配置文件里当做开关。当做配置选项的开关
// 字符串yes或no转换为bool逻辑值1或0并返回
int yesnotoi(char *str)
{
    if (strcasecmp(str,"yes") == 0) return 1;
    if (strcasecmp(str,"no") == 0) return 0;
    return -1;
}
