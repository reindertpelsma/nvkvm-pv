#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
/* The validator under test, extracted verbatim by the Makefile rule so the
 * test cannot drift from the code it is testing. */
#include "utf8_extracted.inc"
static int fails;
static void T(const char *name, const char *p, unsigned len, bool want) {
    bool got = nb_utf8_ok(p, len);
    if (got != want) { printf("FAIL %-28s want=%d got=%d\n", name, want, got); fails++; }
    else printf("ok   %-28s %s\n", name, want ? "accepted" : "rejected");
}
int main(void) {
    T("ascii",              "hello", 5, true);
    T("empty",              "", 0, true);
    T("2-byte u+00e9",      "\xc3\xa9", 2, true);
    T("3-byte u+20ac",      "\xe2\x82\xac", 3, true);
    T("4-byte u+1f600",     "\xf0\x9f\x98\x80", 4, true);
    T("embedded NUL",       "a\x00b", 3, false);
    T("lone continuation",  "\x80", 1, false);
    T("truncated 2-byte",   "\xc3", 1, false);
    T("truncated 3-byte",   "\xe2\x82", 2, false);
    T("truncated 4-byte",   "\xf0\x9f\x98", 3, false);
    T("overlong 2-byte /",  "\xc0\xaf", 2, false);
    T("overlong 3-byte",    "\xe0\x80\xaf", 3, false);
    T("overlong 4-byte",    "\xf0\x80\x80\xaf", 4, false);
    T("surrogate d800",     "\xed\xa0\x80", 3, false);
    T("surrogate dfff",     "\xed\xbf\xbf", 3, false);
    T("above u+10ffff",     "\xf4\x90\x80\x80", 4, false);
    T("5-byte form",        "\xf8\x88\x80\x80\x80", 5, false);
    T("0xfe",               "\xfe", 1, false);
    T("bad continuation",   "\xc3\x28", 2, false);
    T("valid then trunc",   "ok\xe2\x82", 4, false);
    printf(fails ? "\n%d FAILURES\n" : "\nall passed\n", fails);
    return fails != 0;
}
