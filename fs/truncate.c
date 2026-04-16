#include <linux/sched.h>
#include <sys/stat.h>

/**
 * @brief 释放 1 级块。
 * @details 1 级块中存储了512个直接块（每个块占1K），先释放这 512 个直接块，然后释放这个 1 级块本身。
 */
static void free_ind(int dev, int block)
{
    struct buffer_head * bh;
    unsigned short * p;
    int i;

    if (!block)
        return;
    if (bh = bread(dev, block))     ///< 读取 1 级块（1 级块里面存储的是直接块的块号，块号为 unsigned short，占用 2 字节）。
    {
        p = (unsigned short *) bh->b_data;
        for (i = 0; i < 512; i++, p++)
            if (*p)                 ///< 块号不为 0 则释放块号。
                free_block(dev, *p);
        brelse(bh);                 ///< 释放 buffer_head。
    }
    free_block(dev, block);         ///< 释放 1 级块自身。
}

/**
 * @brief 释放二级磁盘块。
 * @details 一个二级块存储了 512 个一级块块号（一个块号占 unsigned short），释放所有一级块，然后释放二级块本身。
 */
static void free_dind(int dev, int block)
{
    struct buffer_head * bh;
    unsigned short * p;
    int i;

    if (!block)
        return;
    if (bh = bread(dev, block))     ///< 读取 2 级块到内存。
    {
        p = (unsigned short *) bh->b_data;
        for (i = 0; i < 512; i++, p++)  ///< 释放所有 1 级块。
            if (*p)
                free_ind(dev, *p);
        brelse(bh);                 ///< 释放 buffer_head
    }
    free_block(dev, block);         ///< 释放 2 级块本身。
}

/**
 * @brief 截断文件
 * @details 只处理目录和普通文件，直接释放直接数据块，释放一级间接块、二级间接块（递归释放每个数据块）。
 */
void truncate(struct m_inode * inode)
{
    int i;

    if (!(S_ISREG(inode->i_mode) || S_ISDIR(inode->i_mode)))    ///< 如果不是普通文件也不是目录，直接返回。
        return;

    /// 如果是普通文件或目录
    for (i = 0; i < 7; i++)     ///< 前 7 个为直接数据块
        if (inode->i_zone[i]) 
        {
            free_block(inode->i_dev, inode->i_zone[i]);         ///< 释放块（1KB）。
            inode->i_zone[i] = 0;                               ///< 清零指针。
        }
    free_ind(inode->i_dev, inode->i_zone[7]);                   ///< 释放一级间接块。
    free_dind(inode->i_dev,inode->i_zone[8]);                   ///< 释放二级间接块。
    inode->i_zone[7] = inode->i_zone[8] = 0;                    ///< 清零指针。
    inode->i_size = 0;          ///< 文件大小为 0。
    inode->i_dirt = 1;          ///< 标记为该 inode 需要同步至磁盘。
    inode->i_mtime = inode->i_ctime = CURRENT_TIME;             ///< 更新状态改变时间和文件内容修改时间。
}