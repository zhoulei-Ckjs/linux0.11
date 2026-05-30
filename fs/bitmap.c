/*
 *  linux/fs/bitmap.c
 *
 *  (C) 1991  Linus Torvalds
 */

/* bitmap.c contains the code that handles the inode and block bitmaps */
#include <string.h>

#include <linux/sched.h>
#include <linux/kernel.h>

#define clear_block(addr) \
__asm__("cld\n\t" \
    "rep\n\t" \
    "stosl" \
    ::"a" (0),"c" (BLOCK_SIZE/4),"D" ((long) (addr)):"cx","di")

#define set_bit(nr,addr) ({\
register int res __asm__("ax"); \
__asm__ __volatile__("btsl %2,%3\n\tsetb %%al": \
"=a" (res):"0" (0),"r" (nr),"m" (*(addr))); \
res;})

/**
 * @brief 在释放磁盘块之前，检查该块是否已经被标记为“空闲”。
 * @details 清除 addr 的 nr 位。如果 nr 位为 1，返回 0；如果 nr 位为 0，返回 1。
 */
#define clear_bit(nr,addr) ({\
register int res __asm__("ax"); /* 请把变量 res 这个 C 语言符号，永远映射到 EAX 寄存器上。 */ \
__asm__ __volatile__("btrl %2,%3\n\tsetnb %%al": /* 测试位并复位（清零）,2% 要清除的位编号，3% 目标内存地址。*/ \
"=a" (res):"0" (0),"r" (nr),"m" (*(addr))); \
res;})

/**
 * @brief 在 addr 位置到 addr+1KB 位置之间找到第一个 bit 位为 0，将这个位索引位置返回。
 */
#define find_first_zero(addr) ({ \
int __res; \
__asm__("cld\n" \               /* 清零方向标志位 */
    "1:\tlodsl\n\t" \           /* 从 DS:ESI 处（即 addr 位置）加载 4 字节到 eax。*/
    "notl %%eax\n\t" \          /* eax 寄存器二进制取反。*/
    "bsfl %%eax,%%edx\n\t" \    /* 从 eax 中从低到高找第一个 1，索引存入 edx。如果 eax 为 0，则设置 ZF = 1。*/
    "je 2f\n\t" \               /* ZF == 1 则跳转 2f */
    "addl %%edx,%%ecx\n\t" \    /* ecx = edx + ecx */
    "jmp 3f\n" \
    "2:\taddl $32,%%ecx\n\t" \  /* ecx += 32 */
    "cmpl $8192,%%ecx\n\t" \    /* 若 ecx == 8192，ZF = 1 */
    "jl 1b\n" \                 /* 若 ecx < 8192，则跳转到 1b */
    "3:" \
    :"=c" (__res):"c" (0),"S" (addr):"ax","dx","si"); \
__res;})

/**
 * @brief 释放块（1KB），指明这块磁盘可以用了，一般为释放文件时要释放文件的所有块。
 * @details 1.释放对应的磁盘内存映射 buffer_head。（都走到释放这一步了，那这个 buffer_head 的进程引用应该只有 1 个，就是当前进程。）
 *          2.清除对应的磁盘数据块标记位图。
 * @param dev：主设备号，设备号 0x306 的主设备号为 3。
 * @param block：块号。
 */
void free_block(int dev, int block)
{
    struct super_block * sb;
    struct buffer_head * bh;

    if (!(sb = get_super(dev))) ///< 如果分区不存在。
        panic("trying to free block on nonexistent device");
    if (block < sb->s_firstdatazone || block >= sb->s_nzones)
        panic("trying to free block not in datazone");
    bh = get_hash_table(dev, block);
    if (bh) 
    {
        if (bh->b_count != 1)   ///< 要释放的磁盘内存映射的进程引用应该只有 1 个。
        {
            printk("trying to free block (%04x:%d), count=%d\n", dev, block, bh->b_count);
            return;
        }
        bh->b_dirt = 0;
        bh->b_uptodate = 0;
        brelse(bh);             ///< 释放该磁盘内存映射。
    }
    block -= sb->s_firstdatazone - 1;   ///< 计算出对应的磁盘 bitmap 位置。

    /// 在释放磁盘块之前，检查该块是否已经被标记为“空闲”，如果已经空闲，说明出现了严重的逻辑错误。
    if (clear_bit(block & 8191, sb->s_zmap[block / 8192]->b_data))      ///< 清除位图。
    {
        printk("block (%04x:%d) ",dev,block+sb->s_firstdatazone-1);
        panic("free_block: bit already cleared");
    }
    sb->s_zmap[block/8192]->b_dirt = 1; ///< 标记 s_zmap[block/8192] 对应的 buffer_head 为脏，表明需要同步。
}

int new_block(int dev)
{
    struct buffer_head * bh;
    struct super_block * sb;
    int i,j;

    if (!(sb = get_super(dev)))
        panic("trying to get new block from nonexistant device");
    j = 8192;
    for (i = 0; i < 8; i++)
        if (bh = sb->s_zmap[i])
            if ((j = find_first_zero(bh->b_data)) < 8192)   ///< 根据磁盘标志位，找到第一个未被占用的磁盘块（1KB）。
                break;
    if (i>=8 || !bh || j>=8192)
        return 0;
    if (set_bit(j,bh->b_data))
        panic("new_block: bit already set");
    bh->b_dirt = 1;
    j += i*8192 + sb->s_firstdatazone-1;
    if (j >= sb->s_nzones)
        return 0;
    if (!(bh=getblk(dev,j)))
        panic("new_block: cannot get block");
    if (bh->b_count != 1)
        panic("new block: count is != 1");
    clear_block(bh->b_data);
    bh->b_uptodate = 1;
    bh->b_dirt = 1;
    brelse(bh);
    return j;
}

void free_inode(struct m_inode * inode)
{
    struct super_block * sb;
    struct buffer_head * bh;

    if (!inode)
        return;
    if (!inode->i_dev) {
        memset(inode,0,sizeof(*inode));
        return;
    }
    if (inode->i_count>1) {
        printk("trying to free inode with count=%d\n",inode->i_count);
        panic("free_inode");
    }
    if (inode->i_nlinks)
        panic("trying to free inode with links");
    if (!(sb = get_super(inode->i_dev)))
        panic("trying to free inode on nonexistent device");
    if (inode->i_num < 1 || inode->i_num > sb->s_ninodes)
        panic("trying to free inode 0 or nonexistant inode");
    if (!(bh=sb->s_imap[inode->i_num>>13]))
        panic("nonexistent imap in superblock");
    if (clear_bit(inode->i_num&8191,bh->b_data))
        printk("free_inode: bit already cleared.\n\r");
    bh->b_dirt = 1;
    memset(inode,0,sizeof(*inode));
}

struct m_inode * new_inode(int dev)
{
    struct m_inode * inode;
    struct super_block * sb;
    struct buffer_head * bh;
    int i,j;

    if (!(inode=get_empty_inode()))
        return NULL;
    if (!(sb = get_super(dev)))
        panic("new_inode with unknown device");
    j = 8192;
    for (i=0 ; i<8 ; i++)
        if (bh=sb->s_imap[i])
            if ((j=find_first_zero(bh->b_data))<8192)
                break;
    if (!bh || j >= 8192 || j+i*8192 > sb->s_ninodes) {
        iput(inode);
        return NULL;
    }
    if (set_bit(j,bh->b_data))
        panic("new_inode: bit already set");
    bh->b_dirt = 1;
    inode->i_count=1;
    inode->i_nlinks=1;
    inode->i_dev=dev;
    inode->i_uid=current->euid;
    inode->i_gid=current->egid;
    inode->i_dirt=1;
    inode->i_num = j + i*8192;
    inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;
    return inode;
}
