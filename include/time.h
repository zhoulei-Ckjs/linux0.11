#ifndef _TIME_H
#define _TIME_H

#ifndef _TIME_T
#define _TIME_T
typedef long time_t;
#endif

#ifndef _SIZE_T
#define _SIZE_T
typedef unsigned int size_t;
#endif

#define CLOCKS_PER_SEC 100

typedef long clock_t;

/**
 * @brief 时间结构体
 */
struct tm 
{
    int tm_sec;     ///< 秒（0-59）
    int tm_min;     ///< 分钟（0-59）
    int tm_hour;    ///< 时（0-23）
    int tm_mday;    ///< 月中第几天（1-31）
    int tm_mon;     ///< 月份（0-11）
    int tm_year;    ///< 年份（自 1900 年起的偏移量，如果当前是 2024 年，这里存的是 2024 - 1900 = 124。）
    int tm_wday;    ///< 星期几（0-6）
    int tm_yday;    ///< 一年中的第几天
    int tm_isdst;   ///< 夏令时标志（当前时间是否处于夏令时。夏令时是一种为了节约能源而人为规定的时间制度。
                    ///< 在夏季，将时钟拨快 1 小时。利用夏季日照时间长的特点，让人们早睡早起，减少照明用电。
                    ///< 夏令时：早上 7:00（钟表显示）实际上对应的是原来的 8:00，大家提前一小时起床工作。）
};

clock_t clock(void);
time_t time(time_t * tp);
double difftime(time_t time2, time_t time1);
time_t mktime(struct tm * tp);

char * asctime(const struct tm * tp);
char * ctime(const time_t * tp);
struct tm * gmtime(const time_t *tp);
struct tm *localtime(const time_t * tp);
size_t strftime(char * s, size_t smax, const char * fmt, const struct tm * tp);
void tzset(void);

#endif
