/*
 *  linux/fs/inode.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <string.h>
#include <sys/stat.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <asm/system.h>

/// 整个系统同时只能有 32 个文件处于“打开”状态（或者说，同时只有 32 个文件的元数据被缓存在内存中）。
struct m_inode inode_table[NR_INODE]={{0,},};

static void read_inode(struct m_inode * inode);
static void write_inode(struct m_inode * inode);

/**
 * @brief 等待 inode 解除锁定。
 * @notes 在读写 inode 时会加锁。
 */
static inline void wait_on_inode(struct m_inode * inode)
{
    cli();
    while (inode->i_lock)
        sleep_on(&inode->i_wait);
    sti();
}

/**
 * @brief 锁定 inode。（在读写时锁定 inode。）
 * @details 先等待 inode 解锁，然后再锁定该 inode。
 */
static inline void lock_inode(struct m_inode * inode)
{
    cli();
    while (inode->i_lock)
        sleep_on(&inode->i_wait);
    inode->i_lock=1;    ///< 这里关中断，linux0.11为单核设计，关中断就不会发生时钟中断导致进程被置换出去的事情，
                        ///< 因此这里能全套执行下来，即只有一个进程能成功对 inode 进行 lock。
    sti();
}

/* 解锁 inode，唤醒等待在此 inode 上的进程 */
static inline void unlock_inode(struct m_inode * inode)
{
    inode->i_lock = 0;
    wake_up(&inode->i_wait);
}

void invalidate_inodes(int dev)
{
    int i;
    struct m_inode * inode;

    inode = 0+inode_table;
    for(i=0 ; i<NR_INODE ; i++,inode++) {
        wait_on_inode(inode);
        if (inode->i_dev == dev) {
            if (inode->i_count)
                printk("inode in use on removed disk\n\r");
            inode->i_dev = inode->i_dirt = 0;
        }
    }
}

/**
 * @brief 同步所有 inode 到磁盘的对应内存缓存。
 */
void sync_inodes(void)
{
    int i;
    struct m_inode * inode;

    inode = 0 + inode_table;
    for(i = 0; i < NR_INODE; i++,inode++) 
    {
        wait_on_inode(inode);                    ///< 等待 inode 解除锁定
        if (inode->i_dirt && !inode->i_pipe)    ///< 管道是个临时内存文件，不需要回写磁盘。
            write_inode(inode);                 ///< 将 inode 写入磁盘的对应内存缓存。
    }
}

static int _bmap(struct m_inode * inode,int block,int create)
{
    struct buffer_head * bh;
    int i;

    if (block<0)
        panic("_bmap: block<0");
    if (block >= 7+512+512*512)
        panic("_bmap: block>big");
    if (block<7) {
        if (create && !inode->i_zone[block])
            if (inode->i_zone[block]=new_block(inode->i_dev)) {
                inode->i_ctime=CURRENT_TIME;
                inode->i_dirt=1;
            }
        return inode->i_zone[block];
    }
    block -= 7;
    if (block<512) {
        if (create && !inode->i_zone[7])
            if (inode->i_zone[7]=new_block(inode->i_dev)) {
                inode->i_dirt=1;
                inode->i_ctime=CURRENT_TIME;
            }
        if (!inode->i_zone[7])
            return 0;
        if (!(bh = bread(inode->i_dev,inode->i_zone[7])))
            return 0;
        i = ((unsigned short *) (bh->b_data))[block];
        if (create && !i)
            if (i=new_block(inode->i_dev)) {
                ((unsigned short *) (bh->b_data))[block]=i;
                bh->b_dirt=1;
            }
        brelse(bh);
        return i;
    }
    block -= 512;
    if (create && !inode->i_zone[8])
        if (inode->i_zone[8]=new_block(inode->i_dev)) {
            inode->i_dirt=1;
            inode->i_ctime=CURRENT_TIME;
        }
    if (!inode->i_zone[8])
        return 0;
    if (!(bh=bread(inode->i_dev,inode->i_zone[8])))
        return 0;
    i = ((unsigned short *)bh->b_data)[block>>9];
    if (create && !i)
        if (i=new_block(inode->i_dev)) {
            ((unsigned short *) (bh->b_data))[block>>9]=i;
            bh->b_dirt=1;
        }
    brelse(bh);
    if (!i)
        return 0;
    if (!(bh=bread(inode->i_dev,i)))
        return 0;
    i = ((unsigned short *)bh->b_data)[block&511];
    if (create && !i)
        if (i=new_block(inode->i_dev)) {
            ((unsigned short *) (bh->b_data))[block&511]=i;
            bh->b_dirt=1;
        }
    brelse(bh);
    return i;
}

int bmap(struct m_inode * inode,int block)
{
    return _bmap(inode,block,0);
}

int create_block(struct m_inode * inode, int block)
{
    return _bmap(inode,block,1);
}

/**
 * @brief inode put，即 inode 归还（减少 inode->i_count 计数），如果计数为 0，则将 inode 归还 inode_table，其他进程可以复用该 inode 了。
 * @details 如果是管道文件（管道只在内存中存在，不在磁盘中），则释放管道文件；如果是块设备文件，则同步当前块设备的所有数据到磁盘。
 */
void iput(struct m_inode * inode)
{
    if (!inode)
        return;
    wait_on_inode(inode);   ///< 等待 inode 解除锁定
    if (!inode->i_count)    ///< 引用为 0，已释放。
        panic("iput: trying to free free inode");

    /// 如果文件为管道，判断是否为最后一个管道引用，如果是，则释放管道所占的物理页面。
    if (inode->i_pipe)
    {
        wake_up(&inode->i_wait);
        if (--inode->i_count)       ///< 如果不是最后一个引用，则返回；如果是，则释放页面。
            return;
        free_page(inode->i_size);   ///< 释放管道所占物理页面。
        inode->i_count = 0;
        inode->i_dirt = 0;
        inode->i_pipe = 0;
        return;
    }

    /// 防御性检查，什么情况下 inode->i_dev==0 ？有可能是块刚被分配，尚未初始化。
    if (!inode->i_dev)
    {
        inode->i_count--;           ///< 释放引用。
        return;
    }

    /// 块设备文件处理（块设备文件，硬盘、软盘、光盘等可以随机访问的存储设备），这是一块块设备（如/dev/hda1）而不是普通的文件或目录，则同步当前块设备的所有内存磁盘和inode。
    if (S_ISBLK(inode->i_mode))
    {
        sync_dev(inode->i_zone[0]); ///< 同步块设备文件。这里i_zone[0]不再指向数据块指针，而是设备号。
        wait_on_inode(inode);       ///< 等待 inode 解除锁定（在读写 inode 时会加锁）。
    }
repeat:
    /// 这里是如果还有引用，直接减少就行，如果没有引用了，那么需要释放文件。
    if (inode->i_count > 1)         ///< 减少引用
    {
        inode->i_count--;
        return;
    }
    /// 如果硬链接为 0，则释放该 inode 占用的内存。
    if (!inode->i_nlinks)
    {
        truncate(inode);
        free_inode(inode);
        return;
    }
    /// 走到这里硬链接不为 0，同步 inode 到对应的内存磁盘。
    if (inode->i_dirt)          ///< 如果为脏
    {
        write_inode(inode);     ///< 写 inode 到内存磁盘。
        wait_on_inode(inode);
        goto repeat;
    }
    inode->i_count--;           ///< 减少自身引用，即释放 inode，其他进程可以复用这个 inode 了。
    return;
}

/**
 * @brief 获取空闲 inode
 * @details 从 inode 列表 inode_table 中寻找无人使用的 inode，即寻找 inode->i_count = 0
 * 如果是脏的，就同步到磁盘的内存缓存 buffer_head，并且重置这个 inode，将引用计数 inode->i_count = 1
 */
struct m_inode * get_empty_inode(void)
{
    struct m_inode * inode;
    static struct m_inode * last_inode = inode_table;
    int i;

    do {
        inode = NULL;
        /// 从 inode 表中寻找空闲 inode
        for (i = NR_INODE; i ; i--) 
        {
            if (++last_inode >= inode_table + NR_INODE)
                last_inode = inode_table;
            if (!last_inode->i_count)
            {
                inode = last_inode;
                if (!inode->i_dirt && !inode->i_lock)
                    break;
            }
        }
        if (!inode)        ///< 找不到空闲 inode 时打印提示信息
        {
            for (i=0 ; i < NR_INODE ; i++)
                printk("%04x: %6d\t",inode_table[i].i_dev, inode_table[i].i_num);
            panic("No free inodes in mem");
        }
        /* 
        走到这里分两种情况：
            1.找到空闲的 inode（i_count=0），非脏且非锁定。
            2.找到空闲的 inode（i_count=0），找到的 inode 可能是 dirty 和 lock 的，这时候就需要同步 inode 到磁盘。
        */
        wait_on_inode(inode);    ///< 等待 inode 解锁。
        while (inode->i_dirt)    ///< 同步 inode。
        {
            write_inode(inode);  ///< 同步到磁盘的内存缓存 buffer_head 中。
            wait_on_inode(inode);///< 等待 inode 解除锁
        }
    } while (inode->i_count);
    memset(inode, 0, sizeof(*inode));
    inode->i_count = 1;
    return inode;
}

struct m_inode * get_pipe_inode(void)
{
    struct m_inode * inode;

    if (!(inode = get_empty_inode()))
        return NULL;
    if (!(inode->i_size=get_free_page())) {
        inode->i_count = 0;
        return NULL;
    }
    inode->i_count = 2;    /* sum of readers/writers */
    PIPE_HEAD(*inode) = PIPE_TAIL(*inode) = 0;
    inode->i_pipe = 1;
    return inode;
}

/**
 * @brief 读取 inode。
 * @param dev 设备号。
 * @param nr inode号。
 * @details 先从 inode_table 中查看该 inode 是否已经在 inode_table 中了，如果不在，就获取一个空闲的 inode，并将该 inode 从磁盘中读取出来。
 * 特殊情况，如果存在挂载点如 /mnt 挂载了一个 U 盘，则跳入新文件系统中，读取该文件系统的根目录。
 */
struct m_inode * iget(int dev, int nr)    ///< dev = 0x306, nr = 1，inode 号 1 固定是根目录。
{
    struct m_inode * inode, * empty;

    if (!dev)
        panic("iget with dev==0");
    empty = get_empty_inode();            ///< 获取一个空闲 inode 存入 empty。
    inode = inode_table;
    while (inode < NR_INODE + inode_table)
    {
        if (inode->i_dev != dev || inode->i_num != nr) 
        {
            inode++;
            continue;
        }
        wait_on_inode(inode);    ///< 等待 inode 解除锁定，如果锁定则进入睡眠，
                                 ///< 等待 inode 使用者来唤醒自己，这个时候就是当前进程独占这个 inode，其他进程还睡眠在这个 inode 上。

        /// 为什么要再次检查？因为在调用 wait_on_inode 等待它解锁的这段时间里，另一个进程可能已经修改了它。
        /// 这里确保拿到的是这个 inode 无误。
        if (inode->i_dev != dev || inode->i_num != nr)
        {
            inode = inode_table;
            continue;
        }
        inode->i_count++;        ///< 增加引用计数。

        /// 这个目录是一个文件系统挂载点，如 /mnt 挂载了一块 U 盘。
        if (inode->i_mount)
        {
            int i;

            /// 找到该挂载的超级块
            for (i = 0 ; i < NR_SUPER ; i++)
                if (super_block[i].s_imount == inode)
                    break;
            if (i >= NR_SUPER)  ///< 如果没找到对应的超级块
            {
                printk("Mounted inode hasn't got sb\n");
                if (empty)
                    iput(empty);
                return inode;
            }
            iput(inode);        ///< 释放 inode。
            /// 既然是挂载点，就需要访问被挂载的文件系统。
            /// 更新 dev 和 nr，跳入新文件系统。
            dev = super_block[i].s_dev;
            nr = ROOT_INO;
            inode = inode_table;
            continue;
        }
        
        /// 已经在 inode_table 中找到了 inode，就需要把之前申请的空闲 inode 释放。
        if (empty)
            iput(empty);
        return inode;
    }

    /// 走到这里就说明上面根据 dev 和 nr 没有找到 inode。
    if (!empty)                 ///< 既没找到空闲 inode 也没在 inode_table 中找到目标 inode，直接返回 NULL。
        return (NULL);
    inode = empty;
    inode->i_dev = dev;
    inode->i_num = nr;
    read_inode(inode);          ///< 从磁盘中读取 inode。
    return inode;
}

/**
 * @brief 从磁盘中读取 inode。
 * @details 计算该 inode 在磁盘中的位置（在第几个 block 上），然后读取出 buffer_head，从 buffer_head 中读取出 inode。
 */
static void read_inode(struct m_inode * inode)
{
    struct super_block * sb;
    struct buffer_head * bh;
    int block;

    lock_inode(inode);          ///< 锁定该 inode。
    if (!(sb = get_super(inode->i_dev)))            ///< 获取超级块。
        panic("trying to read inode without dev");
    block = 2 + sb->s_imap_blocks + sb->s_zmap_blocks + (inode->i_num - 1) / INODES_PER_BLOCK;  ///< 计算所在逻辑块号
    if (!(bh = bread(inode->i_dev, block)))         ///< 读取磁盘块到内存
        panic("unable to read i-node block");
    *(struct d_inode *)inode = ((struct d_inode *)bh->b_data)[(inode->i_num - 1) % INODES_PER_BLOCK];
    brelse(bh);                 ///< 释放 buffer_head。
    unlock_inode(inode);        ///< 释放 inode。
}

/**
 * @brief 将 inode 写入磁盘的对应内存缓存
 * @details 首先要求 inode 为脏且设备存在，然后计算 inode 在磁盘中的位置获取对应的 buffer_head，
 * 将 buffer_head 标记为脏，由下次 getblk 触发磁盘同步，同步整个 buffer_head。
 */
static void write_inode(struct m_inode * inode)
{
    struct super_block * sb;
    struct buffer_head * bh;
    int block;

    lock_inode(inode);    ///< 加锁
    if (!inode->i_dirt || !inode->i_dev)    ///< 如果不为脏 或 设备不存在，则直接返回。
    {
        unlock_inode(inode);
        return;
    }
    if (!(sb = get_super(inode->i_dev)))    ///< 只有当设备被成功挂载后，内核的 super_block 数组中才会存在一个对应的条目
        panic("trying to write inode without device");
    /// 计算当前 inode 所在磁盘位置，每个 block 占用 1K。
    block = 2 + sb->s_imap_blocks + sb->s_zmap_blocks + (inode->i_num - 1) / INODES_PER_BLOCK;

    /// 一个磁盘块中存储了很多 inode，所以需要先将 inode 读取出来再将对应的 inode 更新。
    if (!(bh = bread(inode->i_dev, block)))
        panic("unable to read i-node block");

    /// 同步 inode 到磁盘的内存映射 buffer_head 中。
    ((struct d_inode *)bh->b_data)[(inode->i_num - 1) % INODES_PER_BLOCK] = *(struct d_inode *)inode;  ///< 转换为磁盘 inode 然后写入。
    bh->b_dirt = 1;     ///< 标记为脏，下次再分配即调用 getblk 时会同步磁盘。
    inode->i_dirt = 0;
    brelse(bh);
    unlock_inode(inode);
}
