#ifndef _CTYPE_H
#define _CTYPE_H

#define _U	0x01	/* 大写 */
#define _L	0x02	/* 小写 */
#define _D	0x04	/* 数字 */
#define _C	0x08	/* cntrl */
#define _P	0x10	/* punctuation 标点符号。*/
#define _S	0x20	/* white space (space/lf/tab) （空白字符）是一类不可见或显示为空白的字符。*/
#define _X	0x40	/* 十六进制数字 */
#define _SP	0x80	/* hard space (0x20) 硬空格。与普通空格同形但禁止断行，在 C 的字符分类中被当作“可打印字符”。*/

extern unsigned char _ctype[];
extern char _ctmp;                  ///< 临时辅助变量

#define isalnum(c) ((_ctype+1)[c]&(_U|_L|_D))
#define isalpha(c) ((_ctype+1)[c]&(_U|_L))
#define iscntrl(c) ((_ctype+1)[c]&(_C))
#define isdigit(c) ((_ctype+1)[c]&(_D))
#define isgraph(c) ((_ctype+1)[c]&(_P|_U|_L|_D))
#define islower(c) ((_ctype+1)[c]&(_L))                 /* 判断是否是小写字符。*/
#define isprint(c) ((_ctype+1)[c]&(_P|_U|_L|_D|_SP))
#define ispunct(c) ((_ctype+1)[c]&(_P))
#define isspace(c) ((_ctype+1)[c]&(_S))
#define isupper(c) ((_ctype+1)[c]&(_U))
#define isxdigit(c) ((_ctype+1)[c]&(_D|_X))

#define isascii(c) (((unsigned) c)<=0x7f)
#define toascii(c) (((unsigned) c)&0x7f)

#define tolower(c) (_ctmp=c,isupper(_ctmp)?_ctmp-('A'-'a'):_ctmp)
#define toupper(c) (_ctmp=c,islower(_ctmp)?_ctmp-('a'-'A'):_ctmp)   ///< 小写转大写。_ctmp = c, expr 的值是 expr 的值（先执行逗号左边，再返回右边）。

#endif
