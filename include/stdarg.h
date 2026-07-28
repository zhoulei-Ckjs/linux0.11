#ifndef _STDARG_H
#define _STDARG_H

typedef char *va_list;  ///< 可变参

/// @brief 计算类型占用字节数
#define __va_rounded_size(TYPE)  \
  (((sizeof (TYPE) + sizeof (int) - 1) / sizeof (int)) * sizeof (int))

/// @brief
/// @note sparc 为一种架构，由 Oracle 支持，应用于高端企业级，或嵌入式领域
#ifndef __sparc__
    #define va_start(AP, LASTARG) 						\
    (AP = ((char *) &(LASTARG) + __va_rounded_size (LASTARG)))
#else
    #define va_start(AP, LASTARG) 						\
    (__builtin_saveregs (),						\
      AP = ((char *) &(LASTARG) + __va_rounded_size (LASTARG)))
#endif

void va_end (va_list);		/* Defined in gnulib */
#define va_end(AP)

#define va_arg(AP, TYPE)						\
 (AP += __va_rounded_size (TYPE),					\
  *((TYPE *) (AP - __va_rounded_size (TYPE))))

#endif /* _STDARG_H */
