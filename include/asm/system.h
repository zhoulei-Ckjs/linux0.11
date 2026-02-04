#define move_to_user_mode() \
__asm__ ("movl %%esp,%%eax\n\t" \
    "pushl $0x17\n\t" \
    "pushl %%eax\n\t" \
    "pushfl\n\t" \
    "pushl $0x0f\n\t" \
    "pushl $1f\n\t" \
    "iret\n" \
    "1:\tmovl $0x17,%%eax\n\t" \
    "movw %%ax,%%ds\n\t" \
    "movw %%ax,%%es\n\t" \
    "movw %%ax,%%fs\n\t" \
    "movw %%ax,%%gs" \
    :::"ax")

#define sti() __asm__ ("sti"::)         /* 开中断 */
#define cli() __asm__ ("cli"::)         /* 关中断 */
#define nop() __asm__ ("nop"::)

#define iret() __asm__ ("iret"::)
/* 构造中断描述符 */
#define _set_gate(gate_addr,type,dpl,addr) \            /* &idt[n], 14, 0, addr */ 
__asm__ ("movw %%dx,%%ax\n\t" \                         /* 将 dx 低 16 位给 ax */ 
    "movw %0,%%dx\n\t" \
    "movl %%eax,%1\n\t" \
    "movl %%edx,%2" \
    : \
    : "i" ((short) (0x8000 + (dpl << 13) + (type << 8))), \
    "o" (*((char *) (gate_addr))), \
    "o" (*(4+(char *) (gate_addr))), \
    "d" ((char *) (addr)),"a" (0x00080000))             /* 0x0008 = 0b1_000，其中 1 表示选择子（内核代码段），最后 3 位为属性位（可以查阅段选择子进行了解）*/ 
/* 设置中断门 */
#define set_intr_gate(n,addr) \
    _set_gate(&idt[n], 14, 0, addr)

#define set_trap_gate(n,addr) \
    _set_gate(&idt[n],15,0,addr)

#define set_system_gate(n,addr) \
    _set_gate(&idt[n],15,3,addr)

#define _set_seg_desc(gate_addr,type,dpl,base,limit) {\
    *(gate_addr) = ((base) & 0xff000000) | \
        (((base) & 0x00ff0000)>>16) | \
        ((limit) & 0xf0000) | \
        ((dpl)<<13) | \
        (0x00408000) | \
        ((type)<<8); \
    *((gate_addr)+1) = (((base) & 0x0000ffff)<<16) | \
        ((limit) & 0x0ffff); }

#define _set_tssldt_desc(n,addr,type) \
__asm__ ("movw $104,%1\n\t" \
    "movw %%ax,%2\n\t" \
    "rorl $16,%%eax\n\t" \
    "movb %%al,%3\n\t" \
    "movb $" type ",%4\n\t" \
    "movb $0x00,%5\n\t" \
    "movb %%ah,%6\n\t" \
    "rorl $16,%%eax" \
    ::"a" (addr), "m" (*(n)), "m" (*(n+2)), "m" (*(n+4)), \
     "m" (*(n+5)), "m" (*(n+6)), "m" (*(n+7)) \
    )

#define set_tss_desc(n,addr) _set_tssldt_desc(((char *) (n)),addr,"0x89")
#define set_ldt_desc(n,addr) _set_tssldt_desc(((char *) (n)),addr,"0x82")
