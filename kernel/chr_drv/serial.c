/*
 *	serial.c
 *
 * 本模块实现了 RS-232 串行通信的 I/O 函数
 *	void rs_write(struct tty_struct * queue);
 *	void rs_init(void);
 * 以及所有与串行 I/O 相关的中断处理例程。
 */

#include <linux/tty.h>
#include <linux/sched.h>
#include <asm/system.h>
#include <asm/io.h>

#define WAKEUP_CHARS (TTY_BUF_SIZE/4)

extern void rs1_interrupt(void);	///< 串口（RS-232 / UART）设备的中断处理函数 ，专用于处理 串行通信接口的接收（RX）和发送（TX）中断。
extern void rs2_interrupt(void);	///< 串口2 设备的中断处理函数 ，专用于处理 串行通信接口的接收（RX）和发送（TX）中断。

/**
 * @brief 硬件初始化函数 ，用于将串口从默认状态配置为可通信的 2400 bps、8N1 格式、中断驱动 的串行接口。
 * 完成此初始化后，串口即可响应中断事件（如数据到达），并配合 read()/write() 系统调用实现异步通信。
 */
static void init(int port)
{
	outb_p(0x80, port+3);	/* 第7位DLAB设置为1，表示下一步操作为设置波特率。*/
	outb_p(0x30, port);		/* 写分频系数低字节。设置波特率，2400 bps，设计到计算略。*/
	outb_p(0x00, port+1);	/* 写入分频系数高字节（DLAB==1时为此功能）。MS of divisor */
	outb_p(0x03, port+3);	/* 恢复DLAB设置。*/
	outb_p(0x0b, port+4);	/* 设置调制解调控制寄存器。set DTR,RTS, OUT_2 */
	outb_p(0x0d, port+1);	/* enable all intrs but writes */
	(void)inb(port);		/* 读取 RBR 清除可能的“幽灵中断”，确保初始化后状态干净。read data port to reset things (?) */
}

void rs_init(void)
{
	set_intr_gate(0x24, rs1_interrupt);
	set_intr_gate(0x23, rs2_interrupt);
	init(tty_table[1].read_q.data);		///< 端口为 0x3f8
	init(tty_table[2].read_q.data);		///< 端口为 0x2f8
	outb(inb_p(0x21)&0xE7, 0x21);
}

/*
 * This routine gets called when tty_write has put something into
 * the write_queue. It must check wheter the queue is empty, and
 * set the interrupt register accordingly
 *
 *	void _rs_write(struct tty_struct * tty);
 */
void rs_write(struct tty_struct * tty)
{
	cli();
	if (!EMPTY(tty->write_q))
		outb(inb_p(tty->write_q.data+1)|0x02,tty->write_q.data+1);
	sti();
}
