/**
 * @fileinfo 当处于内核模式（kernel-mode）时，我们不能使用标准 printf，因为段寄存器 fs 很可能指向一些「令人担忧的」（'interesting'）内存区域（例如用户态地址空间）。
 * 因此我们自己实现一个 printf，在调用前先保存并恢复 fs 寄存器——这样一切就稳妥了。
 * 早期 fs 寄存器用来指向用户空间。
 */
#include <stdarg.h>
#include <stddef.h>

#include <linux/kernel.h>

static char buf[1024];

extern int vsprintf(char * buf, const char * fmt, va_list args);

int printk(const char *fmt, ...)
{
	va_list args;
	int i;

	va_start(args, fmt);
	i=vsprintf(buf,fmt,args);
	va_end(args);
	__asm__("push %%fs\n\t"
		"push %%ds\n\t"
		"pop %%fs\n\t"
		"pushl %0\n\t"
		"pushl $_buf\n\t"
		"pushl $0\n\t"
		"call _tty_write\n\t"
		"addl $8,%%esp\n\t"
		"popl %0\n\t"
		"pop %%fs"
		::"r" (i):"ax","cx","dx");
	return i;
}
