#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <utime.h>
#include <sys/stat.h>

#include <linux/sched.h>
#include <linux/tty.h>
#include <linux/kernel.h>
#include <asm/segment.h>

int sys_ustat(int dev, struct ustat * ubuf)
{
    return -ENOSYS;
}

int sys_utime(char * filename, struct utimbuf * times)
{
    struct m_inode * inode;
    long actime,modtime;

    if (!(inode=namei(filename)))
        return -ENOENT;
    if (times) {
        actime = get_fs_long((unsigned long *) &times->actime);
        modtime = get_fs_long((unsigned long *) &times->modtime);
    } else
        actime = modtime = CURRENT_TIME;
    inode->i_atime = actime;
    inode->i_mtime = modtime;
    inode->i_dirt = 1;
    iput(inode);
    return 0;
}

/*
 * XXX should we use the real or effective uid?  BSD uses the real uid,
 * so as to make this call useful to setuid programs.
 */
int sys_access(const char * filename,int mode)
{
    struct m_inode * inode;
    int res, i_mode;

    mode &= 0007;
    if (!(inode=namei(filename)))
        return -EACCES;
    i_mode = res = inode->i_mode & 0777;
    iput(inode);
    if (current->uid == inode->i_uid)
        res >>= 6;
    else if (current->gid == inode->i_gid)
        res >>= 6;
    if ((res & 0007 & mode) == mode)
        return 0;
    /*
     * XXX we are doing this test last because we really should be
     * swapping the effective with the real user id (temporarily),
     * and then calling suser() routine.  If we do call the
     * suser() routine, it needs to be called last. 
     */
    if ((!current->uid) &&
        (!(mode & 1) || (i_mode & 0111)))
        return 0;
    return -EACCES;
}

int sys_chdir(const char * filename)
{
    struct m_inode * inode;

    if (!(inode = namei(filename)))
        return -ENOENT;
    if (!S_ISDIR(inode->i_mode)) {
        iput(inode);
        return -ENOTDIR;
    }
    iput(current->pwd);
    current->pwd = inode;
    return (0);
}

int sys_chroot(const char * filename)
{
    struct m_inode * inode;

    if (!(inode=namei(filename)))
        return -ENOENT;
    if (!S_ISDIR(inode->i_mode)) {
        iput(inode);
        return -ENOTDIR;
    }
    iput(current->root);
    current->root = inode;
    return (0);
}

int sys_chmod(const char * filename,int mode)
{
    struct m_inode * inode;

    if (!(inode=namei(filename)))
        return -ENOENT;
    if ((current->euid != inode->i_uid) && !suser()) {
        iput(inode);
        return -EACCES;
    }
    inode->i_mode = (mode & 07777) | (inode->i_mode & ~07777);
    inode->i_dirt = 1;
    iput(inode);
    return 0;
}

int sys_chown(const char * filename,int uid,int gid)
{
    struct m_inode * inode;

    if (!(inode=namei(filename)))
        return -ENOENT;
    if (!suser()) {
        iput(inode);
        return -EACCES;
    }
    inode->i_uid=uid;
    inode->i_gid=gid;
    inode->i_dirt=1;
    iput(inode);
    return 0;
}

/// @retval EINVAL 打开文件数过多 or 全局文件表用完了
int sys_open(const char * filename, int flag, int mode)     ///< filename = /dev/tty0, flag = O_RDWR
{
    struct m_inode * inode;
    struct file * f;
    int i, fd;                              ///< 文件描述符

    mode &= 0777 & ~current->umask;         ///< 获取文件权限。
    for(fd = 0 ; fd < NR_OPEN ; fd++)       ///< 寻找一块空闲文件指针
        if (!current->filp[fd])
            break;
    if (fd >= NR_OPEN)                      ///< 判断打开文件数是否超限
        return -EINVAL;
    current->close_on_exec &= ~(1 << fd);   ///< 在执行exec时默认不关闭该文件描述符。

    /* 在全局文件表中找一个空闲的文件对象 */
    f = 0 + file_table;                     ///< f 指向全局文件表的首地址。
    for (i = 0 ; i < NR_FILE ; i++, f++)
        if (!f->f_count)
            break;
    if (i >= NR_FILE)                       ///< 全局文件表用完了
        return -EINVAL;

    (current->filp[fd] = f)->f_count++;     ///< 更新进程打开文件指针，更新文件引用计数。
    if ((i = open_namei(filename, flag, mode, &inode)) < 0)     ///< flag = O_RDWR
    {
        current->filp[fd] = NULL;
        f->f_count = 0;
        return i;
    }
/* ttys are somewhat special (ttyxx major==4, tty major==5) */
    if (S_ISCHR(inode->i_mode))
        if (MAJOR(inode->i_zone[0])==4) {
            if (current->leader && current->tty<0) {
                current->tty = MINOR(inode->i_zone[0]);
                tty_table[current->tty].pgrp = current->pgrp;
            }
        } else if (MAJOR(inode->i_zone[0])==5)
            if (current->tty<0) {
                iput(inode);
                current->filp[fd]=NULL;
                f->f_count=0;
                return -EPERM;
            }
/* Likewise with block-devices: check for floppy_change */
    if (S_ISBLK(inode->i_mode))
        check_disk_change(inode->i_zone[0]);
    f->f_mode = inode->i_mode;
    f->f_flags = flag;
    f->f_count = 1;
    f->f_inode = inode;
    f->f_pos = 0;
    return (fd);
}

int sys_creat(const char * pathname, int mode)
{
    return sys_open(pathname, O_CREAT | O_TRUNC, mode);
}

int sys_close(unsigned int fd)
{    
    struct file * filp;

    if (fd >= NR_OPEN)
        return -EINVAL;
    current->close_on_exec &= ~(1<<fd);
    if (!(filp = current->filp[fd]))
        return -EINVAL;
    current->filp[fd] = NULL;
    if (filp->f_count == 0)
        panic("Close: file count is 0");
    if (--filp->f_count)
        return (0);
    iput(filp->f_inode);
    return (0);
}
