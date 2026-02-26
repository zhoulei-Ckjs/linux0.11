/*
 *  linux/fs/file_table.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <linux/fs.h>

/* 全局文件表（共享），整个系统最多打开 64 个文件，所有进程打开的文件都从这里分配。*/
struct file file_table[NR_FILE];