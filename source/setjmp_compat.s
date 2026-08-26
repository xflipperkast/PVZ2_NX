/* setjmp_compat.s -- provide the BSD _setjmp/_longjmp names that the native
 * native lib imports but devkitPro's newlib doesn't export. They are identical
 * to setjmp/longjmp except they don't save/restore the signal mask -- and the
 * Switch has no POSIX signals, so plain setjmp/longjmp are an exact match.
 *
 * We tail-BRANCH (not BL) so the real setjmp saves _setjmp's *caller's* return
 * address; x0 (jmp_buf) and x1 (value) pass straight through untouched.
 *
 * MIT license -- see LICENSE.
 */
    .text

    .global _setjmp
    .type   _setjmp, %function
_setjmp:
    b       setjmp

    .global _longjmp
    .type   _longjmp, %function
_longjmp:
    b       longjmp
