/*
 *  linux/kernel/hd.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * This is the low-level hd interrupt support. It traverses the
 * request-list, using interrupts to jump between functions. As
 * all the functions are called within interrupts, we may not
 * sleep. Special care is recommended.
 * 
 *  modified by Drew Eckhardt to check nr of hd's from the CMOS.
 */

#include <linux/config.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/hdreg.h>
#include <asm/system.h>
#include <asm/io.h>
#include <asm/segment.h>

#define MAJOR_NR 3
#include "blk.h"

#define CMOS_READ(addr) ({ \
outb_p(0x80|addr,0x70); \
inb_p(0x71); \
})

/* Max read/write errors/sector */
#define MAX_ERRORS	7           ///< 硬盘最大读写错误次数
#define MAX_HD		2

static void recal_intr(void);

static int recalibrate = 1;
static int reset = 1;           ///< 硬盘重置标识
/* hd 结构。
 * head：磁头号，标识硬盘的哪个磁头正在读写。sect：逻辑扇区号，指定当前要读写的扇区。
 * cyl：柱面号。wpcom：写预补偿（早起硬盘中，当数据记录在高密度磁介质上时，相邻的磁记录位可能会相互干扰）。
 * lzone：磁头在断电后应该放置的位置（随意放置可能破坏磁盘）。ctl：控制寄存器，控制硬盘的工作模式和操作命令。 */
struct hd_i_struct {
    int head,sect,cyl,wpcom,lzone,ctl;
};
/* 硬盘结构 */
#ifdef HD_TYPE
    struct hd_i_struct hd_info[] = { HD_TYPE };
    #define NR_HD ((sizeof (hd_info))/(sizeof (struct hd_i_struct)))
#else
    struct hd_i_struct hd_info[] = { {0,0,0,0,0,0},{0,0,0,0,0,0} };
    static int NR_HD = 0;    ///< 记录硬盘个数，后续在 bios 读取硬盘信息和 cmos 获取硬盘信息会修改
#endif
/* 硬盘设备分区 */
static struct hd_struct {
    long start_sect;        ///< 起始扇区号
    long nr_sects;          ///< 分区大小（扇区数）
} hd[5*MAX_HD]={{0,0},};    ///< 每个盘有 5 个分区。

#define port_read(port,buf,nr) \
__asm__("cld;rep;insw"::"d" (port),"D" (buf),"c" (nr):"cx","di")

#define port_write(port,buf,nr) \
__asm__("cld;rep;outsw"::"d" (port),"S" (buf),"c" (nr):"cx","si")

extern void hd_interrupt(void);
extern void rd_load(void);

/* This may be used only once, enforced by 'static int callable' */
int sys_setup(void * BIOS)
{
    static int callable = 1;
    int i,drive;
    unsigned char cmos_disks;
    struct partition *p;
    struct buffer_head * bh;

    if (!callable)
        return -1;
    callable = 0;
#ifndef HD_TYPE				///< 从BIOS里面读取数据
    for (drive=0 ; drive<2 ; drive++) {
        hd_info[drive].cyl = *(unsigned short *) BIOS;
        hd_info[drive].head = *(unsigned char *) (2+BIOS);
        hd_info[drive].wpcom = *(unsigned short *) (5+BIOS);
        hd_info[drive].ctl = *(unsigned char *) (8+BIOS);
        hd_info[drive].lzone = *(unsigned short *) (12+BIOS);
        hd_info[drive].sect = *(unsigned char *) (14+BIOS);
        BIOS += 16;
    }
    if (hd_info[1].cyl)
        NR_HD=2;
    else
        NR_HD=1;
#endif
    for (i=0 ; i<NR_HD ; i++) {
        hd[i*5].start_sect = 0;
        hd[i*5].nr_sects = hd_info[i].head*
                hd_info[i].sect*hd_info[i].cyl;
    }

    /*
        We querry CMOS about hard disks : it could be that 
        we have a SCSI/ESDI/etc controller that is BIOS
        compatable with ST-506, and thus showing up in our
        BIOS table, but not register compatable, and therefore
        not present in CMOS.

        Furthurmore, we will assume that our ST-506 drives
        <if any> are the primary drives in the system, and 
        the ones reflected as drive 1 or 2.

        The first drive is stored in the high nibble of CMOS
        byte 0x12, the second in the low nibble.  This will be
        either a 4 bit drive type or 0xf indicating use byte 0x19 
        for an 8 bit type, drive 1, 0x1a for drive 2 in CMOS.

        Needless to say, a non-zero value means we have 
        an AT controller hard disk for that drive.

        
    */

    if ((cmos_disks = CMOS_READ(0x12)) & 0xf0)
        if (cmos_disks & 0x0f)
            NR_HD = 2;
        else
            NR_HD = 1;
    else
        NR_HD = 0;
    for (i = NR_HD ; i < 2 ; i++) {
        hd[i*5].start_sect = 0;
        hd[i*5].nr_sects = 0;
    }
    for (drive=0 ; drive<NR_HD ; drive++) {
        if (!(bh = bread(0x300 + drive*5,0))) { ///< 0x300是第一块硬盘/dev/hd0，硬盘分区表在MBR内部，需要读取MBR来获取硬盘分区表（分区表位于MBR扇区最后66字节（64+0x55aa））。
            printk("Unable to read partition table of drive %d\n\r",
                drive);
            panic("");
        }
        if (bh->b_data[510] != 0x55 || (unsigned char)
            bh->b_data[511] != 0xAA) {
            printk("Bad partition table on drive %d\n\r",drive);
            panic("");
        }
        p = 0x1BE + (void *)bh->b_data;
        for (i=1;i<5;i++,p++) {
            hd[i+5*drive].start_sect = p->start_sect;
            hd[i+5*drive].nr_sects = p->nr_sects;
        }
        brelse(bh);
    }
    if (NR_HD)
        printk("Partition table%s ok.\n\r",(NR_HD>1)?"s":"");
    rd_load();
    mount_root();
    return (0);
}
/* 确保硬盘控制器处于 READY 状态 */
static int controller_ready(void)
{
    int retries=10000;

    while (--retries && (inb_p(HD_STATUS)&0xc0)!=0x40);
    return (retries);
}
/* 读取硬盘返回结果，没有问题则返回 0，有问题则返回 1。 */
static int win_result(void)
{
    int i = inb_p(HD_STATUS);   ///< 从 0x1f7 端口获取处理结果

    if ((i & (BUSY_STAT | READY_STAT | WRERR_STAT | SEEK_STAT | ERR_STAT))
        == (READY_STAT | SEEK_STAT))
        return(0); /* ok */     ///< 检测是否 READY，READY 则返回 0。
    if (i&1) i=inb(HD_ERROR);   ///< 如果出现错误，则读取错误码
    return (1);
}
/* 硬盘执行命令，完成后硬盘产生 IRQ14 中断，CPU 跳转到中断向量 0x2E 进行处理 */
static void hd_out(unsigned int drive,unsigned int nsect,unsigned int sect,
        unsigned int head,unsigned int cyl,unsigned int cmd,
        void (*intr_addr)(void))
{
    register int port asm("dx");                ///< 凡是涉及到 port 的操作都用 dx。

    if (drive>1 || head>15)                     ///< linux0.11 最多支持 2 个 IDE 硬盘，0 是主盘，1 是从盘
        panic("Trying to write bad sector");    ///< head 为磁头号，用 4 位表示，所以最大为 15
    if (!controller_ready())
        panic("HD controller not ready");
    do_hd = intr_addr;
    outb_p(hd_info[drive].ctl,HD_CMD);          ///< 设置控制位，防止之前的操作遗留异常
    port=HD_DATA;
    outb_p(hd_info[drive].wpcom>>2,++port);     ///< port = 0x1f1，设置写预补偿，早期硬盘特性
    outb_p(nsect,++port);                       ///< port = 0x1f2，待读取的扇区个数
    outb_p(sect,++port);                        ///< port = 0x1f3，待读取扇区的起始位置
    outb_p(cyl,++port);                         ///< port = 0x1f4，起始柱面低 8 位。
    outb_p(cyl>>8,++port);                      ///< port = 0x1f5，起始柱面高 8 位。
    outb_p(0xA0|(drive<<4)|head,++port);        ///< port = 0x1f6，
    outb(cmd,++port);                           ///< port = 0x1f7，设置 CHS 参数，需要先写入参数到其他寄存器（cmd = 0x91 为设置 CHS 参数）
}。

static int drive_busy(void)
{
    unsigned int i;

    for (i = 0; i < 10000; i++)         ///< 等待硬盘 READY
        if (READY_STAT == (inb_p(HD_STATUS) & (BUSY_STAT|READY_STAT)))
            break;
    i = inb(HD_STATUS);
    i &= BUSY_STAT | READY_STAT | SEEK_STAT;
    if (i == READY_STAT | SEEK_STAT)    ///< 硬盘就绪 | 寻道完成
        return(0);
    printk("HD controller times out\n\r");
    return(1);
}

static void reset_controller(void)
{
    int	i;

    outb(4, HD_CMD);                    ///< 软件复位
    for(i = 0; i < 100; i++) nop();     ///< 等待
    outb(hd_info[0].ctl & 0x0f ,HD_CMD);///< 设置IDE控制器的控制寄存器（启用中断模式）
    if (drive_busy())
        printk("HD-controller still busy\n\r");
    if ((i = inb(HD_ERROR)) != 1)
        printk("HD-controller reset failed: %02x\n\r",i);
}
/* 复位硬盘 */
static void reset_hd(int nr)
{
    reset_controller();                 ///< 复位硬盘控制器
    hd_out(nr,hd_info[nr].sect,hd_info[nr].sect,hd_info[nr].head-1,
        hd_info[nr].cyl,WIN_SPECIFY,&recal_intr);       ///< 设置完硬盘参数后会产生IRQ14中断，对应中断向量0x2E。
}

void unexpected_hd_interrupt(void)
{
    printk("Unexpected HD interrupt\n\r");
}
/* 读写错误处理函数，当错误个数过多，清除 IO 队列头请求，设置重置磁盘标志。 */
static void bad_rw_intr(void)
{
    if (++CURRENT->errors >= MAX_ERRORS)    ///< 当前请求的错误数过多
        end_request(0);                     ///< 结束 IO 请求
    if (CURRENT->errors > MAX_ERRORS/2)     ///< 如果错误数过多，重置磁盘。
        reset = 1;
}

static void read_intr(void)
{
    if (win_result()) {
        bad_rw_intr();
        do_hd_request();
        return;
    }
    port_read(HD_DATA,CURRENT->buffer,256);
    CURRENT->errors = 0;
    CURRENT->buffer += 512;
    CURRENT->sector++;
    if (--CURRENT->nr_sectors) {
        do_hd = &read_intr;
        return;
    }
    end_request(1);
    do_hd_request();
}

static void write_intr(void)
{
    if (win_result()) {
        bad_rw_intr();
        do_hd_request();
        return;
    }
    if (--CURRENT->nr_sectors) {
        CURRENT->sector++;
        CURRENT->buffer += 512;
        do_hd = &write_intr;
        port_write(HD_DATA,CURRENT->buffer,256);
        return;
    }
    end_request(1);
    do_hd_request();
}
/* 硬盘初始化（WIN_SPECIFY）和重置硬盘磁头的操作（WIN_RESTORE），属于 IRQ14 的回调 */
static void recal_intr(void)
{
    if (win_result())       ///< 获取硬盘处理结果
        bad_rw_intr();
    do_hd_request();        ///< 进一步处理请求
}

void do_hd_request(void)
{
    int i,r;
    unsigned int block,dev;
    unsigned int sec,head,cyl;
    unsigned int nsect;

    INIT_REQUEST;
    dev = MINOR(CURRENT->dev);      ///< 次设备号。
    block = CURRENT->sector;        ///< 起始扇区号。
    if (dev >= 5*NR_HD || block+2 > hd[dev].nr_sects) {    ///< 判断是否超出最大设备号，待读取的设备块不能越界
        end_request(0);
        goto repeat;
    }
    block += hd[dev].start_sect;    ///< 结束位置。
    dev /= 5;                       ///< 计算是第几个硬盘。
    __asm__("divl %4":"=a" (block),"=d" (sec):"0" (block),"1" (0),
        "r" (hd_info[dev].sect));
    __asm__("divl %4":"=a" (cyl),"=d" (head):"0" (block),"1" (0),
        "r" (hd_info[dev].head));
    sec++;
    nsect = CURRENT->nr_sectors;    ///< 要读写扇区数
    if (reset) {
        reset = 0;
        recalibrate = 1;
        reset_hd(CURRENT_DEV);      ///< CURRENT_DEV = 0。
        return;
    }
    if (recalibrate) {
        recalibrate = 0;
        hd_out(dev,hd_info[CURRENT_DEV].sect, 0, 0, 0,
            WIN_RESTORE, &recal_intr);      ///< 恢复命令（Restore），用于将磁头移回 0 号柱面（即归位）
        return;
    }	
    if (CURRENT->cmd == WRITE) {
        hd_out(dev,nsect,sec,head,cyl,WIN_WRITE,&write_intr);
        for(i=0 ; i<3000 && !(r=inb_p(HD_STATUS)&DRQ_STAT) ; i++)
            /* nothing */ ;
        if (!r) {
            bad_rw_intr();
            goto repeat;
        }
        port_write(HD_DATA,CURRENT->buffer,256);
    } else if (CURRENT->cmd == READ) {  ///< 读请求
        hd_out(dev, nsect, sec, head, cyl, WIN_READ, &read_intr);
    } else
        panic("unknown hd-command");
}
/* 设置硬盘中断函数，清除对主硬盘中断的屏蔽 */
void hd_init(void)
{
    blk_dev[MAJOR_NR].request_fn = DEVICE_REQUEST;		///< MAJOR_NR = 3 , request_fn = do_hd_request
    set_intr_gate(0x2E, &hd_interrupt);                 ///< 设置硬盘中断处理函数。
    outb_p(inb_p(0x21)&0xfb,0x21);                      ///< 0x21:8259A控制寄存器的数据端口，0xfb=0b1111_1011，清除IRQ2的屏蔽位（级联到从片）。
    outb(inb_p(0xA1)&0xbf,0xA1);                        ///< 0xA1为从8259A芯片的数据端口，0xf=0b1011_1111，清除从片的IRQ14。
}
