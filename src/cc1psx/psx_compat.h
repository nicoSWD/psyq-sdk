/* Force-included when building the PsyQ GCC 2.8.1 on a 64-bit host.
   The code base calls these pointer-returning functions without a declaration; the implicit
   `int` return type truncates the pointer on LP64. */
#ifndef PSX_COMPAT_H
#define PSX_COMPAT_H
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
union tree_node;
struct rtx_def;
extern struct rtx_def *expand_expr ();
extern struct rtx_def *label_rtx ();
extern struct rtx_def *negate_rtx ();
/* The diagnostic functions are variadic: on arm64 an unprototyped call passes the arguments differently
   from what the variadic definition expects, so every call site needs the prototype. */
extern void error (char *, ...);
extern void fatal (char *, ...);
extern void warning (char *, ...);
extern void pedwarn (char *, ...);
extern void error_with_file_and_line (char *, int, char *, ...);
extern void warning_with_file_and_line (char *, int, char *, ...);
extern void pedwarn_with_file_and_line (char *, int, char *, ...);
extern void error_with_decl (union tree_node *, char *, ...);
extern void warning_with_decl (union tree_node *, char *, ...);
extern void pedwarn_with_decl (union tree_node *, char *, ...);
extern void error_for_asm (struct rtx_def *, char *, ...);
extern void warning_for_asm (struct rtx_def *, char *, ...);

/* qsort with glibc's behaviour (a stable merge sort): ties in GCC's comparison functions then resolve the same way as on
   the Linux host the compiler was developed against. */
static void psx_msort (char *base, char *tmp, unsigned long n, unsigned long sz, int (*cmp) (const void *, const void *))
{
  unsigned long n1, n2, i, j, k;
  if (n <= 1)
    return;
  n1 = n / 2;
  n2 = n - n1;
  psx_msort (base, tmp, n1, sz, cmp);
  psx_msort (base + n1 * sz, tmp, n2, sz, cmp);
  i = 0; j = 0; k = 0;
  while (i < n1 && j < n2)
    {
      if (cmp (base + i * sz, base + (n1 + j) * sz) <= 0)
        memcpy (tmp + (k++) * sz, base + (i++) * sz, sz);
      else
        memcpy (tmp + (k++) * sz, base + (n1 + j++) * sz, sz);
    }
  while (i < n1)
    memcpy (tmp + (k++) * sz, base + (i++) * sz, sz);
  while (j < n2)
    memcpy (tmp + (k++) * sz, base + (n1 + j++) * sz, sz);
  memcpy (base, tmp, n * sz);
}
static void psx_qsort (void *base, unsigned long n, unsigned long sz, int (*cmp) (const void *, const void *))
{
  char *tmp;
  if (n <= 1)
    return;
  tmp = (char *) malloc (n * sz);
  psx_msort ((char *) base, tmp, n, sz, cmp);
  free (tmp);
}
#define qsort psx_qsort
#endif

/* old bison defined YYLEX as the lexer call; bison 3.x does not, and c-parse.y uses it in its actions */
#ifndef YYLEX
#define YYLEX yylex()
#endif
