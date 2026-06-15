# llvm-libc coverage of the audited surface (2026-06-10, llvmorg-22.1.7, x86_64-none-elf)

Built by scripts/build-llvm-libc.sh; coverage = surface symbols defined by
libc.a + libm.a. 95 of 152 covered. The 57 uncovered, by migration bucket:

## Phase 3 (stdio File port + wide-stdio stubs)
__fwritex __uflow __overflow __towrite __ofl_add __stdio_close __stdio_seek
__stdio_write fclose fflush fileno fopen fseeko ftello fwide perror ungetc
getwc putwc ungetwc vfwprintf vfwscanf

## Phase 4 (OSv-owned shims: C-locale _l family, iconv, ctype table, env, time)
__strcoll_l __strftime_l __strxfrm_l __towlower_l __towupper_l __wcscoll_l
__wcsftime_l __wcsxfrm_l __wctype_l __iswctype_l iconv iconv_open iconv_close
__ctype_b_loc __ctype_get_mb_cur_max getenv setenv unsetenv time __map_file
__month_to_secs tempnam tmpnam

## Phase 5 (long tail: no llvm-libc equivalent at 22.1.7)
regcomp regexec fnmatch strsignal wcswidth iswdigit iswspace towlower towupper
logl __fpclassifyl __signbitl
