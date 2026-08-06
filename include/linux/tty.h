/*
 * 'tty.h' defines some structures used by tty_io.c and some defines.
 *
 * NOTE! Don't touch this without checking that nothing in rs_io.s or
 * con_io.s breaks. Some constants are hardwired into the system (mainly
 * offsets into 'tty_queue'
 */

#ifndef _TTY_H
#define _TTY_H

#include <termios.h>

#define TTY_BUF_SIZE 1024

/**
 * @brief 终端设备的环形缓冲区，管理终端的输入/输出数据流。
 */
struct tty_queue
{
	unsigned long data;					/* 端口 */
	unsigned long head;					/* 新数据入队位置 */
	unsigned long tail;					/* 数据出队位置 */
	struct task_struct * proc_list;		/* 睡眠在此队列的进程 */
	char buf[TTY_BUF_SIZE];
};

#define INC(a) ((a) = ((a)+1) & (TTY_BUF_SIZE-1))			/* 环形队列增加 */
#define DEC(a) ((a) = ((a)-1) & (TTY_BUF_SIZE-1))			/* 环形队列减少 */
#define EMPTY(a) ((a).head == (a).tail)						/* 环形队列是否为空，初始时 head==tail */
#define LEFT(a) (((a).tail-(a).head-1)&(TTY_BUF_SIZE-1))	/* 剩余多少空间，tail 为下次读取位置，head 为下次写入位置；为空时 tail == head，LEFT 结果为 0x3FF，即剩余最大空间；往里写入时就是 head 一直在增加，环形队列，超越最大位置后回环，后再 head-1 的位置达到最大，表示写满了。*/
#define LAST(a) ((a).buf[(TTY_BUF_SIZE-1)&((a).head-1)])
#define FULL(a) (!LEFT(a))									/* 剩余空间为 0 表示FULL。*/
#define CHARS(a) (((a).head-(a).tail)&(TTY_BUF_SIZE-1))		/* 计算环形队列中拥有多少字符。*/
#define GETCH(queue,c) 										/* 从缓冲区中获取一个字符。*/ \
(void)({c=(queue).buf[(queue).tail];INC((queue).tail);})	/* (void)( ... ) 强制丢弃返回值。({ ... }) —— GNU C 的 statement expression，允许在圆括号内写多个语句，最终表达式的值作为整个块的值。 */
#define PUTCH(c,queue) 										/* 往缓冲区中写入一个字符。*/ \
(void)({(queue).buf[(queue).head]=(c);INC((queue).head);})	/* (void)( ... ) 强制丢弃返回值。({ ... }) —— GNU C 的 statement expression，允许在圆括号内写多个语句，最终表达式的值作为整个块的值。*/

#define INTR_CHAR(tty) ((tty)->termios.c_cc[VINTR])
#define QUIT_CHAR(tty) ((tty)->termios.c_cc[VQUIT])
#define ERASE_CHAR(tty) ((tty)->termios.c_cc[VERASE])
#define KILL_CHAR(tty) ((tty)->termios.c_cc[VKILL])
#define EOF_CHAR(tty) ((tty)->termios.c_cc[VEOF])
#define START_CHAR(tty) ((tty)->termios.c_cc[VSTART])
#define STOP_CHAR(tty) ((tty)->termios.c_cc[VSTOP])
#define SUSPEND_CHAR(tty) ((tty)->termios.c_cc[VSUSP])

/**
 * @brief 终端设备数据结构
 */
struct tty_struct
{
	struct termios termios;						///< 终端配置（波特率、回显、控制字符等）。
	int pgrp;									///< 当前前台进程组 ID。
	int stopped;								///< 输出暂停标志。
	void (*write)(struct tty_struct * tty);		///< 驱动层写函数。
	struct tty_queue read_q;					///< 输入队列。
	struct tty_queue write_q;					///< 用户写入数据缓冲。
	struct tty_queue secondary;					///< 次级队列。
};

extern struct tty_struct tty_table[];

/**
 * @brief 特殊控制字符
 *  intr=^C		quit=^|		erase=del	kill=^U
 *  eof=^D		vtime=\0	vmin=\1		sxtc=\0
 *  start=^Q	stop=^S		susp=^Z		eol=\0
 *  reprint=^R	discard=^U	werase=^W	lnext=^V
 *  eol2=\0
 */
#define INIT_C_CC "\003\034\177\025\004\0\1\0\021\023\032\0\022\017\027\026\0"

void rs_init(void);
void con_init(void);
void tty_init(void);

int tty_read(unsigned c, char * buf, int n);

/**
 * @brief 终端输出。
 * @param buf 待输出字符串。
 * @param count 待输出字节个数。
 */
int tty_write(unsigned c, char * buf, int n);

void rs_write(struct tty_struct * tty);
void con_write(struct tty_struct * tty);

void copy_to_cooked(struct tty_struct * tty);

#endif
