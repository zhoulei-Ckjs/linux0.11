#define outb(value,port) \
__asm__ ("outb %%al,%%dx"::"a" (value),"d" (port))
/*
 * 从 port 端口读取 1 字节，返回读取到的值（1 字节）。 */
#define inb(port) ({ \
unsigned char _v; \
__asm__ volatile ("inb %%dx,%%al":"=a" (_v):"d" (port)); \
_v; \
})

#define outb_p(value,port) \
__asm__ ("outb %%al,%%dx\n" \
		"\tjmp 1f\n" \
		"1:\tjmp 1f\n" \
		"1:"::"a" (value),"d" (port))
/* 从 port 端口读取 1 字节，返回读取到的值（1 字节）。 */
#define inb_p(port) ({ \
unsigned char _v; \
__asm__ volatile ("inb %%dx,%%al\n" /* 从端口 dx 读取 1 字节到 al。 */ \
	"\tjmp 1f\n" \
	"1:\tjmp 1f\n" \
	"1:":"=a" (_v):"d" (port));     /* _v = eax，edx = 端口。 */ \
_v; \
})