/*
 * This file has definitions for some important file table
 * structures etc.
 */

#ifndef _FS_H
#define _FS_H

#include <sys/types.h>

/* devices are as follows: (same as minix, so we can use the minix
 * file system. These are major numbers.)
 *
 * 0 - unused (nodev)
 * 1 - /dev/mem
 * 2 - /dev/fd
 * 3 - /dev/hd
 * 4 - /dev/ttyx
 * 5 - /dev/tty
 * 6 - /dev/lp
 * 7 - unnamed pipes
 */

#define IS_SEEKABLE(x) ((x)>=1 && (x)<=3)

#define READ 0
#define WRITE 1
#define READA 2        /* read-ahead - don't pause */
#define WRITEA 3    /* "write-ahead" - silly, but somewhat useful */

void buffer_init(long buffer_end);

#define MAJOR(a) (((unsigned)(a))>>8)        ///< 主设备号（高8位）
#define MINOR(a) ((a)&0xff)                  ///< 次设备号（低8位）

#define NAME_LEN 14     /* 最长文件名。*/
#define ROOT_INO 1      /* 一个文件系统根目录的 inode 号。*/

#define I_MAP_SLOTS 8
#define Z_MAP_SLOTS 8
#define SUPER_MAGIC 0x137F

#define NR_OPEN 20
#define NR_INODE 32         /* 内存中 inode 只有32个，文件系统中远远超过这个数字 */
#define NR_FILE 64
#define NR_SUPER 8          /* 可以容纳 8 个已挂载文件系统的超级块 */
#define NR_HASH 307
#define NR_BUFFERS nr_buffers
#define BLOCK_SIZE 1024     /* 1KB */
#define BLOCK_SIZE_BITS 10
#ifndef NULL
#define NULL ((void *) 0)
#endif

#define INODES_PER_BLOCK ((BLOCK_SIZE)/(sizeof (struct d_inode)))
#define DIR_ENTRIES_PER_BLOCK ((BLOCK_SIZE)/(sizeof (struct dir_entry)))

#define PIPE_HEAD(inode) ((inode).i_zone[0])
#define PIPE_TAIL(inode) ((inode).i_zone[1])
#define PIPE_SIZE(inode) ((PIPE_HEAD(inode)-PIPE_TAIL(inode))&(PAGE_SIZE-1))
#define PIPE_EMPTY(inode) (PIPE_HEAD(inode)==PIPE_TAIL(inode))
#define PIPE_FULL(inode) (PIPE_SIZE(inode)==(PAGE_SIZE-1))
#define INC_PIPE(head) \
__asm__("incl %0\n\tandl $4095,%0"::"m" (head))

typedef char buffer_block[BLOCK_SIZE];

/**
 * @brief 磁盘的内存映射
 */
struct buffer_head 
{
    char * b_data;                  /* pointer to data block (1024 bytes) */
    unsigned long b_blocknr;        /* 对应硬盘的逻辑块号，标识这块内存对应磁盘的第多少个块 */
    unsigned short b_dev;           /* device (0 = free) */
    unsigned char b_uptodate;       /* 表示该缓冲块（buffer）中的数据是否是最新的、已从磁盘读取或已正确写入的。*/
    unsigned char b_dirt;           /* 是否为脏的标记，脏表示需要同步到磁盘。0-非脏，1-脏。 */
    unsigned char b_count;          /* 使用当前缓冲区的进程个数。*/
    unsigned char b_lock;           /* 0 - ok, 1 -locked */
    struct task_struct * b_wait;    /* 等待在此内存块的进程 */
    struct buffer_head * b_prev;
    struct buffer_head * b_next;
    struct buffer_head * b_prev_free;
    struct buffer_head * b_next_free;
};

/**
 * @brief inode 在硬盘中的存储格式
 */
struct d_inode 
{
    unsigned short i_mode;          ///< 文件类型和权限
    unsigned short i_uid;           ///< 所有者用户 ID
    unsigned long i_size;           ///< 文件大小（字节）
    unsigned long i_time;           ///< 最后修改时间
    unsigned char i_gid;            ///< 所属组 ID
    unsigned char i_nlinks;         ///< 硬链接数，当减小为0时，会删除
    unsigned short i_zone[9];       ///< 数据块指针，指向文件实际数据存储的物理块号，前 7 个直接指针，第 8 个为一级指针，第 9 个为二级指针。
                                    ///< 如果是块设备，则 i_zone[0] 存储了设备号。
};

/**
 * @brief 内存 inode，文件系统级的数据结构。
 * @details inode 可以指向块设备（/dev/hda）、目录或文件、管道。
 */
struct m_inode
{
    unsigned short i_mode;          ///< i_mode 高几位存文件类型，低 9 位存权限（所有者/所属组/其他人）。文件类型，可以为块设备文件、管道、目录、普通文件。
    unsigned short i_uid;           ///< 文件所有者的用户标识符
    unsigned long i_size;           ///< 如果是文件，则为文件大小；
                                    ///< 如果是目录，里面存储了dir_entry表，即目录项表。
                                    ///< 如果是管道，存储管道缓冲区所在物理内存地址。
    unsigned long i_mtime;          ///< 文件内容最后一次被修改的时间戳
    unsigned char i_gid;            ///< 文件所属组的标识符
    unsigned char i_nlinks;         ///< 硬链接数。
    unsigned short i_zone[9];       ///< 数据块。文件实际数据存储的物理块号，前 7 个直接数据物理块号，第 8 个为一级指针，第 9 个为二级指针。
                                    ///< 如果是块设备，则 i_zone[0] 存储了设备号。
                                    ///< 如果是目录，存储目录项。

    /* 下面这些属性是内存 inode 独有的，内存 inode 继承了磁盘 inode */
    struct task_struct * i_wait;    ///< 等待该文件的进程队列。
    unsigned long i_atime;          ///< 访问时间，文件最后被读取（Read）的时间。
    unsigned long i_ctime;          ///< 状态改变时间，文件元数据（如权限、所有者）最后被修改的时间。
    unsigned short i_dev;           ///< 设备号，该 inode 所在的设备编号（如 0x301 代表 hda1）。
    unsigned short i_num;           ///< inode 编号，该 inode 在文件系统中的唯一编号（索引）。inode 号 1 固定是根目录。
    unsigned short i_count;         ///< 引用计数，记录当前有多少个进程或数据结构正在使用这个 inode。
    unsigned char i_lock;           ///< 锁标志。
    unsigned char i_dirt;           ///< 脏标记。
    unsigned char i_pipe;           ///< 管道标记，如果该 inode 代表一个命名管道（FIFO），此标志置 1。
    unsigned char i_mount;          ///< 挂载点标记。如果该 inode 是一个文件系统的挂载点（Mount Point），此标志置 1。
    unsigned char i_seek;           ///< 寻址标记，内部使用，通常与文件偏移量相关，标记是否需要进行特殊的寻址操作。
    unsigned char i_update;         ///< 更新标记，辅助标志，用于标记某些特定的更新状态，配合 i_dirt 使用。
};

/**
 * @brief 文件对象
 */
struct file 
{
    unsigned short f_mode;          ///< 文件访问模式
    unsigned short f_flags;         ///< 文件状态标志
    unsigned short f_count;         ///< 文件引用计数
    struct m_inode * f_inode;       ///< 指向内存 inode
    off_t f_pos;                    ///< 当前文件位置，偏移量
};

/**
 * @brief 文件系统超级块，一个文件系统超级块代表了一个分区。
 * @details 一个 super_block 能表示 64Mb 硬盘。
 */
struct super_block
{
    unsigned short s_ninodes;           ///< inode 总数。
    unsigned short s_nzones;            ///< 磁盘总块数（1块 = 1KB）。
    unsigned short s_imap_blocks;       ///< inode 位图占用的逻辑块数量（一个逻辑块 1KB）。
    unsigned short s_zmap_blocks;       ///< 数据块位图占用的磁盘块数（每个数据块 1KB）。
    unsigned short s_firstdatazone;     ///< 第一个数据块的块号
    unsigned short s_log_zone_size;     ///< 
    unsigned long s_max_size;           ///<
    unsigned short s_magic;             ///< 魔数，Minix文件系统时 0x137F
/* These are only in memory */
    struct buffer_head * s_imap[8];     ///< inode 位图在内存中的缓冲。inode 位图中的每一位表示一个 inode 是否被使用（0表示空闲，1表示使用）。
                                        ///< 当创建新文件时，文件系统会扫描 inode 位图，找到第一个空闲位，将其置为 1，分配对应的 inode；
                                        ///< 删除文件时，将对应位清 0，释放 inode。

    struct buffer_head * s_zmap[8];     ///< 磁盘块占用情况。一个磁盘块用 1 bit 表示，一个 buffer_head 有 1024 Bytes，能表示 8192 个磁盘块占用情况。
                                        ///< 这样这个数组能表示 8192 * 8 个数据块，一个块大小为 1K。总共能表示 8192 * 8 * 1024 = 64 Mb。

    unsigned short s_dev;               ///< 该超级块对应的设备号（0x0301=/dev/hda1）
    struct m_inode * s_isup;            ///< 指向根目录的 inode（/目录）
    struct m_inode * s_imount;          ///< 如果该文件系统（如 U 盘）被 mount 到某个目录，指向该目录的 inode。
                                        ///< 超级块也会增加 inode 的一个引用计数，致使 s_imount 的 inode 永远不会被释放。
    unsigned long s_time;               ///< 最后一次修改时间
    struct task_struct * s_wait;        ///< 等待该超级块的进程队列
    unsigned char s_lock;               ///< 锁定标志，防止并发修改
    unsigned char s_rd_only;            ///< 只读标志
    unsigned char s_dirt;               ///< 脏标志
};

struct d_super_block {
    unsigned short s_ninodes;
    unsigned short s_nzones;
    unsigned short s_imap_blocks;
    unsigned short s_zmap_blocks;
    unsigned short s_firstdatazone;
    unsigned short s_log_zone_size;
    unsigned long s_max_size;
    unsigned short s_magic;
};

/**
 * @brief 目录项
 */
struct dir_entry
{
    unsigned short inode;       ///< 索引节点编号。
    char name[NAME_LEN];        ///< 文件名。固定长度字符数组。
};

extern struct m_inode inode_table[NR_INODE];
extern struct file file_table[NR_FILE];
extern struct super_block super_block[NR_SUPER];
extern struct buffer_head * start_buffer;
extern int nr_buffers;

extern void check_disk_change(int dev);
extern int floppy_change(unsigned int nr);
extern int ticks_to_floppy_on(unsigned int dev);
extern void floppy_on(unsigned int dev);
extern void floppy_off(unsigned int dev);
extern void truncate(struct m_inode * inode);
extern void sync_inodes(void);
extern void wait_on(struct m_inode * inode);
extern int bmap(struct m_inode * inode,int block);
extern int create_block(struct m_inode * inode,int block);
extern struct m_inode * namei(const char * pathname);
extern int open_namei(const char * pathname, int flag, int mode,
    struct m_inode ** res_inode);
extern void iput(struct m_inode * inode);
extern struct m_inode * iget(int dev,int nr);
extern struct m_inode * get_empty_inode(void);
extern struct m_inode * get_pipe_inode(void);
extern struct buffer_head * get_hash_table(int dev, int block);
extern struct buffer_head * getblk(int dev, int block);
extern void ll_rw_block(int rw, struct buffer_head * bh);
extern void brelse(struct buffer_head * buf);
extern struct buffer_head * bread(int dev,int block);
extern void bread_page(unsigned long addr,int dev,int b[4]);
extern struct buffer_head * breada(int dev,int block,...);
extern int new_block(int dev);
extern void free_block(int dev, int block);
extern struct m_inode * new_inode(int dev);
extern void free_inode(struct m_inode * inode);
extern int sync_dev(int dev);
extern struct super_block * get_super(int dev);
extern int ROOT_DEV;

extern void mount_root(void);

#endif
