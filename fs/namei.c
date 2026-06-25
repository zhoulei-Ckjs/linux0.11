#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/segment.h>

#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <const.h>
#include <sys/stat.h>

#define ACC_MODE(x) ("\004\002\006\377"[(x)&O_ACCMODE])

/*
 * comment out this line if you want names > NAME_LEN chars to be
 * truncated. Else they will be disallowed.
 */
/* #define NO_TRUNCATE */

#define MAY_EXEC 1      /* 对于普通文件时可执行权限，目录没有“执行”的概念，它的 x 权限实际代表 search（搜索/遍历）权限，
    有 x 权限：可以 cd 进入该目录，或访问该目录下的子文件/子目录（即使没有 r 权限）
    无 x 权限：无法进入该目录，也无法通过该目录访问任何下级路径（即使知道完整路径）*/
#define MAY_WRITE 2
#define MAY_READ 4

/**
 * @brief 权限检查，检查当前进程是否有权限
 * @retval 0 校验不通过
 * @retval 1 校验通过
 */
static int permission(struct m_inode * inode, int mask)
{
    int mode = inode->i_mode;

    if (inode->i_dev && !inode->i_nlinks)       ///< 如果文件在真实设备上（i_dev!=0）且硬链接数为 0（!i_nlinks），说明文件已被删除（unlinked）。此时连 root 也无法访问，直接拒绝。
        return 0;
    else if (current->euid == inode->i_uid)     ///< 如果当前进程有效用户 id 与文件所有者 id 一致，则提取所有者 id。
        mode >>= 6;
    else if (current->egid == inode->i_gid)     ///< 若当前进程有效 GID 等于文件所属组，右移 3 位，把组权限位（bit 5~3）移到最低 3 位。
        mode >>= 3;
    if (((mode & mask & 0007) == mask) || suser())  ///< 请求的权限在范围内 或 是root用户，则校验通过。
        return 1;
    return 0;       ///< 校验不通过
}

/**
 * @brief 比较字符串name和dir_entry->name是否相等
 * @retval 1: 字符串相等
 * @retval 0: 不相等
 * @note name为指向用户空间字符串，strncmp() 会按 DS 段直接访问内存，而用户字符串应该通过 FS 段访问，因此不能直接用 strncmp()，必须使用 match()，由 match() 利用 FS 段逐字节读取用户空间字符串进行比较。
 */
static int match(int len, const char * name, struct dir_entry * de)
{
    register int same __asm__("ax");            ///< same 绑定到 EAX

    if (!de || !de->inode || len > NAME_LEN)
        return 0;
    if (len < NAME_LEN && de->name[len])
        return 0;
    __asm__("cld\n\t"
        "fs ; repe ; cmpsb\n\t"     /* 重复执行ecx次比较fs:esi vs es:edi，如果不同，则设置ZF=0，结束循环；如果相等，则设置ZF=1，继续向下比较（esi++,edi++）。*/
        "setz %%al"
        :"=a" (same)
        :"0" (0),"S" ((long) name),"D" ((long) de->name),"c" (len)
        :"cx","di","si");
    return same;
}

/**
 * @brief 找到当前目录 dir 下的子目录或文件
 * @param dir 当前目录
 * @param name 为当前目录下的文件相对路径，如 name = dev/tty0，namelen = 3，则寻找当前目录下的 dev 目录。
 * @param res_dir 输出参数，找到的目录或文件
 * @return 返回包含当前子目录的页面 buffer_head
 */
static struct buffer_head * find_entry(struct m_inode ** dir, const char * name, int namelen, struct dir_entry ** res_dir) 
{
    int entries;        ///< 用于辅助统计当前目录有多少目录项，即有多少文件
    int block,i;
    struct buffer_head * bh;
    struct dir_entry * de;
    struct super_block * sb;

#ifdef NO_TRUNCATE      /* 内核编译期配置宏，用于控制文件系统对超长文件名的处理策略，如果是 NO_TRUNCATE 则对于超长文件名不处理，直接返回。*/
    if (namelen > NAME_LEN)
        return NULL;
#else
    if (namelen > NAME_LEN)     /* 对超长文件名进行截断。*/
        namelen = NAME_LEN;
#endif
    entries = (*dir)->i_size / (sizeof (struct dir_entry));     ///< 计算当前目录有多少目录项
    *res_dir = NULL;
    if (!namelen)
        return NULL;
    
    /// 处理 ../
    if (namelen == 2 && get_fs_byte(name) == '.' && get_fs_byte(name + 1) == '.') 
    {
        if ((*dir) == current->root)    ///< 如果当前目录是根目录
            namelen = 1;
        else if ((*dir)->i_num == ROOT_INO)     ///< 当前目录是一个挂载点
        {
            sb = get_super((*dir)->i_dev);      ///< 获取该设备对应的超级块
            if (sb->s_imount)                   ///< 被挂载目录的 inode
            {
                iput(*dir);                     ///< 释放当前 inode（被挂载文件系统的根目录）的引用计数。
                (*dir)=sb->s_imount;            ///< 将当前目录指针切换回原文件系统的挂载点 inode。
                (*dir)->i_count++;              ///< 增加新 inode 的引用计数，防止被提前释放。
            }
        }
    }
    if (!(block = (*dir)->i_zone[0]))           ///< 获取第一个直接数据块的物理块号
        return NULL;
    if (!(bh = bread((*dir)->i_dev, block)))    ///< 将数据块读取至内存。
        return NULL;
    i = 0;
    de = (struct dir_entry *) bh->b_data;       ///< 用 de 去遍历 bh->b_data

    /// 在当前目录下寻找子文件夹的目录项，如 name = dev/tty0，则寻找 dev 目录项。
    while (i < entries)
    {
        if ((char *)de >= BLOCK_SIZE + bh->b_data)  ///< 如果超出了数据块的容量。
        {
            brelse(bh);                             ///< 释放数据块
            bh = NULL;
            if (!(block = bmap(*dir, i/DIR_ENTRIES_PER_BLOCK)) || !(bh = bread((*dir)->i_dev, block)))  ///< 目录块不存在或读取磁盘出错
            {
                i += DIR_ENTRIES_PER_BLOCK;         ///< 跳过，进入下一块
                continue;
            }
            de = (struct dir_entry *) bh->b_data;   ///< 更新遍历指针
        }
        if (match(namelen, name, de))
        {
            *res_dir = de;                          ///< 找到了目录项
            return bh;                              ///< 返回包含当前子目录的页面
        }
        de++;
        i++;
    }
    brelse(bh);
    return NULL;
}

/**
 * @brief 在 dir 目录中添加一个目录项。
 * @details 目录项的 inode 仍然为 0，add_entry 不负责填写 inode，只负责填写目录项名。
 * @example dir = tty0 所属文件夹 dev 的 inode; basename = tty0; namelen = 4，即在 dev 文件夹加一个目录项 tty0。
 * @param dir 文件夹
 * @param res_dir 找到的空白目录项
 * @return 待添加的目录项所在的内存页（内存页对应磁盘块）
 */
static struct buffer_head * add_entry(struct m_inode * dir, const char * name, int namelen, struct dir_entry ** res_dir) 
{
    int block, i;
    struct buffer_head * bh;    ///< 存储将要添加的目录项所在的内存页
    struct dir_entry * de;

    *res_dir = NULL;
#ifdef NO_TRUNCATE
    if (namelen > NAME_LEN)
        return NULL;
#else
    if (namelen > NAME_LEN)
        namelen = NAME_LEN;
#endif
    if (!namelen)
        return NULL;
    if (!(block = dir->i_zone[0]))
        return NULL;                                ///< 空文件/目录
    if (!(bh = bread(dir->i_dev, block)))           ///< 数据块读取失败
        return NULL;
    i = 0;
    de = (struct dir_entry *) bh->b_data;
    while (1) 
    {
        if ((char *)de >= BLOCK_SIZE + bh->b_data)      ///< 如果超出了数据块容量
        {
            brelse(bh);
            bh = NULL;
            block = create_block(dir, i/DIR_ENTRIES_PER_BLOCK);     ///< create_block 对多个行为合一，如果页 i/DIR_ENTRIES_PER_BLOCK 对应的 block 不存在，则创建；如果存在，则直接返回这个 block。
            if (!block)
                return NULL;
            if (!(bh = bread(dir->i_dev, block)))       ///< 数据块无法读取，可能是坏道、I/O错误、buffer_cache没有buffer、设备不存在。
            {
                i += DIR_ENTRIES_PER_BLOCK;
                continue;
            }
            de = (struct dir_entry *) bh->b_data;
        }
        if (i * sizeof(struct dir_entry) >= dir->i_size)    ///< 如果超出了目录项个数，则增加一个目录项
        {
            de->inode = 0;
            dir->i_size = (i + 1) * sizeof(struct dir_entry);
            dir->i_dirt = 1;
            dir->i_ctime = CURRENT_TIME;
        }

        /// 找到了空目录项
        if (!de->inode)
        {
            dir->i_mtime = CURRENT_TIME;
            /// 存储目录项对应的文件名
            for (i = 0; i < NAME_LEN ; i++)
                de->name[i] = (i < namelen) ? get_fs_byte(name + i) : 0;
            bh->b_dirt = 1;
            *res_dir = de;      ///< 返回的文件夹
            return bh;          ///< 内有该目录项的buffer_head。
        }
        de++;
        i++;
    }

    /// 走到这里就是异常情况
    brelse(bh);
    return NULL;
}

/**
 * @brief 获取文件的所属文件夹的 inode。
 * @param pathname 文件名，如/dev/tty0
 * @return 文件夹 inode。
 * @retval NULL: 出错了
 */
static struct m_inode * get_dir(const char * pathname)  ///< pathname = /dev/tty0
{
    char c;
    const char * thisname;
    struct m_inode * inode;
    struct buffer_head * bh;
    int namelen, inr, idev;
    struct dir_entry * de;

    if (!current->root || !current->root->i_count)      ///< 系统根目录
        panic("No root inode");
    if (!current->pwd || !current->pwd->i_count)        ///< 进程当前目录
        panic("No cwd inode");
    if ((c = get_fs_byte(pathname)) == '/')             ///< 如果是/开头的，则为根目录开始
    {
        inode = current->root;
        pathname++;                                     ///< pathname = dev/tty0
    }
    else if (c)
        inode = current->pwd;                           ///< 当前工作目录
    else
        return NULL;    /* empty name is bad */         ///< 空目录

    inode->i_count++;
    while (1) 
    {
        thisname = pathname;
        if (!S_ISDIR(inode->i_mode) || !permission(inode, MAY_EXEC))    ///< 如果不是目录 或者 不具备可执行或可搜索权限，目录没有“执行”的概念，它的 x 权限实际代表 search（搜索/遍历）权限
            ///< 有 x 权限：可以 cd 进入该目录，或访问该目录下的子文件/子目录（即使没有 r 权限）
        {
            iput(inode);    /* 释放 inode */
            return NULL;
        }

        /// 走到这里代表满足 pathname 是目录且具备可搜索权限。

        for(namelen = 0; (c = get_fs_byte(pathname++)) && (c != '/'); namelen++)    ///< 如 dev/tty0，跳过 dev，此时 pathname = tty0，namelen = 3
            /* nothing */ ;
        if (!c)     ///< 如果到达文件末尾，返回找到的inode（当找到文件名时，如 tty0，由上面的 for 循环可知，c 为遍历的最后一位即 \0，此时inode即为文件夹 dev 的 inode）。
            return inode;
        if (!(bh = find_entry(&inode, thisname, namelen, &de)))         ///< inode 为 root 或者当前工作目录，thisname = dev/tty0, namelen = 3，此时为寻找 dev 文件夹的 inode
        {
            iput(inode);
            return NULL;
        }
        inr = de->inode;
        idev = inode->i_dev;
        brelse(bh);
        iput(inode);
        if (!(inode = iget(idev, inr)))                 ///< 递归寻找 inode，这里是获取子目录的 inode。
            return NULL;
    }
}

/**
 * @brief 获取文件 inode、文件名、文件长度
 * @param namelen 文件名长度（去掉文件夹路径后的文件名长度）
 * @param name 文件名（不含文件路径）
 * @return 目标文件的 inode
 */
static struct m_inode * dir_namei(const char * pathname, int * namelen, const char ** name)     ///< pathname = /dev/tty0
{
    char c;
    const char * basename;
    struct m_inode * dir;

    if (!(dir = get_dir(pathname)))                     ///< 获取文件 inode。
        return NULL;
    basename = pathname;
    while (c = get_fs_byte(pathname++))
        if (c == '/')
            basename = pathname;                        ///< 若pathname = /dev/tty0，则 basename = tty0
    *namelen = pathname - basename - 1;                 ///< 计算文件名长度（去掉文件夹路径）
    *name = basename;
    return dir;
}

/*
 *    namei()
 *
 * is used by most simple commands to get the inode of a specified name.
 * Open, link etc use their own routines, but this is enough for things
 * like 'chmod' etc.
 */
struct m_inode * namei(const char * pathname)
{
    const char * basename;
    int inr,dev,namelen;
    struct m_inode * dir;
    struct buffer_head * bh;
    struct dir_entry * de;

    if (!(dir = dir_namei(pathname,&namelen,&basename)))
        return NULL;
    if (!namelen)            /* special case: '/usr/' etc */
        return dir;
    bh = find_entry(&dir,basename,namelen,&de);
    if (!bh) {
        iput(dir);
        return NULL;
    }
    inr = de->inode;
    dev = dir->i_dev;
    brelse(bh);
    iput(dir);
    dir=iget(dev,inr);
    if (dir) {
        dir->i_atime=CURRENT_TIME;
        dir->i_dirt=1;
    }
    return dir;
}

/// @param res_inode 输出参数，待打开文件的 inode，
/// @retval 0: 成功
int open_namei(const char * pathname, int flag, int mode, struct m_inode ** res_inode)      ///< pathname = /dev/tty0
{
    const char * basename;
    int inr, dev, namelen;
    struct m_inode * dir, *inode;
    struct buffer_head * bh;
    struct dir_entry * de;

    if ((flag & O_TRUNC) && !(flag & O_ACCMODE))    ///< 如果用户要求截断文件但是又没有读写权限，那么系统默认会给它加个读写权限。
        flag |= O_WRONLY;

    mode &= 0777 & ~current->umask;                 ///< 去掉权限屏蔽字
    mode |= I_REGULAR;                              ///< 这是一个普通文件
    if (!(dir = dir_namei(pathname, &namelen, &basename)))      ///< 获取文件所属文件夹 inode、文件名长度、文件名。（都是去掉路径后的），dir = 待打开文件的所属文件夹的 inode。
        return -ENOENT;
    if (!namelen)                                   /* special case: '/usr/' etc */
    {
        if (!(flag & (O_ACCMODE | O_CREAT | O_TRUNC)))      ///< 不是读写/创建/截断
        {
            *res_inode = dir;                       ///< 将获取到的文件 inode 返回。
            return 0;
        }
        iput(dir);
        return -EISDIR;                             ///< 报文件夹错误
    }
    bh = find_entry(&dir, basename, namelen, &de);  ///< 找到目标文件的 inode。dir 为 tty0 所属文件夹 dev 的 inode, basename = tty0, namelen = 4

    /// 找不到该文件的 inode
    if (!bh)
    {
        /// 文件 inode 找不到，又不具备创建/写权限，那就按错误处理
        if (!(flag & O_CREAT)) 
        {
            iput(dir);
            return -ENOENT;
        }
        if (!permission(dir, MAY_WRITE)) 
        {
            iput(dir);
            return -EACCES;
        }
        inode = new_inode(dir->i_dev);                  ///< 创建一个新的 indoe。
        if (!inode) 
        {
            iput(dir);
            return -ENOSPC;                             ///< 设备上没有剩余空间。
        }
        inode->i_uid = current->euid;
        inode->i_mode = mode;
        inode->i_dirt = 1;
        bh = add_entry(dir, basename, namelen, &de);    ///< 在 dir 中加 basename 目录项。dir = tty0 所属文件夹 dev 的 inode; basename = tty0; namelen = 4
        if (!bh)                        ///< 加入失败的错误处理
        {
            inode->i_nlinks--;
            iput(inode);
            iput(dir);
            return -ENOSPC;
        }
        de->inode = inode->i_num;       ///< 填写 inode 号。
        bh->b_dirt = 1;                 ///< 该磁盘内容被改写，需要同步。
        brelse(bh);
        iput(dir);
        *res_inode = inode;
        return 0;
    }

    /// 该文件 inode 存在。
    inr = de->inode;
    dev = dir->i_dev;
    brelse(bh);
    iput(dir);
    if (flag & O_EXCL)
        return -EEXIST;
    if (!(inode=iget(dev,inr)))
        return -EACCES;
    if ((S_ISDIR(inode->i_mode) && (flag & O_ACCMODE)) ||
        !permission(inode,ACC_MODE(flag))) {
        iput(inode);
        return -EPERM;
    }
    inode->i_atime = CURRENT_TIME;
    if (flag & O_TRUNC)
        truncate(inode);
    *res_inode = inode;
    return 0;
}

int sys_mknod(const char * filename, int mode, int dev)
{
    const char * basename;
    int namelen;
    struct m_inode * dir, * inode;
    struct buffer_head * bh;
    struct dir_entry * de;
    
    if (!suser())
        return -EPERM;
    if (!(dir = dir_namei(filename,&namelen,&basename)))
        return -ENOENT;
    if (!namelen) {
        iput(dir);
        return -ENOENT;
    }
    if (!permission(dir,MAY_WRITE)) {
        iput(dir);
        return -EPERM;
    }
    bh = find_entry(&dir,basename,namelen,&de);
    if (bh) {
        brelse(bh);
        iput(dir);
        return -EEXIST;
    }
    inode = new_inode(dir->i_dev);
    if (!inode) {
        iput(dir);
        return -ENOSPC;
    }
    inode->i_mode = mode;
    if (S_ISBLK(mode) || S_ISCHR(mode))
        inode->i_zone[0] = dev;
    inode->i_mtime = inode->i_atime = CURRENT_TIME;
    inode->i_dirt = 1;
    bh = add_entry(dir,basename,namelen,&de);
    if (!bh) {
        iput(dir);
        inode->i_nlinks=0;
        iput(inode);
        return -ENOSPC;
    }
    de->inode = inode->i_num;
    bh->b_dirt = 1;
    iput(dir);
    iput(inode);
    brelse(bh);
    return 0;
}

int sys_mkdir(const char * pathname, int mode)
{
    const char * basename;
    int namelen;
    struct m_inode * dir, * inode;
    struct buffer_head * bh, *dir_block;
    struct dir_entry * de;

    if (!suser())
        return -EPERM;
    if (!(dir = dir_namei(pathname,&namelen,&basename)))
        return -ENOENT;
    if (!namelen) {
        iput(dir);
        return -ENOENT;
    }
    if (!permission(dir,MAY_WRITE)) {
        iput(dir);
        return -EPERM;
    }
    bh = find_entry(&dir,basename,namelen,&de);
    if (bh) {
        brelse(bh);
        iput(dir);
        return -EEXIST;
    }
    inode = new_inode(dir->i_dev);
    if (!inode) {
        iput(dir);
        return -ENOSPC;
    }
    inode->i_size = 32;
    inode->i_dirt = 1;
    inode->i_mtime = inode->i_atime = CURRENT_TIME;
    if (!(inode->i_zone[0]=new_block(inode->i_dev))) {
        iput(dir);
        inode->i_nlinks--;
        iput(inode);
        return -ENOSPC;
    }
    inode->i_dirt = 1;
    if (!(dir_block=bread(inode->i_dev,inode->i_zone[0]))) {
        iput(dir);
        free_block(inode->i_dev,inode->i_zone[0]);
        inode->i_nlinks--;
        iput(inode);
        return -ERROR;
    }
    de = (struct dir_entry *) dir_block->b_data;
    de->inode=inode->i_num;
    strcpy(de->name,".");
    de++;
    de->inode = dir->i_num;
    strcpy(de->name,"..");
    inode->i_nlinks = 2;
    dir_block->b_dirt = 1;
    brelse(dir_block);
    inode->i_mode = I_DIRECTORY | (mode & 0777 & ~current->umask);
    inode->i_dirt = 1;
    bh = add_entry(dir,basename,namelen,&de);
    if (!bh) {
        iput(dir);
        free_block(inode->i_dev,inode->i_zone[0]);
        inode->i_nlinks=0;
        iput(inode);
        return -ENOSPC;
    }
    de->inode = inode->i_num;
    bh->b_dirt = 1;
    dir->i_nlinks++;
    dir->i_dirt = 1;
    iput(dir);
    iput(inode);
    brelse(bh);
    return 0;
}

/*
 * routine to check that the specified directory is empty (for rmdir)
 */
static int empty_dir(struct m_inode * inode)
{
    int nr,block;
    int len;
    struct buffer_head * bh;
    struct dir_entry * de;

    len = inode->i_size / sizeof (struct dir_entry);
    if (len<2 || !inode->i_zone[0] ||
        !(bh=bread(inode->i_dev,inode->i_zone[0]))) {
            printk("warning - bad directory on dev %04x\n",inode->i_dev);
        return 0;
    }
    de = (struct dir_entry *) bh->b_data;
    if (de[0].inode != inode->i_num || !de[1].inode || 
        strcmp(".",de[0].name) || strcmp("..",de[1].name)) {
            printk("warning - bad directory on dev %04x\n",inode->i_dev);
        return 0;
    }
    nr = 2;
    de += 2;
    while (nr<len) {
        if ((void *) de >= (void *) (bh->b_data+BLOCK_SIZE)) {
            brelse(bh);
            block=bmap(inode,nr/DIR_ENTRIES_PER_BLOCK);
            if (!block) {
                nr += DIR_ENTRIES_PER_BLOCK;
                continue;
            }
            if (!(bh=bread(inode->i_dev,block)))
                return 0;
            de = (struct dir_entry *) bh->b_data;
        }
        if (de->inode) {
            brelse(bh);
            return 0;
        }
        de++;
        nr++;
    }
    brelse(bh);
    return 1;
}

int sys_rmdir(const char * name)
{
    const char * basename;
    int namelen;
    struct m_inode * dir, * inode;
    struct buffer_head * bh;
    struct dir_entry * de;

    if (!suser())
        return -EPERM;
    if (!(dir = dir_namei(name,&namelen,&basename)))
        return -ENOENT;
    if (!namelen) {
        iput(dir);
        return -ENOENT;
    }
    if (!permission(dir,MAY_WRITE)) {
        iput(dir);
        return -EPERM;
    }
    bh = find_entry(&dir,basename,namelen,&de);
    if (!bh) {
        iput(dir);
        return -ENOENT;
    }
    if (!(inode = iget(dir->i_dev, de->inode))) {
        iput(dir);
        brelse(bh);
        return -EPERM;
    }
    if ((dir->i_mode & S_ISVTX) && current->euid &&
        inode->i_uid != current->euid) {
        iput(dir);
        iput(inode);
        brelse(bh);
        return -EPERM;
    }
    if (inode->i_dev != dir->i_dev || inode->i_count>1) {
        iput(dir);
        iput(inode);
        brelse(bh);
        return -EPERM;
    }
    if (inode == dir) {    /* we may not delete ".", but "../dir" is ok */
        iput(inode);
        iput(dir);
        brelse(bh);
        return -EPERM;
    }
    if (!S_ISDIR(inode->i_mode)) {
        iput(inode);
        iput(dir);
        brelse(bh);
        return -ENOTDIR;
    }
    if (!empty_dir(inode)) {
        iput(inode);
        iput(dir);
        brelse(bh);
        return -ENOTEMPTY;
    }
    if (inode->i_nlinks != 2)
        printk("empty directory has nlink!=2 (%d)",inode->i_nlinks);
    de->inode = 0;
    bh->b_dirt = 1;
    brelse(bh);
    inode->i_nlinks=0;
    inode->i_dirt=1;
    dir->i_nlinks--;
    dir->i_ctime = dir->i_mtime = CURRENT_TIME;
    dir->i_dirt=1;
    iput(dir);
    iput(inode);
    return 0;
}

int sys_unlink(const char * name)
{
    const char * basename;
    int namelen;
    struct m_inode * dir, * inode;
    struct buffer_head * bh;
    struct dir_entry * de;

    if (!(dir = dir_namei(name,&namelen,&basename)))
        return -ENOENT;
    if (!namelen) {
        iput(dir);
        return -ENOENT;
    }
    if (!permission(dir,MAY_WRITE)) {
        iput(dir);
        return -EPERM;
    }
    bh = find_entry(&dir,basename,namelen,&de);
    if (!bh) {
        iput(dir);
        return -ENOENT;
    }
    if (!(inode = iget(dir->i_dev, de->inode))) {
        iput(dir);
        brelse(bh);
        return -ENOENT;
    }
    if ((dir->i_mode & S_ISVTX) && !suser() &&
        current->euid != inode->i_uid &&
        current->euid != dir->i_uid) {
        iput(dir);
        iput(inode);
        brelse(bh);
        return -EPERM;
    }
    if (S_ISDIR(inode->i_mode)) {
        iput(inode);
        iput(dir);
        brelse(bh);
        return -EPERM;
    }
    if (!inode->i_nlinks) {
        printk("Deleting nonexistent file (%04x:%d), %d\n",
            inode->i_dev,inode->i_num,inode->i_nlinks);
        inode->i_nlinks=1;
    }
    de->inode = 0;
    bh->b_dirt = 1;
    brelse(bh);
    inode->i_nlinks--;
    inode->i_dirt = 1;
    inode->i_ctime = CURRENT_TIME;
    iput(inode);
    iput(dir);
    return 0;
}

int sys_link(const char * oldname, const char * newname)
{
    struct dir_entry * de;
    struct m_inode * oldinode, * dir;
    struct buffer_head * bh;
    const char * basename;
    int namelen;

    oldinode=namei(oldname);
    if (!oldinode)
        return -ENOENT;
    if (S_ISDIR(oldinode->i_mode)) {
        iput(oldinode);
        return -EPERM;
    }
    dir = dir_namei(newname,&namelen,&basename);
    if (!dir) {
        iput(oldinode);
        return -EACCES;
    }
    if (!namelen) {
        iput(oldinode);
        iput(dir);
        return -EPERM;
    }
    if (dir->i_dev != oldinode->i_dev) {
        iput(dir);
        iput(oldinode);
        return -EXDEV;
    }
    if (!permission(dir,MAY_WRITE)) {
        iput(dir);
        iput(oldinode);
        return -EACCES;
    }
    bh = find_entry(&dir,basename,namelen,&de);
    if (bh) {
        brelse(bh);
        iput(dir);
        iput(oldinode);
        return -EEXIST;
    }
    bh = add_entry(dir,basename,namelen,&de);
    if (!bh) {
        iput(dir);
        iput(oldinode);
        return -ENOSPC;
    }
    de->inode = oldinode->i_num;
    bh->b_dirt = 1;
    brelse(bh);
    iput(dir);
    oldinode->i_nlinks++;
    oldinode->i_ctime = CURRENT_TIME;
    oldinode->i_dirt = 1;
    iput(oldinode);
    return 0;
}
