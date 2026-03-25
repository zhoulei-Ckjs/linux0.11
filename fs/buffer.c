/*
 *  linux/fs/buffer.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 *  'buffer.c' implements the buffer-cache functions. Race-conditions have
 * been avoided by NEVER letting a interrupt change a buffer (except for the
 * data, of course), but instead letting the caller do it. NOTE! As interrupts
 * can wake up a caller, some cli-sti sequences are needed to check for
 * sleep-on-calls. These should be extremely quick, though (I hope).
 */

/*
 * NOTE! There is one discordant note here: checking floppies for
 * disk change. This is where it fits best, I think, as it should
 * invalidate changed floppy-disk-caches.
 */

#include <stdarg.h>
 
#include <linux/config.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/system.h>
#include <asm/io.h>

extern int end;
struct buffer_head * start_buffer = (struct buffer_head *) &end;        ///< end 为代码段、数据段以及 bss 段的结束，定义在 link.ld 中
struct buffer_head * hash_table[NR_HASH];
static struct buffer_head * free_list;
static struct task_struct * buffer_wait = NULL;
int NR_BUFFERS = 0;         ///< buffer 块个数

/* 等待读取完缓冲区（主要由硬盘初始化后触发 IRQ14 中断来完成） */
static inline void wait_on_buffer(struct buffer_head * bh)
{
    cli();                  ///< 关中断
    while (bh->b_lock)
        sleep_on(&bh->b_wait);
    sti();                  ///< 开中断
}

int sys_sync(void)
{
    int i;
    struct buffer_head * bh;

    sync_inodes();        /* write out inodes into buffers */
    bh = start_buffer;
    for (i=0 ; i<NR_BUFFERS ; i++,bh++) {
        wait_on_buffer(bh);
        if (bh->b_dirt)
            ll_rw_block(WRITE,bh);
    }
    return 0;
}

/**
 * @brief 完成磁盘同步
 */
int sync_dev(int dev)       ///< 如：dev = 0x306
{
    int i;
    struct buffer_head * bh;

    bh = start_buffer;
    for (i=0 ; i < NR_BUFFERS ; i++, bh++) 
    {
        if (bh->b_dev != dev)
            continue;
        wait_on_buffer(bh);                 ///< 等待未锁定
        if (bh->b_dev == dev && bh->b_dirt) ///< 数据为脏
            ll_rw_block(WRITE, bh);
    }
    sync_inodes();                          ///< 同步所有 inode 到磁盘。
    bh = start_buffer;

    /// 上面的 for 循环会将所有的 bh 都锁定，这个 for 循环会等待所有 bh 解锁（在硬盘完成同步 buffer_head 后会解锁）
    /// 所以下面这个 for 循环走完才算完成同步。
    for (i=0 ; i < NR_BUFFERS ; i++, bh++) 
    {
        if (bh->b_dev != dev)
            continue;
        wait_on_buffer(bh);
        if (bh->b_dev == dev && bh->b_dirt)
            ll_rw_block(WRITE, bh);
    }
    return 0;
}

void inline invalidate_buffers(int dev)
{
    int i;
    struct buffer_head * bh;

    bh = start_buffer;
    for (i=0 ; i<NR_BUFFERS ; i++,bh++) {
        if (bh->b_dev != dev)
            continue;
        wait_on_buffer(bh);
        if (bh->b_dev == dev)
            bh->b_uptodate = bh->b_dirt = 0;
    }
}

/*
 * This routine checks whether a floppy has been changed, and
 * invalidates all buffer-cache-entries in that case. This
 * is a relatively slow routine, so we have to try to minimize using
 * it. Thus it is called only upon a 'mount' or 'open'. This
 * is the best way of combining speed and utility, I think.
 * People changing diskettes in the middle of an operation deserve
 * to loose :-)
 *
 * NOTE! Although currently this is only for floppies, the idea is
 * that any additional removable block-device will use this routine,
 * and that mount/open needn't know that floppies/whatever are
 * special.
 */
void check_disk_change(int dev)     ///< 对 floppy 的检查
{
    int i;

    if (MAJOR(dev) != 2)
        return;
    if (!floppy_change(dev & 0x03))
        return;
    for (i=0 ; i<NR_SUPER ; i++)
        if (super_block[i].s_dev == dev)
            put_super(super_block[i].s_dev);
    invalidate_inodes(dev);
    invalidate_buffers(dev);
}

/* 内存 hash 桶 */
#define _hashfn(dev,block) (((unsigned)(dev^block))%NR_HASH)
#define hash(dev,block) hash_table[_hashfn(dev,block)]

/* 将内存块移出 hash 表和 free_list */
static inline void remove_from_queues(struct buffer_head * bh)
{
/* remove from hash-queue */
    if (bh->b_next)
        bh->b_next->b_prev = bh->b_prev;
    if (bh->b_prev)
        bh->b_prev->b_next = bh->b_next;
    if (hash(bh->b_dev,bh->b_blocknr) == bh)
        hash(bh->b_dev,bh->b_blocknr) = bh->b_next;
/* remove from free list */
    if (!(bh->b_prev_free) || !(bh->b_next_free))
        panic("Free block list corrupted");
    bh->b_prev_free->b_next_free = bh->b_next_free;
    bh->b_next_free->b_prev_free = bh->b_prev_free;
    if (free_list == bh)
        free_list = bh->b_next_free;
}

/* 插入到空闲队列尾部，并放入 hash_table 中 */
static inline void insert_into_queues(struct buffer_head * bh)
{
/* put at end of free list */
    bh->b_next_free = free_list;
    bh->b_prev_free = free_list->b_prev_free;
    free_list->b_prev_free->b_next_free = bh;
    free_list->b_prev_free = bh;

/* put the buffer in new hash-queue if it has a device */
    bh->b_prev = NULL;
    bh->b_next = NULL;
    if (!bh->b_dev)            ///< 0x300
        return;

    /// 放入到 hash_table 的桶中
    bh->b_next = hash(bh->b_dev,bh->b_blocknr);        
    hash(bh->b_dev,bh->b_blocknr) = bh;
    bh->b_next->b_prev = bh;
}

/* 从 hash 桶中寻找对应设备 dev 的对应块 block，找到了则返回这个内存，找不到返回 NULL */
static struct buffer_head * find_buffer(int dev, int block)
{
    struct buffer_head * tmp;

    for (tmp = hash(dev, block); tmp != NULL; tmp = tmp->b_next)
        if (tmp->b_dev == dev && tmp->b_blocknr == block)
            return tmp;
    return NULL;
}

/**
 * @brief 获取到对应设备 dev 的对应块 block 对应的内存映射 buffer_head 并增加引用计数。 未找到则返回 NULL。
 */
struct buffer_head * get_hash_table(int dev, int block)
{
    struct buffer_head * bh;

    for (;;) 
    {
        if (!(bh = find_buffer(dev, block)))
            return NULL;
        bh->b_count++;    ///< 增加引用计数
        wait_on_buffer(bh);
        if (bh->b_dev == dev && bh->b_blocknr == block)
            return bh;
        bh->b_count--;
    }
}

/**
 * @ brief 获取一个空闲链表头，如果当前块在 hash_table 里，则直接返回这个块；
 * 如果不在 hash_table 里，从空闲链表里找到一块，如果这个块被占用，则同步磁盘进行释放。
 * 将这个块插入 hash_table。
 */
#define BADNESS(bh) (((bh)->b_dirt<<1)+(bh)->b_lock)
struct buffer_head * getblk(int dev,int block)
{
    struct buffer_head * tmp, * bh;

repeat:
    /// 如果当前块在 hash_table 里，则直接返回
    if (bh = get_hash_table(dev, block))
        return bh;

    tmp = free_list;                    ///< 可用内存循环链表头
    do {
        if (tmp->b_count)               ///< 判断内存块是否被使用，是就继续找下一块
            continue;
        if (!bh || BADNESS(tmp) < BADNESS(bh)) ///< !bh为首次进入的情况，BADNESS(tmp) < BADNESS(bh)为找到了更好的 buffer_head
        {
            bh = tmp;                   ///< 找到了内存块
            if (!BADNESS(tmp))          ///< 不是脏的 & 不被锁定
                break;
        }
/* and repeat until we find something good */
    } while ((tmp = tmp->b_next_free) != free_list);        ///< 循环检查一圈，来寻找空闲内存块
    
    /// 找了一圈都没找到，睡一觉重新找
    if (!bh) 
    {
        sleep_on(&buffer_wait);
        goto repeat;
    }

    /// 走到这里有两种情况。
    /// 第一种情况：找到了一块干净的 buffer_head，即 b_dirt 和 b_lock 都为 0，这是最理想的情况。
    /// 第二种情况：找了一圈没有干净的 buffer_head，说明被用空了，这时由上面的 while 循环可知，bh=free_list，
    ///             即拿到了最久的 buffer_head，因为free_list是尾插法（新用一块就插入 free_list 末尾）
    ///             这个时候就需要将这个内存同步到对应的磁盘中再拿来用了。
    wait_on_buffer(bh);      ///< 等待 bh 不被锁定。
    if (bh->b_count)         ///< 如果还有人在使用这个内存，重新找。
        goto repeat;

    /// 走到这里，说明找到一块没人用的内存块，如果当前块为脏，则需要写入磁盘
    while (bh->b_dirt)
    {
        sync_dev(bh->b_dev);    ///< 完成磁盘同步之后会解锁 bh。
        wait_on_buffer(bh);
        if (bh->b_count)
            goto repeat;
    }

/* NOTE!! While we slept waiting for this block, somebody else might */
/* already have added "this" block to the cache. check it */
    if (find_buffer(dev, block))
        goto repeat;
/* OK, FINALLY we know that this buffer is the only one of it's kind, */
/* and that it's unused (b_count=0), unlocked (b_lock=0), and clean */
    bh->b_count = 1;
    bh->b_dirt = 0;
    bh->b_uptodate=0;                           ///< 并不是磁盘中最新的
    remove_from_queues(bh);                     ///< 将内存块移出 hash 表和 free_list。
    bh->b_dev = dev;
    bh->b_blocknr = block;
    insert_into_queues(bh);                     ///< 放入尾部。
    return bh;
}

/* 释放缓冲区，唤醒没有缓冲区可用的进程 */
void brelse(struct buffer_head * buf)
{
    if (!buf)                       ///< 如果缓冲区为空，则直接返回。
        return;
    wait_on_buffer(buf);            ///< 等待缓冲区没有被锁定
    if (!(buf->b_count--))          ///< 减少使用标记
        panic("Trying to free free buffer");    ///< 逻辑错误，缓冲区已被释放
    wake_up(&buffer_wait);          ///< 这个队列中是拿不到缓冲区的队列，那我这里既然要释放该缓冲区了，说明这个缓冲区可以给别人用了，所以这里唤醒等获取缓冲区的进程。
}

/*
 * bread() 读取一个特定的块到缓冲区中并且返回这个缓冲区
 * 会等待读取完成，读取块大小为 512 * 2 = 1K。
 * 如果无法读取，返回 NULL。（第一次读取了 MBR，第二次读取了 0x306）。
 */
struct buffer_head * bread(int dev,int block)               ///< dev = 0x300, block = 0
{
    struct buffer_head * bh;

    if (!(bh = getblk(dev, block)))                         ///< 获取一个空闲链表头，这个链表头指向了空闲链表
        panic("bread: getblk returned NULL\n");
    if (bh->b_uptodate)                                     ///< 内存块最新（与硬盘内容一致），则返回内存块。如果不一致的话就需要等待读取硬盘块到内存块中。
        return bh;
    ll_rw_block(READ, bh);                                  ///< 创建读取硬盘块请求，这里是读取MBR。 
    wait_on_buffer(bh);                                     ///< 进行进程切换，让出 CPU 并等待读取磁盘完成（主要是IRQ14中断触发来完成硬盘读取）。
    if (bh->b_uptodate)                                     ///< 当前内存与硬盘一致则返回 buffer_head，buffer_head里面存储了与硬盘映射后的内存。
        return bh;
    brelse(bh);                                             ///< 读取失败，释放缓冲区
    return NULL;
}

#define COPYBLK(from,to) \
__asm__("cld\n\t" \
    "rep\n\t" \
    "movsl\n\t" \
    ::"c" (BLOCK_SIZE/4),"S" (from),"D" (to) \
    :"cx","di","si")

/*
 * bread_page reads four buffers into memory at the desired address. It's
 * a function of its own, as there is some speed to be got by reading them
 * all at the same time, not waiting for one to be read, and then another
 * etc.
 */
void bread_page(unsigned long address,int dev,int b[4])
{
    struct buffer_head * bh[4];
    int i;

    for (i=0 ; i<4 ; i++)
        if (b[i]) {
            if (bh[i] = getblk(dev,b[i]))
                if (!bh[i]->b_uptodate)
                    ll_rw_block(READ,bh[i]);
        } else
            bh[i] = NULL;
    for (i=0 ; i<4 ; i++,address += BLOCK_SIZE)
        if (bh[i]) {
            wait_on_buffer(bh[i]);
            if (bh[i]->b_uptodate)
                COPYBLK((unsigned long) bh[i]->b_data,address);
            brelse(bh[i]);
        }
}

/*
 * Ok, breada can be used as bread, but additionally to mark other
 * blocks for reading as well. End the argument list with a negative
 * number.
 */
struct buffer_head * breada(int dev,int first, ...)
{
    va_list args;
    struct buffer_head * bh, *tmp;

    va_start(args,first);
    if (!(bh=getblk(dev,first)))
        panic("bread: getblk returned NULL\n");
    if (!bh->b_uptodate)
        ll_rw_block(READ,bh);
    while ((first=va_arg(args,int))>=0) {
        tmp=getblk(dev,first);
        if (tmp) {
            if (!tmp->b_uptodate)
                ll_rw_block(READA,bh);
            tmp->b_count--;
        }
    }
    va_end(args);
    wait_on_buffer(bh);
    if (bh->b_uptodate)
        return bh;
    brelse(bh);
    return (NULL);
}

/* 
 * 初始化缓存磁盘的内存（需要将磁盘块读入到内存块中来访问，专门留了部分内存来进行磁盘->内存映射），这部分为 free_list
 * 初始化 hash_table，分配出去的 buffer_head 会插入到这个 hash 表中。
 */
void buffer_init(long buffer_end)                   ///< buffer_end = 4*1024*1024
{
    struct buffer_head * h = start_buffer;          ///< 代码段、数据段以及 bss 段的结束
    void * b;
    int i;

    if (buffer_end == 1 << 20)
        b = (void *) (640 * 1024);
    else
        b = (void *) buffer_end;                    ///< b = 4M
    while ( (b -= BLOCK_SIZE) >= ((void *) (h+1)) ) ///< 从 4M 往前的每个 1K 都定义为 buffer_head，并进行初始化
    {
        h->b_dev = 0;
        h->b_dirt = 0;
        h->b_count = 0;
        h->b_lock = 0;
        h->b_uptodate = 0;
        h->b_wait = NULL;
        h->b_next = NULL;
        h->b_prev = NULL;
        h->b_data = (char *) b;                     ///< 把内存管理起来
        h->b_prev_free = h-1;                       ///< 双向链表
        h->b_next_free = h+1;
        h++;
        NR_BUFFERS++;
        if (b == (void *) 0x100000)                 ///< 跳过 1M ~ 0xA0000 之间的内存（显存和 bios ROM 内存）
            b = (void *) 0xA0000;
    }
    h--;                ///< h++ 后还回去
    free_list = start_buffer;
    free_list->b_prev_free = h;                     ///< 双向循环链表
    h->b_next_free = free_list;
    for (i = 0; i < NR_HASH; i++)                   ///< NR_HASH = 307
        hash_table[i] = NULL;                       ///< 初始化 hash_table
}