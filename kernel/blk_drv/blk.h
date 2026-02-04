#ifndef _BLK_H
#define _BLK_H
#define MAJOR_NR 3      ///< 自己加的，实际上也是如此，3 代表硬盘。
#define NR_BLK_DEV	7
/*
 * NR_REQUEST is the number of entries in the request-queue.
 * NOTE that writes may use only the low 2/3 of these: reads
 * take precedence.
 *
 * 32 seems to be a reasonable number: enough to get some benefit
 * from the elevator-mechanism, but not so much as to lock a lot of
 * buffers when they are in the queue. 64 seems to be too many (easily
 * long pauses in reading when heavy writing/syncing is going on)
 */
#define NR_REQUEST	32
/*
 *
 * Ok, this is an expanded form so that we can use the same
 * request for paging requests when that is implemented. In
 * paging, 'bh' is NULL, and 'waiting' is used to wait for
 * read/write completion.
 */
struct request {
	int dev;						///< 设备号，-1表示无设备
	int cmd;						///< READ 或 WRITE
	int errors;
	unsigned long sector;			///< 起始扇区号
	unsigned long nr_sectors;		///< 要读写的扇区数
	char * buffer;
	struct task_struct * waiting;	///< 当前发起 IO 请求的进程
	struct buffer_head * bh;		///< 内存块头
	struct request * next;
};

/*
 * This is used in the elevator algorithm: Note that
 * reads always go before writes. This is natural: reads
 * are much more time-critical than writes.
 */
#define IN_ORDER(s1,s2) \
((s1)->cmd<(s2)->cmd || (s1)->cmd==(s2)->cmd && \
((s1)->dev < (s2)->dev || ((s1)->dev == (s2)->dev && \
(s1)->sector < (s2)->sector)))

struct blk_dev_struct {
	void (*request_fn)(void);				///< 请求处理函数指针
	struct request * current_request;		///< 当前正在处理的请求
};

extern struct blk_dev_struct blk_dev[NR_BLK_DEV];
extern struct request request[NR_REQUEST];
extern struct task_struct * wait_for_request;

#ifdef MAJOR_NR

/*
 * Add entries as needed. Currently the only block devices
 * supported are hard-disks and floppies.
 */

#if (MAJOR_NR == 1)
	/* ram disk */
	#define DEVICE_NAME "ramdisk"
	#define DEVICE_REQUEST do_rd_request
	#define DEVICE_NR(device) ((device) & 7)
	#define DEVICE_ON(device) 
	#define DEVICE_OFF(device)

#elif (MAJOR_NR == 2)
	/* floppy */
	#define DEVICE_NAME "floppy"
	#define DEVICE_INTR do_floppy
	#define DEVICE_REQUEST do_fd_request
	#define DEVICE_NR(device) ((device) & 3)
	#define DEVICE_ON(device) floppy_on(DEVICE_NR(device))
	#define DEVICE_OFF(device) floppy_off(DEVICE_NR(device))

#elif (MAJOR_NR == 3)
	/* harddisk */
	#define DEVICE_NAME "harddisk"	                  ///< 硬盘名
	#define DEVICE_INTR do_hd
	#define DEVICE_REQUEST do_hd_request
	#define DEVICE_NR(device) (MINOR(device)/5)
	#define DEVICE_ON(device)
	#define DEVICE_OFF(device)

#elif
/* unknown blk device */
#error "unknown blk device"

#endif

#define CURRENT (blk_dev[MAJOR_NR].current_request)		/* 当前请求 */
#define CURRENT_DEV DEVICE_NR(CURRENT->dev)				/* 当前请求的设备 */

#ifdef DEVICE_INTR
void (*DEVICE_INTR)(void) = NULL;
#endif
static void (DEVICE_REQUEST)(void);
/* 去除锁标志，并唤醒等待在此内存的进程 */
extern inline void unlock_buffer(struct buffer_head * bh)
{
	if (!bh->b_lock)
		printk(DEVICE_NAME ": free buffer being unlocked\n");
	bh->b_lock=0;
	wake_up(&bh->b_wait);                       ///< 唤醒等待在此内存块的进程
}
/* 更新磁盘处理结果，删除磁盘 IO 队列头的请求（因为这个就是处理第一个 IO 请求的结果），即内存是否映射了磁盘块，并唤醒等待 buffer 的进程 */
extern inline void end_request(int uptodate)	///< uptodate = 0
{
	DEVICE_OFF(CURRENT->dev);					///< hard disk : do nothing
	if (CURRENT->bh) {
		CURRENT->bh->b_uptodate = uptodate;		///< 更新磁盘块是否是最新的
		unlock_buffer(CURRENT->bh);             ///< 唤醒等待此 buffer 的进程，即使没有读取到磁盘也唤醒，问题让用户进程去处理
	}
	if (!uptodate) {
		printk(DEVICE_NAME " I/O error\n\r");   ///< 打印错误日志
		printk("dev %04x, block %d\n\r",CURRENT->dev,
			CURRENT->bh->b_blocknr);
	}
	wake_up(&CURRENT->waiting);     ///< 唤醒等待此信号的进程
	wake_up(&wait_for_request);     ///< 唤醒外层等待队列里的进程
	CURRENT->dev = -1;              ///< 将请求的设备置空
	CURRENT = CURRENT->next;        ///< 切换到下一个请求
}
/* 对 IO 队列进行初步校验。1.无 IO 请求则 return；2.主设备号检测；3.锁定检测。 */
#define INIT_REQUEST \
repeat: \
	if (!CURRENT)	/* 如果当前无请求，则返回。 */ \
		return; \
	if (MAJOR(CURRENT->dev) != MAJOR_NR)	/* 检查主设备号 0x300，主设备号为 3。 */ \
		panic(DEVICE_NAME ": request list destroyed"); \
	if (CURRENT->bh) { \
		if (!CURRENT->bh->b_lock)	/* 检查是否被锁定 */ \
			panic(DEVICE_NAME ": block not locked"); \
	}
#endif
#endif