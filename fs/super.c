/*
 *  linux/fs/super.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * super.c contains code to handle the super-block tables.
 */
#include <linux/config.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/system.h>

#include <errno.h>
#include <sys/stat.h>

int sync_dev(int dev);
void wait_for_keypress(void);

/* set_bit uses setb, as gas doesn't recognize setc（gas是一种汇编器）*/
/**
 * @brief 判断 addr 位置的 bitnr 位是否为 1，如果是 1 则返回 true，否则返回 false。
 * @details 取出 addr 位置的第 bitnr 位，存入 al 中，并返回 al 值。
 */
#define set_bit(bitnr, addr) ({ \
register int __res __asm__("ax"); \
__asm__("bt %2,%3; setb %%al":"=a" (__res):"a" (0),"r" (bitnr),"m" (*(addr))); /* 检查addr[bitnr]的值，将值同步到 al 中。*/ \
__res; })

/* 文件系统超级块 */
struct super_block super_block[NR_SUPER];
/* 在 main 里面初始化，得到 ROOT_DEV = 0x306（第二块硬盘第一个可用分区，非/dev/hdb，为/dev/hdb1）*/
int ROOT_DEV = 0;

/* 锁定超级块 */
static void lock_super(struct super_block * sb)
{
	cli();
	while (sb->s_lock)
		sleep_on(&(sb->s_wait));
	sb->s_lock = 1;
	sti();
}

/* 将超级块 lock 标志解除，并唤醒等待在这个超级块上的进程 */
static void free_super(struct super_block * sb)
{
	cli();					///< 关中断
	sb->s_lock = 0;			///< 释放 lock
	wake_up(&(sb->s_wait));	///< 唤醒等待在此超级块的进程
	sti();					///< 开中断
}

/* 等待超级块解锁 */
static void wait_on_super(struct super_block * sb)
{
	cli();
	while (sb->s_lock)
		sleep_on(&(sb->s_wait));
	sti();
}

/**
 * @brief 获取超级块，当超级块不存在时返回空。
 */
struct super_block * get_super(int dev)
{
	struct super_block * s;

	if (!dev)
		return NULL;
	s = 0 + super_block;
	while (s < NR_SUPER + super_block)
	{
		if (s->s_dev == dev)		///< 如果超级块已经存在，则直接返回该超级块
		{
			wait_on_super(s);
			if (s->s_dev == dev)
				return s;
			s = 0 + super_block;
		}
		else
			s++;
	}
	return NULL;
}

void put_super(int dev)
{
	struct super_block * sb;
	struct m_inode * inode;
	int i;

	if (dev == ROOT_DEV) {
		printk("root diskette changed: prepare for armageddon\n\r");
		return;
	}
	if (!(sb = get_super(dev)))
		return;
	if (sb->s_imount) {
		printk("Mounted disk changed - tssk, tssk\n\r");
		return;
	}
	lock_super(sb);
	sb->s_dev = 0;
	for(i=0;i<I_MAP_SLOTS;i++)
		brelse(sb->s_imap[i]);
	for(i=0;i<Z_MAP_SLOTS;i++)
		brelse(sb->s_zmap[i]);
	free_super(sb);
	return;
}

/**
 * @brief 读取磁盘中的 超级块 和 紧随超级块的 inode 位图 以及 数据块位图
 */
static struct super_block * read_super(int dev)		///< dev = 0x306
{
	struct super_block * s;
	struct buffer_head * bh;
	int i,block;

	if (!dev)
		return NULL;
	check_disk_change(dev);
	if (s = get_super(dev))		///< 如果超级块已存在
		return s;

	/// 找一块未用的超级块
	for (s = 0 + super_block ;; s++) 
	{
		if (s >= NR_SUPER + super_block)
			return NULL;
		if (!s->s_dev)
			break;
	}
	/// 对该超级块进行初始化
	s->s_dev = dev;
	s->s_isup = NULL;
	s->s_imount = NULL;
	s->s_time = 0;
	s->s_rd_only = 0;
	s->s_dirt = 0;
	lock_super(s);
	if (!(bh = bread(dev, 1)))		///< 读取 0x306 的第 1 个块，即 超级块。
	{
		s->s_dev=0;
		free_super(s);
		return NULL;
	}
	*((struct d_super_block *) s) = *((struct d_super_block *) bh->b_data);	///< 复制超级块的内容。

	brelse(bh);						///< 释放缓冲区
	if (s->s_magic != SUPER_MAGIC)	///< 超级块的标识不符处理
	{
		s->s_dev = 0;
		free_super(s);
		return NULL;
	}
	for (i = 0; i < I_MAP_SLOTS; i++)
		s->s_imap[i] = NULL;
	for (i=0;i<Z_MAP_SLOTS;i++)
		s->s_zmap[i] = NULL;
	block = 2;						///< linux0.11 中 1 个 block 大小为 1024 字节，那读取 block = 2 就是读取 1024~2047 磁盘的数据。
	
	/* 读取 inode 位图 */
	for (i = 0 ; i < s->s_imap_blocks ; i++)	///< 告诉内核需要读取多少个逻辑块才能把整个 Inode 位图读入内存
		if (s->s_imap[i] = bread(dev, block))
			block++;
		else
			break;
	/* 读取数据块位图 */
	for (i=0 ; i < s->s_zmap_blocks ; i++)
		if (s->s_zmap[i] = bread(dev, block))
			block++;
		else
			break;
	/* 进行两种块的校验 */
	if (block != 2 + s->s_imap_blocks + s->s_zmap_blocks) 
	{
		for(i = 0; i < I_MAP_SLOTS; i++)
			brelse(s->s_imap[i]);
		for(i = 0; i < Z_MAP_SLOTS; i++)
			brelse(s->s_zmap[i]);
		s->s_dev = 0;
		free_super(s);
		return NULL;
	}
	s->s_imap[0]->b_data[0] |= 1;    ///< inode0 已经被占用
	s->s_zmap[0]->b_data[0] |= 1;    ///< 数据块0 已经被占用
	free_super(s);                   ///< 解除lock标志，唤醒等待在此的进程。
	return s;
}

int sys_umount(char * dev_name)
{
	struct m_inode * inode;
	struct super_block * sb;
	int dev;

	if (!(inode=namei(dev_name)))
		return -ENOENT;
	dev = inode->i_zone[0];
	if (!S_ISBLK(inode->i_mode)) {
		iput(inode);
		return -ENOTBLK;
	}
	iput(inode);
	if (dev==ROOT_DEV)
		return -EBUSY;
	if (!(sb=get_super(dev)) || !(sb->s_imount))
		return -ENOENT;
	if (!sb->s_imount->i_mount)
		printk("Mounted inode has i_mount=0\n");
	for (inode=inode_table+0 ; inode<inode_table+NR_INODE ; inode++)
		if (inode->i_dev==dev && inode->i_count)
				return -EBUSY;
	sb->s_imount->i_mount=0;
	iput(sb->s_imount);
	sb->s_imount = NULL;
	iput(sb->s_isup);
	sb->s_isup = NULL;
	put_super(dev);
	sync_dev(dev);
	return 0;
}

int sys_mount(char * dev_name, char * dir_name, int rw_flag)
{
	struct m_inode * dev_i, * dir_i;
	struct super_block * sb;
	int dev;

	if (!(dev_i=namei(dev_name)))
		return -ENOENT;
	dev = dev_i->i_zone[0];
	if (!S_ISBLK(dev_i->i_mode)) {
		iput(dev_i);
		return -EPERM;
	}
	iput(dev_i);
	if (!(dir_i=namei(dir_name)))
		return -ENOENT;
	if (dir_i->i_count != 1 || dir_i->i_num == ROOT_INO) {
		iput(dir_i);
		return -EBUSY;
	}
	if (!S_ISDIR(dir_i->i_mode)) {
		iput(dir_i);
		return -EPERM;
	}
	if (!(sb=read_super(dev))) {
		iput(dir_i);
		return -EBUSY;
	}
	if (sb->s_imount) {
		iput(dir_i);
		return -EBUSY;
	}
	if (dir_i->i_mount) {
		iput(dir_i);
		return -EPERM;
	}
	sb->s_imount=dir_i;
	dir_i->i_mount=1;
	dir_i->i_dirt=1;		/* NOTE! we don't iput(dir_i) */
	return 0;			/* we do that in umount */
}

void mount_root(void)
{
	int i,free;
	struct super_block * p;
	struct m_inode * mi;

	if (32 != sizeof (struct d_inode))
		panic("bad i-node size");
	for(i = 0; i < NR_FILE; i++)
		file_table[i].f_count = 0;		///< 文件引用计数为 0。
	if (MAJOR(ROOT_DEV) == 2)			///< floppy 的处理。
	{
		printk("Insert root floppy and press ENTER");
		wait_for_keypress();
	}
	/// 初始化超级块
	for(p = &super_block[0] ; p < &super_block[NR_SUPER] ; p++) 
	{
		p->s_dev = 0;					///< 超级块对应的设备号
		p->s_lock = 0;					///< 非锁定
		p->s_wait = NULL;				///< 等待该超级块的进程队列
	}
	/// 从磁盘中读取超级块
	if (!(p = read_super(ROOT_DEV)))    ///< ROOT_DEV = 0x306
		panic("Unable to mount root");
	/// 获取 root inode。
	if (!(mi = iget(ROOT_DEV, ROOT_INO)))   ///< ROOT_DEV = 0x306
		panic("Unable to read root i-node");
	mi->i_count += 3 ;					///< 这里总共是 4 个引用（超级块2个，当前进程2个），分配的时候默认有 1 个，所以应该加 3 个。
	p->s_isup = p->s_imount = mi;		///< 超级块增加两个这个 inode 的引用。
	current->pwd = mi;					///< 当前进程的 inode 引用。
	current->root = mi;					///< 当前进程的 inode 引用。
	free = 0;
	i = p->s_nzones;
	/// 统计有多少个空的磁盘块（每个块1KB）。
	while (--i >= 0)
		if (!set_bit(i & 8191, p->s_zmap[i >> 13]->b_data))
			free++;
	printk("%d/%d free blocks\n\r",free,p->s_nzones);
	free=0;
	i=p->s_ninodes+1;
	while (-- i >= 0)
		if (!set_bit(i&8191,p->s_imap[i>>13]->b_data))
			free++;
	printk("%d/%d free inodes\n\r",free,p->s_ninodes);
}
