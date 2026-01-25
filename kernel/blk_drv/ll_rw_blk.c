/*
 *  linux/kernel/blk_dev/ll_rw.c
 *
 * (C) 1991 Linus Torvalds
 */

/*
 * This handles all read/write requests to block devices
 */
#include <errno.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/system.h>

#include "blk.h"

/*
 * The request-struct contains all necessary data
 * to load a nr of sectors into memory
 */
struct request request[NR_REQUEST];                ///< 请求

/* 如果 request 数组被用没了，就让当前进程等待在 wait_for_request 上
 * 
 */
struct task_struct * wait_for_request = NULL;

/* blk_dev_struct is:
 *    do_request-address
 *    next-request
 */
struct blk_dev_struct blk_dev[NR_BLK_DEV] = {
    { NULL, NULL },        /* no_dev */
    { NULL, NULL },        /* dev mem */
    { NULL, NULL },        /* dev fd (floopy disk) */
    { NULL, NULL },        /* dev hd (hard disk) */
    { NULL, NULL },        /* dev ttyx */
    { NULL, NULL },        /* dev tty */
    { NULL, NULL }        /* dev lp */
};
/* 锁定 buffer，多进程只有一个能锁定成功，其他进程会等待在 b_wait 上。*/
static inline void lock_buffer(struct buffer_head * bh)
{
    cli();                      ///< 关中断（清除 eflags 寄存器中的 IF 位，防止在检查锁定状态和设置锁定状态时被打断）
    while (bh->b_lock)          ///< 检查当前缓冲区是否已被锁定
        sleep_on(&bh->b_wait);  ///< 如果已被锁定，等待在 b_wait 上。
    bh->b_lock=1;               ///< 设置锁定标志。
    sti();                      ///< 开中断
}

static inline void unlock_buffer(struct buffer_head * bh)
{
    if (!bh->b_lock)
        printk("ll_rw_block.c: buffer not locked\n\r");
    bh->b_lock = 0;
    wake_up(&bh->b_wait);
}

/*
 * add-request adds a request to the linked list.
 * It disables interrupts so that it can muck with the
 * request-lists in peace.
 */
static void add_request(struct blk_dev_struct * dev, struct request * req)
{
    struct request * tmp;

    req->next = NULL;
    cli();
    if (req->bh)                            ///< buffer_head不空
        req->bh->b_dirt = 0;
    if (!(tmp = dev->current_request)) {    ///< 如果没有请求，设置请求为当前请求
        dev->current_request = req;
        sti();
        (dev->request_fn)();                ///< 进行硬盘初始化（重置硬盘控制器，重设硬盘参数）。
        return;
    }
    for ( ; tmp->next ; tmp=tmp->next)
        if ((IN_ORDER(tmp,req) ||
            !IN_ORDER(tmp,tmp->next)) &&
            IN_ORDER(req,tmp->next))
            break;
    req->next=tmp->next;
    tmp->next=req;
    sti();
}
/* 将请求加入到 blk_dev 的请求队列中，major 为硬盘主设备号（0x0300 的主设备号为 3） */
static void make_request(int major,int rw, struct buffer_head * bh)        ///< major = 0b0011，rw = READ，bh 为空闲链表头
{
    struct request * req;
    int rw_ahead;

    /* WRITEA/READA是预读写，起始并不是真正的需要，因此，如果内存块是锁定的，那就不用处理了 */
    /* 否则将请求当作是常规的读写请求 */
    if (rw_ahead = (rw == READA || rw == WRITEA)) {
        if (bh->b_lock)     ///< 如果被锁定，就不用处理了。
            return;
        if (rw == READA)
            rw = READ;
        else
            rw = WRITE;
    }   /* 走下来的话分两种情况，1.不是预读写；2.预读写但是内存块没有被锁定。 */
    if (rw!=READ && rw!=WRITE)
        panic("Bad block dev command, must be R/W/RA/WA");
    lock_buffer(bh);
    if ((rw == WRITE && !bh->b_dirt) || (rw == READ && bh->b_uptodate)) {        ///< 如果是读请求，判断内存中的磁盘块是否是最新的，如果是最新的，直接返回
        unlock_buffer(bh);
        return;
    }
repeat:
/* we don't allow the write-requests to fill up the queue completely:
 * we want some room for reads: they take precedence. The last third            ///< 前 2/3 作为写请求
 * of the requests are only for reads.        ///< 后 1/3 作为读请求
 */
    if (rw == READ)
        req = request + NR_REQUEST;            ///< NR_REQUEST = 32
    else
        req = request+((NR_REQUEST*2)/3);
/* find an empty request */
    while (--req >= request)
        if (req->dev<0)                        ///< 找到了设备
            break;
/* if none found, sleep on new requests: check for rw_ahead */
    if (req < request) {                    ///< 当前请求已经满了（超过 request 数组的大小了）
        if (rw_ahead) {                        ///< 是否是预读请求
            unlock_buffer(bh);                  ///< 队列满时可直接丢弃（避免阻塞主请求），由上层应用重试。
            return;
        }
        sleep_on(&wait_for_request);            ///< 如果 request 都用完了，使当前进程进入睡眠状态，等待 wait_for_request 等待队列被唤醒。
        goto repeat;
    }
/* fill up the request-info, and add it to the queue */
    req->dev = bh->b_dev;
    req->cmd = rw;
    req->errors=0;
    req->sector = bh->b_blocknr<<1;             ///< 1 个内核块 = 2 个磁盘扇区
    req->nr_sectors = 2;
    req->buffer = bh->b_data;                ///< 内核块的位置
    req->waiting = NULL;
    req->bh = bh;
    req->next = NULL;
    add_request(major + blk_dev, req);          ///< 将当前读写请求加入队列，major = 0b0011
}
/* 将请求加入到队列中 */
void ll_rw_block(int rw, struct buffer_head * bh)
{
    unsigned int major;

    if ((major=MAJOR(bh->b_dev)) >= NR_BLK_DEV ||                    ///< major = 0
    !(blk_dev[major].request_fn)) {
        printk("Trying to read nonexistent block-device\n\r");
        return;
    }
    make_request(major, rw, bh);                                    ///< 将请求加入 blk_dev 请求队列中。major = 0b0011，rw = READ，bh 为空闲链表头
}

void blk_dev_init(void)
{
    int i;

    for (i=0 ; i<NR_REQUEST ; i++) {
        request[i].dev = -1;                                        ///< 初始化读写请求的设备号为 -1
        request[i].next = NULL;
    }
}
