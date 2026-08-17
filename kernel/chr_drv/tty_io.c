/*
 * 'tty_io.c' 'tty_io.c' 使终端（TTY）设备呈现出一种正交（即统一、通用）的编程接口，
 * 无论它们是控制台（console）还是串口通道（如 RS-232 串行端口）。
 * 此模块还实现了回显（echoing）、规范模式（cooked mode）等功能。
 * “行删除”（kill-line）功能感谢 John T. Kohl 的贡献。
 */
#include <ctype.h>
#include <errno.h>
#include <signal.h>

#define ALRMMASK (1<<(SIGALRM-1))
#define KILLMASK (1<<(SIGKILL-1))
#define INTMASK (1<<(SIGINT-1))
#define QUITMASK (1<<(SIGQUIT-1))
#define TSTPMASK (1<<(SIGTSTP-1))

#include <linux/sched.h>
#include <linux/tty.h>
#include <asm/segment.h>
#include <asm/system.h>

#define _L_FLAG(tty,f)	((tty)->termios.c_lflag & f)
#define _I_FLAG(tty,f)	((tty)->termios.c_iflag & f)
#define _O_FLAG(tty,f)	((tty)->termios.c_oflag & f)	/* 判断输出 f 位是否存在。*/

#define L_CANON(tty)	_L_FLAG((tty),ICANON)
#define L_ISIG(tty)	_L_FLAG((tty),ISIG)
#define L_ECHO(tty)	_L_FLAG((tty),ECHO)
#define L_ECHOE(tty)	_L_FLAG((tty),ECHOE)
#define L_ECHOK(tty)	_L_FLAG((tty),ECHOK)
#define L_ECHOCTL(tty)	_L_FLAG((tty),ECHOCTL)
#define L_ECHOKE(tty)	_L_FLAG((tty),ECHOKE)

#define I_UCLC(tty)	_I_FLAG((tty),IUCLC)
#define I_NLCR(tty)	_I_FLAG((tty),INLCR)
#define I_CRNL(tty)	_I_FLAG((tty),ICRNL)
#define I_NOCR(tty)	_I_FLAG((tty),IGNCR)

#define O_POST(tty)	_O_FLAG((tty),OPOST)		/* 是否包含 启用输出处理（如 NL->CR/NL 转换，回车->换行/回车）*/
#define O_NLCR(tty)	_O_FLAG((tty),ONLCR)		/* \n 转 \r */
#define O_CRNL(tty)	_O_FLAG((tty),OCRNL)		/* 将输出的回车（Carriage Return, \r）自动转换为换行（Line Feed, \n）*/
#define O_NLRET(tty)	_O_FLAG((tty),ONLRET)	/* 当输出 \n（换行）时，自动附加一个 \r（回车）*/
#define O_LCUC(tty)	_O_FLAG((tty),OLCUC)		/* 输出时将小写字母转换为大写字母。*/

/**
 * @brief 终端结构体
 */
struct tty_struct tty_table[] = 
{
	/// /dev/tty1
	{
		{
			ICRNL,			/* 输入模式：将回车更换为换行。*/
			OPOST|ONLCR,	/* 输出模式：启用输出处理 | 将换行更换为回车+换行。*/
			0,
			ISIG | ICANON | ECHO | ECHOCTL | ECHOKE,	/* 自动将键盘输入转换为信号 | 终端以行为单位处理输入 | 启用输入回显 | 控制字符^X显示 | 删除操作回显为^H */
			0,				/* 标准终端处理 */
			INIT_C_CC		/* 特殊控制字符 */
		},
		0,					/* 当前前台进程组 ID */
		0,					/* 输出暂停标志 */
		con_write,			/* 驱动层写函数 */
		{0,0,0,0,""},		/* console read-queue */
		{0,0,0,0,""},		/* console write-queue */
		{0,0,0,0,""}		/* console secondary queue */
	},
	/// /dev/ttyS0
	{
		{
			0, 				/* 输入模式：不将回车更换为换行 */
			0,  			/* 输出模式：不启用输出处理 | 不将换行更换为回车+换行。*/
			B2400 | CS8,	/* 2400波特率 | 数据位为 8 位，终端设备以 8 个数据位（无校验位）进行串行通信。 */
			0,				/* 无任何操作 */
			0,				/* 标准终端处理 */
			INIT_C_CC		/* 特殊控制字符 */
		},
		0,					/* 当前前台进程组 ID */
		0,					/* 输出暂停标志 */
		rs_write,			/* 驱动层写函数 */
		{0x3f8,0,0,0,""},	/* 读队列，0x3f8为第一个串口设备的数据端口。*/
		{0x3f8,0,0,0,""},	/* 写队列，0x3f8为第一个串口设备的数据端口。*/
		{0,0,0,0,""}
	},
	/// /dev/ttyS1
	{
		{
			0, 				/* 输入模式：不将回车更换为换行 */
			0,  			/* 输出模式：不启用输出处理 | 不将换行更换为回车+换行。*/
			B2400 | CS8,	/* 2400波特率 | 数据位为 8 位，终端设备以 8 个数据位（无校验位）进行串行通信。*/
			0,				/* 无任何操作 */
			0,				/* 标准终端处理 */
			INIT_C_CC		/* 特殊控制字符 */
		},
		0,					/* 当前前台进程组 ID */
		0,					/* 输出暂停标志 */
		rs_write,			/* 驱动层写函数 */
		{0x2f8,0,0,0,""},	/* 读队列，0x3f8为第二个串口设备的数据端口。*/
		{0x2f8,0,0,0,""},	/* 写队列，0x3f8为第二个串口设备的数据端口。*/
		{0,0,0,0,""}
	}
};

/*
 * these are the tables used by the machine code handlers.
 * you can implement pseudo-tty's or something by changing
 * them. Currently not done.
 */
struct tty_queue * table_list[] =
{
	&tty_table[0].read_q, &tty_table[0].write_q,
	&tty_table[1].read_q, &tty_table[1].write_q,
	&tty_table[2].read_q, &tty_table[2].write_q
};

void tty_init(void)
{
	rs_init();		///< COM串口初始化。
	con_init();
}

void tty_intr(struct tty_struct * tty, int mask)
{
	int i;

	if (tty->pgrp <= 0)
		return;
	for (i=0;i<NR_TASKS;i++)
		if (task[i] && task[i]->pgrp==tty->pgrp)
			task[i]->signal |= mask;
}

static void sleep_if_empty(struct tty_queue * queue)
{
	cli();
	while (!current->signal && EMPTY(*queue))
		interruptible_sleep_on(&queue->proc_list);
	sti();
}
/**
 * @brief 如果队列满了，就睡眠等待这个队列空闲。
 * @param queue 终端任务队列
 */ 
static void sleep_if_full(struct tty_queue * queue)
{
	if (!FULL(*queue))		///< 队列还有空间，则直接返回。
		return;
	cli();
	while (!current->signal && LEFT(*queue) < 128)		///< 当 没有信号 && 剩余空间少于 128 字节时，睡眠。
		interruptible_sleep_on(&queue->proc_list);		///< 可中断睡眠。
	sti();
}

void wait_for_keypress(void)
{
	sleep_if_empty(&tty_table[0].secondary);
}

void copy_to_cooked(struct tty_struct * tty)
{
	signed char c;

	while (!EMPTY(tty->read_q) && !FULL(tty->secondary)) {
		GETCH(tty->read_q,c);
		if (c==13)
			if (I_CRNL(tty))
				c=10;
			else if (I_NOCR(tty))
				continue;
			else ;
		else if (c==10 && I_NLCR(tty))
			c=13;
		if (I_UCLC(tty))
			c=tolower(c);
		if (L_CANON(tty)) {
			if (c==KILL_CHAR(tty)) {
				/* deal with killing the input line */
				while(!(EMPTY(tty->secondary) ||
				        (c=LAST(tty->secondary))==10 ||
				        c==EOF_CHAR(tty))) {
					if (L_ECHO(tty)) {
						if (c<32)
							PUTCH(127,tty->write_q);
						PUTCH(127,tty->write_q);
						tty->write(tty);
					}
					DEC(tty->secondary.head);
				}
				continue;
			}
			if (c==ERASE_CHAR(tty)) {
				if (EMPTY(tty->secondary) ||
				   (c=LAST(tty->secondary))==10 ||
				   c==EOF_CHAR(tty))
					continue;
				if (L_ECHO(tty)) {
					if (c<32)
						PUTCH(127,tty->write_q);
					PUTCH(127,tty->write_q);
					tty->write(tty);
				}
				DEC(tty->secondary.head);
				continue;
			}
			if (c==STOP_CHAR(tty)) {
				tty->stopped=1;
				continue;
			}
			if (c==START_CHAR(tty)) {
				tty->stopped=0;
				continue;
			}
		}
		if (L_ISIG(tty)) {
			if (c==INTR_CHAR(tty)) {
				tty_intr(tty,INTMASK);
				continue;
			}
			if (c==QUIT_CHAR(tty)) {
				tty_intr(tty,QUITMASK);
				continue;
			}
		}
		if (c==10 || c==EOF_CHAR(tty))
			tty->secondary.data++;
		if (L_ECHO(tty)) {
			if (c==10) {
				PUTCH(10,tty->write_q);
				PUTCH(13,tty->write_q);
			} else if (c<32) {
				if (L_ECHOCTL(tty)) {
					PUTCH('^',tty->write_q);
					PUTCH(c+64,tty->write_q);
				}
			} else
				PUTCH(c,tty->write_q);
			tty->write(tty);
		}
		PUTCH(c,tty->secondary);
	}
	wake_up(&tty->secondary.proc_list);
}

int tty_read(unsigned channel, char * buf, int nr)
{
	struct tty_struct * tty;
	char c, * b=buf;
	int minimum,time,flag=0;
	long oldalarm;

	if (channel>2 || nr<0) return -1;
	tty = &tty_table[channel];
	oldalarm = current->alarm;
	time = 10L*tty->termios.c_cc[VTIME];
	minimum = tty->termios.c_cc[VMIN];
	if (time && !minimum) {
		minimum=1;
		if (flag=(!oldalarm || time+jiffies<oldalarm))
			current->alarm = time+jiffies;
	}
	if (minimum>nr)
		minimum=nr;
	while (nr>0) {
		if (flag && (current->signal & ALRMMASK)) {
			current->signal &= ~ALRMMASK;
			break;
		}
		if (current->signal)
			break;
		if (EMPTY(tty->secondary) || (L_CANON(tty) &&
		!tty->secondary.data && LEFT(tty->secondary)>20)) {
			sleep_if_empty(&tty->secondary);
			continue;
		}
		do {
			GETCH(tty->secondary,c);
			if (c==EOF_CHAR(tty) || c==10)
				tty->secondary.data--;
			if (c==EOF_CHAR(tty) && L_CANON(tty))
				return (b-buf);
			else {
				put_fs_byte(c,b++);
				if (!--nr)
					break;
			}
		} while (nr>0 && !EMPTY(tty->secondary));
		if (time && !L_CANON(tty))
			if (flag=(!oldalarm || time+jiffies<oldalarm))
				current->alarm = time+jiffies;
			else
				current->alarm = oldalarm;
		if (L_CANON(tty)) {
			if (b-buf)
				break;
		} else if (b-buf >= minimum)
			break;
	}
	current->alarm = oldalarm;
	if (current->signal && !(b-buf))
		return -EINTR;
	return (b-buf);
}

int tty_write(unsigned channel, char * buf, int nr)		///< channel = 0
{
	static cr_flag = 0;
	struct tty_struct * tty;
	char c, *b = buf;

	if (channel > 2 || nr < 0)
		return -1;
	tty = channel + tty_table;
	while (nr > 0)
	{
		sleep_if_full(&tty->write_q);					///< 写入队列满了则睡眠在这里。
		if (current->signal)							///< 若有信号位图，则停止输出。
			break;
		while (nr > 0 && !FULL(tty->write_q)) 
		{
			c = get_fs_byte(b);							///< 获取 1 字节。
			if (O_POST(tty)) 							///< 判断是否包含输出处理（如 NL->CR/NL 转换，回车->换行/回车）
			{
				if (c == '\r' && O_CRNL(tty))			///< 回车转换行
					c = '\n';
				else if (c == '\n' && O_NLRET(tty))
					c = '\r';
				if (c == '\n' && !cr_flag && O_NLCR(tty)) 
				{
					cr_flag = 1;
					PUTCH(13, tty->write_q);			///< 往输出缓冲区中写入一个字符。
					continue;
				}
				if (O_LCUC(tty))
					c = toupper(c);
			}
			b++;
			nr--;
			cr_flag = 0;
			PUTCH(c, tty->write_q);
		}
		tty->write(tty);	///< 在 tty_table 中进行初始化的。
		if (nr>0)
			schedule();
	}
	return (b - buf);		///< 返回写了多少字节。
}

/*
 * Jeh, sometimes I really like the 386.
 * This routine is called from an interrupt,
 * and there should be absolutely no problem
 * with sleeping even in an interrupt (I hope).
 * Of course, if somebody proves me wrong, I'll
 * hate intel for all time :-). We'll have to
 * be careful and see to reinstating the interrupt
 * chips before calling this, though.
 *
 * I don't think we sleep here under normal circumstances
 * anyway, which is good, as the task sleeping might be
 * totally innocent.
 */
void do_tty_interrupt(int tty)
{
	copy_to_cooked(tty_table+tty);
}

void chr_dev_init(void)
{
}
