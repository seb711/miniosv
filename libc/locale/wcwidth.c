/*
 * wcwidth / wcswidth - column width of wide characters.
 *
 * OSv's baremetal libc (llvm-libc) does not ship these, but the vendored
 * tabulate table-printer (external/tabulate, used by LeanStore's profiling
 * output) calls wcswidth(). This is a compact, locale-independent
 * implementation: control characters are non-printable (-1), the common
 * double-width CJK/fullwidth ranges count as 2 columns, everything else 1.
 * Combining marks are treated as width 1 (we do not carry Unicode tables).
 */
#include <wchar.h>

int wcwidth(wchar_t wc)
{
   if (wc == 0)
      return 0;
   /* C0/C1 control characters have no printable width. */
   if (wc < 32 || (wc >= 0x7f && wc < 0xa0))
      return -1;
   /* Common double-width ranges (East Asian Wide / Fullwidth). */
   if ((wc >= 0x1100 && wc <= 0x115f) ||  /* Hangul Jamo */
       wc == 0x2329 || wc == 0x232a ||
       (wc >= 0x2e80 && wc <= 0xa4cf && wc != 0x303f) ||  /* CJK .. Yi */
       (wc >= 0xac00 && wc <= 0xd7a3) ||  /* Hangul Syllables */
       (wc >= 0xf900 && wc <= 0xfaff) ||  /* CJK Compatibility Ideographs */
       (wc >= 0xfe30 && wc <= 0xfe4f) ||  /* CJK Compatibility Forms */
       (wc >= 0xff00 && wc <= 0xff60) ||  /* Fullwidth Forms */
       (wc >= 0xffe0 && wc <= 0xffe6) ||
       (wc >= 0x20000 && wc <= 0x3fffd))  /* CJK Ext. B and beyond */
      return 2;
   return 1;
}

int wcswidth(const wchar_t *s, size_t n)
{
   int width = 0;
   for (; n-- > 0 && *s != 0; s++) {
      int w = wcwidth(*s);
      if (w < 0)
         return -1;
      width += w;
   }
   return width;
}
