/* brine runtime — public header included by generated code.
 * Keep this header free of platform headers (windows.h etc.) so user
 * identifiers can never collide with platform macros. */
#ifndef BRINE_RT_H
#define BRINE_RT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <math.h>

/* a*b+c rounds twice, as written, on every target: no fused multiply-adds
 * (the driver also passes -ffp-contract=off; this covers a C compiler run by
 * hand on the output of `brine emit-c`) */
#ifdef __clang__
#pragma STDC FP_CONTRACT OFF
#endif

#if defined(__GNUC__) || defined(__clang__)
#define BR_NORETURN __attribute__((noreturn))
#define BR_LIKELY(x) __builtin_expect(!!(x), 1)
#define BR_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define BR_NORETURN _Noreturn
#define BR_LIKELY(x) (x)
#define BR_UNLIKELY(x) (x)
#endif

/* ------------------------------------------------------------ core types */
/* Generated code lays memory out as on a 64-bit machine: a pointer takes 8
 * bytes, in a struct field or an array element. Where pointers are 4 bytes
 * (wasm32), a pointer field is the low half of an 8-byte slot (BR_P), and
 * the runtime keeps pointer elements in BR_PSLOT-byte slots. */
#if UINTPTR_MAX == 0xffffffffu
#define BR_P(T, name) union { T name; uint64_t name##_slot; }
#else
#define BR_P(T, name) T name
#endif
#define BR_PSLOT 8
/* load and store a pointer slot, and a local variable that is one (generated code) */
#if UINTPTR_MAX == 0xffffffffu
#define BR_LDP(at) ((void *)(uintptr_t) * (uint64_t *)(at))
#define BR_STP(at, v) (*(uint64_t *)(at) = (uint64_t)(uintptr_t)(v))
typedef uint64_t brine_pslot;
#else
#define BR_LDP(at) (*(void **)(at))
#define BR_STP(at, v) (*(void **)(at) = (v))
typedef void *brine_pslot;
#endif
/* a word of a table that holds pointers (generated code) */
typedef union {
    void *p;
    uint64_t u;
} brine_word;
typedef struct brine_str {
    int64_t rc; /* < 0 means immortal (string literal) */
    int64_t len;
    char data[];
} brine_str;

typedef void (*brine_elem_fn)(void *);

typedef struct brine_arr {
    int64_t rc;
    int64_t len;
    int64_t cap;
    int64_t esz;
    BR_P(brine_elem_fn, retain); /* NULL for plain-data elements */
    BR_P(brine_elem_fn, release);
    BR_P(char *, data);
} brine_arr;

/* What a map needs to know about its key and value types (one per map type). */
typedef struct brine_sb brine_sb;
/* `()` when stored in a variable or field */
typedef uint8_t brine_unit;
/* 128-bit integers (a GCC/Clang extension, on 64-bit targets) */
__extension__ typedef __int128 brine_i128;
__extension__ typedef unsigned __int128 brine_u128;
#define BR_U128_MAX (~(brine_u128)0)
#define BR_I128_MAX ((brine_i128)(BR_U128_MAX >> 1))
#define BR_I128_MIN (-BR_I128_MAX - 1)
typedef uint64_t (*brine_hash_fn)(const void *k);
typedef bool (*brine_eq_fn)(const void *a, const void *b);
typedef void (*brine_fmt_fn)(brine_sb *sb, const void *k);
typedef struct brine_map_ty {
    int64_t ksz, vsz;
    BR_P(brine_hash_fn, hash);
    BR_P(brine_eq_fn, keq);
    BR_P(brine_eq_fn, veq); /* NULL: values can't be compared */
    BR_P(brine_fmt_fn, kfmt);
    /* NULL for plain data */
    BR_P(brine_elem_fn, kretain);
    BR_P(brine_elem_fn, krelease);
    BR_P(brine_elem_fn, vretain);
    BR_P(brine_elem_fn, vrelease);
} brine_map_ty;

/* Hash map that remembers insertion order. Entries live in `keys`/`vals`
 * (slots [0, used)); `index` is an open-addressing table of entry numbers. */
typedef struct brine_map {
    int64_t rc;
    int64_t len;  /* live entries */
    int64_t used; /* entry slots used, including removed ones */
    int64_t cap;  /* entry slots allocated */
    int64_t nidx; /* index slots: 0 or a power of two >= 2 * cap */
    BR_P(const brine_map_ty *, ty);
    BR_P(char *, keys);
    BR_P(char *, vals);
    BR_P(uint64_t *, hashes);
    BR_P(bool *, live);
    BR_P(int64_t *, index); /* -1 empty, -2 removed, else an entry number */
} brine_map;

/* header shared by every enum box (enums whose variants carry data) */
typedef struct brine_box {
    int64_t rc; /* < 0 means immortal (variants without fields) */
    int64_t tag;
} brine_box;

struct brine_sb {
    BR_P(char *, p);
    int64_t len, cap;
};

/* ------------------------------------------------------------ panics */
BR_NORETURN void brine_panic_at(const char *loc, const char *msg);
BR_NORETURN void brine_panic_idx(int64_t i, int64_t len, const char *loc);
BR_NORETURN void brine_panic_ovf(const char *op, const char *loc);
BR_NORETURN void brine_panic_str(const char *loc, brine_str *msg);
BR_NORETURN void brine_assert_fail(const char *loc, const char *src, brine_str *msg);
BR_NORETURN void brine_exit(int64_t code);
BR_NORETURN void brine_unreachable(void);

/* ------------------------------------------------------------ memory */
void *brine_alloc(int64_t n);
void brine_free(void *p);
void brine_str_free(brine_str *s);
void brine_arr_free(brine_arr *a);

static inline void brine_str_retain(brine_str *s) {
    if (__atomic_load_n(&s->rc, __ATOMIC_RELAXED) >= 0) __atomic_fetch_add(&s->rc, 1, __ATOMIC_RELAXED);
}
/* A count of 1 is our own reference: no other thread holds one to change it,
   so the last release frees without a locked instruction. */
static inline void brine_str_release(brine_str *s) {
    int64_t rc = __atomic_load_n(&s->rc, __ATOMIC_ACQUIRE);
    if (rc < 0) return;
    if (rc == 1 || __atomic_sub_fetch(&s->rc, 1, __ATOMIC_ACQ_REL) == 0) brine_str_free(s);
}
static inline void brine_arr_retain(brine_arr *a) { __atomic_fetch_add(&a->rc, 1, __ATOMIC_RELAXED); }
static inline void brine_arr_release(brine_arr *a) {
    if (__atomic_load_n(&a->rc, __ATOMIC_ACQUIRE) == 1 || __atomic_sub_fetch(&a->rc, 1, __ATOMIC_ACQ_REL) == 0)
        brine_arr_free(a);
}
void *brine_box_new(int64_t size, int64_t tag);
void brine_box_free(void *p);
/* fn(p) finishes freeing the dead object p; deep structures go iteratively */
void brine_drop(void *p, void (*fn)(void *));
static inline void brine_box_retain(void *p) {
    brine_box *b = (brine_box *)p;
    if (__atomic_load_n(&b->rc, __ATOMIC_RELAXED) >= 0) __atomic_fetch_add(&b->rc, 1, __ATOMIC_RELAXED);
}
/* drops one reference; true when the box must be freed */
static inline bool brine_box_dec(void *p) {
    brine_box *b = (brine_box *)p;
    int64_t rc = __atomic_load_n(&b->rc, __ATOMIC_ACQUIRE);
    if (rc < 0) return false;
    return rc == 1 || __atomic_sub_fetch(&b->rc, 1, __ATOMIC_ACQ_REL) == 0;
}
void brine_rt_str(void *p);
void brine_rl_str(void *p);
void brine_rt_arr(void *p);
void brine_rl_arr(void *p);
void brine_map_free(brine_map *m);
static inline void brine_map_retain(brine_map *m) { __atomic_fetch_add(&m->rc, 1, __ATOMIC_RELAXED); }
static inline void brine_map_release(brine_map *m) {
    if (__atomic_sub_fetch(&m->rc, 1, __ATOMIC_ACQ_REL) == 0) brine_map_free(m);
}
void brine_rt_map(void *p);
void brine_rl_map(void *p);

/* ------------------------------------------------------------ strings */
brine_str *brine_str_new(const char *p, int64_t n);
brine_str *brine_str_concat(brine_str *a, brine_str *b);
void brine_str_append(brine_str **ps, brine_str *b);
/* The bytes allocated for a string of length n, rounded up (by at most a
   quarter) so `+=` can usually grow it in place. Every heap string is at least
   this big for its length. */
static inline size_t brine_str_size(int64_t n) {
    size_t t = sizeof(brine_str) + (size_t)n + 1;
    if (t <= 64) return (t + 15) & ~(size_t)15;
    size_t g = (size_t)1 << (61 - __builtin_clzll(t));
    return (t + g - 1) & ~(g - 1);
}
/* The longest string the block of a string of length n holds. */
static inline int64_t brine_str_room(int64_t n) {
    if (n <= 47) return (int64_t)(((uint64_t)n + 32) & ~(uint64_t)15) - 17;
    return (int64_t)brine_str_size(n) - 17;
}
/* A string's bytes start 8-byte aligned, and every string (heap blocks by
   brine_str_size, literals by padding) can be read to the end of the 8-byte
   word that holds its terminating NUL. So short strings are read, compared and
   hashed a word at a time, with the bytes past the end masked off. All
   targets are little-endian. */
static inline uint64_t brine_ld8(const char *p) {
    uint64_t v;
    __builtin_memcpy(&v, p, 8);
    return v;
}
static inline void brine_st8(char *p, uint64_t v) { __builtin_memcpy(p, &v, 8); }
/* the low n bytes of a word, 0 <= n <= 8 */
static inline uint64_t brine_lowmask(uint64_t n) { return n >= 8 ? ~0ULL : ~(~0ULL << (n * 8)); }
/* small copies inline: a memcpy of a length known only at run time is a library call */
static inline void brine_copy(char *d, const char *s, int64_t n) {
    if (n > 16) {
        __builtin_memcpy(d, s, (size_t)n);
        return;
    }
    for (int64_t i = 0; i < n; i++) d[i] = s[i];
}
/* `s += t`: in place when s is unshared and its block has room */
static inline void brine_str_add(brine_str **ps, brine_str *b) {
    brine_str *a = *ps;
    int64_t la = a->len, lb = b->len, n = la + lb;
    if (BR_LIKELY(__atomic_load_n(&a->rc, __ATOMIC_ACQUIRE) == 1 && n <= brine_str_room(la))) {
        brine_copy(a->data + la, b->data, lb);
        a->data[n] = 0;
        a->len = n;
        return;
    }
    brine_str_append(ps, b);
}
void brine_str_add_byte_slow(brine_str **ps, uint8_t c);
/* `s += t[i]` and `s += "c"`: one byte, in place when s is unshared and has
   room. Within the first word the whole word is rewritten: a later word load
   (hashing, comparing) then forwards from one store instead of waiting for
   several byte stores to retire. */
static inline void brine_str_add_byte(brine_str **ps, uint8_t c) {
    brine_str *a = *ps;
    int64_t la = a->len;
    if (BR_LIKELY(__atomic_load_n(&a->rc, __ATOMIC_ACQUIRE) == 1 && la < brine_str_room(la))) {
        if (la < 8) {
            uint64_t sh = (uint64_t)la * 8;
            brine_st8(a->data, (brine_ld8(a->data) & ~(~0ULL << sh)) | ((uint64_t)c << sh));
            if (la == 7) a->data[8] = 0;
        } else {
            a->data[la] = (char)c;
            a->data[la + 1] = 0;
        }
        a->len = la + 1;
        return;
    }
    brine_str_add_byte_slow(ps, c);
}
/* `var s = lit` run again (a loop body's variable): keeps s's block when
   nothing else holds it, instead of freeing it here and allocating again at
   the next `+=`. lit is a literal, so it needs no reference. */
static inline void brine_str_reset(brine_str **ps, brine_str *lit) {
    brine_str *a = *ps;
    int64_t n = lit->len;
    if (__atomic_load_n(&a->rc, __ATOMIC_ACQUIRE) == 1 && n <= brine_str_room(a->len)) {
        /* a short literal goes in as one word store, so the next append's
           word load forwards from it */
        if (n < 8)
            brine_st8(a->data, brine_ld8(lit->data) & brine_lowmask((uint64_t)n));
        else {
            brine_copy(a->data, lit->data, n);
            a->data[n] = 0;
        }
        a->len = n;
        return;
    }
    brine_str_release(a);
    *ps = lit;
}
/* A loop whose only use of string variable s is `s += t[i]` (or a one-byte
   literal) appends through an appender: the length and the first 8 bytes
   stay in the appender, which the C compiler keeps in registers, instead of
   going through s's block on every byte. Nothing reads s during the loop;
   brine_app_close writes the length back when it ends. While s is unshared,
   bytes go straight into its block (the first word, `w0`, at the end), up
   to `room`, and `lim` is the end of the bytes `w0` takes; when s is shared,
   room is -1 and the first byte makes it a block of its own. */
typedef struct {
    char *data;
    int64_t len, room, lim;
    uint64_t w0;
} brine_app;
typedef struct {
    brine_str *s;
    int64_t len, room, lim;
    uint64_t w0;
} brine_app_st;
brine_app_st brine_app_more(brine_str *a, int64_t n, int64_t room, uint64_t w0, uint8_t c);
static inline void brine_app_open(brine_str **ps, void *ap) {
    brine_app *p = (brine_app *)ap;
    brine_str *a = *ps;
    int64_t n = a->len;
    int64_t room = __atomic_load_n(&a->rc, __ATOMIC_ACQUIRE) == 1 ? brine_str_room(n) : -1;
    p->data = a->data;
    p->len = n;
    p->room = room;
    p->lim = room < 8 ? room : 8;
    p->w0 = brine_ld8(a->data) & brine_lowmask((uint64_t)n);
}
static inline void brine_app_byte(brine_str **ps, void *ap, uint8_t c) {
    brine_app *p = (brine_app *)ap;
    int64_t n = p->len;
    if (BR_LIKELY(n < p->lim)) {
        p->w0 |= (uint64_t)c << (8 * n);
        p->len = n + 1;
        return;
    }
    if (BR_LIKELY(n > 8 && n < p->room)) {
        p->data[n] = (char)c;
        p->len = n + 1;
        return;
    }
    /* by value, so the appender's address doesn't escape */
    brine_app_st x = brine_app_more(*ps, n, p->room, p->w0, c);
    *ps = x.s;
    p->data = x.s->data;
    p->len = x.len;
    p->room = x.room;
    p->lim = x.lim;
    p->w0 = x.w0;
}
static inline void brine_app_close(brine_str **ps, void *ap) {
    brine_app *p = (brine_app *)ap;
    brine_str *a = *ps;
    int64_t n = p->len;
    if (a->len == n) return; /* nothing appended */
    a->len = n;
    /* the bytes past n in w0 are 0: the NUL */
    if (n <= 8) brine_st8(p->data, p->w0);
    if (n >= 8) p->data[n] = 0;
}
bool brine_str_eq_long(brine_str *a, brine_str *b);
static inline bool brine_str_eq(brine_str *a, brine_str *b) {
    if (a == b) return true;
    uint64_t n = (uint64_t)a->len;
    if (n != (uint64_t)b->len) return false;
    if (BR_LIKELY(n <= 8)) return ((brine_ld8(a->data) ^ brine_ld8(b->data)) & brine_lowmask(n)) == 0;
    return brine_str_eq_long(a, b);
}
bool brine_str_contains(brine_str *s, brine_str *sub);
/* `s[i]`: the byte at i as a one-character string, from a table of immortal
   ones so indexing never allocates */
typedef struct {
    int64_t rc, len;
    char d[2];
} brine_str1;
extern brine_str1 brine_bytes[256];
static inline brine_str *brine_str_byte(brine_str *s, int64_t i) { return (brine_str *)&brine_bytes[(unsigned char)s->data[i]]; }
static inline brine_str *brine_str_at(brine_str *s, int64_t i, const char *loc) {
    if (BR_UNLIKELY((uint64_t)i >= (uint64_t)s->len)) brine_panic_idx(i, s->len, loc);
    return brine_str_byte(s, i);
}
brine_str *brine_str_slice(brine_str *s, int64_t lo, int64_t hi, const char *loc);
int64_t brine_ord(brine_str *s, const char *loc);
brine_str *brine_chr(int64_t c, const char *loc);
/* string methods: `s.name(args)` is brine_s_name(s, args, loc) */
int64_t brine_s_find(brine_str *s, brine_str *sub, const char *loc);
brine_arr *brine_s_split(brine_str *s, brine_str *sep, const char *loc);
brine_arr *brine_s_split_ws(brine_str *s, const char *loc);
brine_str *brine_s_join(brine_str *sep, brine_arr *parts, const char *loc);
bool brine_s_starts_with(brine_str *s, brine_str *p, const char *loc);
bool brine_s_ends_with(brine_str *s, brine_str *p, const char *loc);
brine_str *brine_s_strip(brine_str *s, const char *loc);
brine_str *brine_s_lstrip(brine_str *s, const char *loc);
brine_str *brine_s_rstrip(brine_str *s, const char *loc);
brine_str *brine_s_replace(brine_str *s, brine_str *a, brine_str *b, const char *loc);
brine_str *brine_s_upper(brine_str *s, const char *loc);
brine_str *brine_s_lower(brine_str *s, const char *loc);
brine_arr *brine_s_lines(brine_str *s, const char *loc);
bool brine_s_is_digit(brine_str *s, const char *loc);
bool brine_s_is_alpha(brine_str *s, const char *loc);
bool brine_s_is_space(brine_str *s, const char *loc);
bool brine_s_is_alnum(brine_str *s, const char *loc);
int64_t brine_s_count(brine_str *s, brine_str *sub, const char *loc);
brine_arr *brine_s_bytes(brine_str *s, const char *loc);
brine_arr *brine_s_chars(brine_str *s, const char *loc);
brine_str *brine_s_repeat(brine_str *s, int64_t n, const char *loc);
int brine_str_cmp(brine_str *a, brine_str *b);
int64_t brine_parse_i64(brine_str *s, const char *loc);
brine_i128 brine_parse_i128(brine_str *s, const char *loc);
brine_u128 brine_parse_u128(brine_str *s, const char *loc);
double brine_parse_f64(brine_str *s, const char *loc);

/* ------------------------------------------------------------ arrays */
brine_arr *brine_arr_new(int64_t len, int64_t esz, brine_elem_fn rt, brine_elem_fn rl, const char *loc);
brine_arr *brine_bits_new(int64_t len, int64_t fill, const char *loc);
brine_arr *brine_bytes_new(int64_t len, const char *loc);
void brine_arr_unique(brine_arr **pa);

/* Loops the compiler recognizes (idiom.br), done in one step when every
   element they touch is in bounds. They return false, having changed
   nothing, when the caller must run the loop as written instead (so a panic
   happens where it would have). */
static inline bool br_arr_mine(brine_arr **pa) {
    if (BR_UNLIKELY(__atomic_load_n(&(*pa)->rc, __ATOMIC_ACQUIRE) != 1)) brine_arr_unique(pa);
    return true;
}
/* for i in lo..hi: a[i] = a[i + 1] */
static inline bool brine_arr_shl(brine_arr **pa, int64_t lo, int64_t hi) {
    if (lo >= hi) return true;
    if (lo < 0 || hi >= (*pa)->len || (*pa)->esz == 0) return false;
    br_arr_mine(pa);
    brine_arr *a = *pa;
    int64_t e = a->esz;
    __builtin_memmove(a->data + lo * e, a->data + (lo + 1) * e, (size_t)((hi - lo) * e));
    return true;
}
/* for i in lo..hi: b[i] = a[i], b and a different variables (so different
   arrays once b is unique) */
static inline bool brine_arr_copy(brine_arr **pb, brine_arr *a, int64_t lo, int64_t hi) {
    if (lo >= hi) return true;
    if (lo < 0 || hi > a->len || hi > (*pb)->len || a->esz != (*pb)->esz || a->esz == 0) return false;
    br_arr_mine(pb);
    brine_arr *b = *pb;
    int64_t e = a->esz;
    __builtin_memcpy(b->data + lo * e, a->data + lo * e, (size_t)((hi - lo) * e));
    return true;
}
/* x = a[lo]; for i in lo..hi: a[i] = a[i + 1]; a[hi] = x */
static inline bool brine_arr_rotl(brine_arr **pa, int64_t lo, int64_t hi) {
    if (lo > hi || lo < 0 || hi >= (*pa)->len || (*pa)->esz == 0 || (*pa)->esz > 64) return false;
    br_arr_mine(pa);
    brine_arr *a = *pa;
    int64_t e = a->esz;
    char x[64];
    __builtin_memcpy(x, a->data + lo * e, (size_t)e);
    __builtin_memmove(a->data + lo * e, a->data + (lo + 1) * e, (size_t)((hi - lo) * e));
    __builtin_memcpy(a->data + hi * e, x, (size_t)e);
    return true;
}

/* The same on byte arrays (narrow.br): never shared, with 16 bytes to spare
   after the last element (brine_bytes_new), so these work on 16 bytes in a
   pair of registers. Data `d`, length `n`. */
typedef unsigned __int128 br_u128x;
/* the low n bytes set, n in 0..=16: a table, as a variable 128-bit shift is
   a chain of dependent instructions */
static const uint64_t br_low_tab[17][2] = {
    {0, 0}, {0xff, 0}, {0xffff, 0}, {0xffffff, 0}, {0xffffffffULL, 0}, {0xffffffffffULL, 0},
    {0xffffffffffffULL, 0}, {0xffffffffffffffULL, 0}, {~0ULL, 0}, {~0ULL, 0xff}, {~0ULL, 0xffff},
    {~0ULL, 0xffffff}, {~0ULL, 0xffffffffULL}, {~0ULL, 0xffffffffffULL}, {~0ULL, 0xffffffffffffULL},
    {~0ULL, 0xffffffffffffffULL}, {~0ULL, ~0ULL}};
static inline br_u128x br_low_bytes(int64_t n) { return ((br_u128x)br_low_tab[n][1] << 64) | br_low_tab[n][0]; }
static inline br_u128x br_bswap128(br_u128x x) {
    return ((br_u128x)__builtin_bswap64((uint64_t)x) << 64) | __builtin_bswap64((uint64_t)(x >> 64));
}
/* as two 8-byte halves, so a load that follows a store gets it forwarded */
static inline br_u128x br_ld128(const char *p) {
    uint64_t a, b;
    __builtin_memcpy(&a, p, 8);
    __builtin_memcpy(&b, p + 8, 8);
    return ((br_u128x)b << 64) | a;
}
static inline void br_st128(char *p, br_u128x x) {
    uint64_t a = (uint64_t)x, b = (uint64_t)(x >> 64);
    __builtin_memcpy(p, &a, 8);
    __builtin_memcpy(p + 8, &b, 8);
}
/* while i < j: a[i], a[j] = a[j], a[i]; i += 1; j -= 1. Only 16 bytes at
   most: longer ones gain little over the loop, and a loop around this one
   then holds nothing else, so the C compiler can keep the bytes in registers. */
static inline bool brine_bytes_rev(char *d, int64_t n, int64_t *pi, int64_t *pj) {
    int64_t i = *pi, j = *pj;
    if (i >= j) return true;
    if (i < 0 || j >= n || j - i >= 16) return false;
    /* bytes i..j reversed in place, the rest of the 16 kept */
    int64_t k = j - i;
    br_u128x x = br_ld128(d + i);
    br_u128x m = br_low_bytes(k + 1);
    br_st128(d + i, ((br_bswap128(x) >> (8 * (15 - k))) & m) | (x & ~m));
    *pi = i + (k + 1) / 2;
    *pj = j - (k + 1) / 2;
    return true;
}
/* x = a[lo]; for i in lo..hi: a[i] = a[i + 1]; a[hi] = x */
static inline bool brine_bytes_rotl(char *d, int64_t n, int64_t lo, int64_t hi) {
    if (lo > hi || lo < 0 || hi >= n) return false;
    int64_t k = hi - lo;
    if (k < 16) {
        br_u128x x = br_ld128(d + lo);
        br_u128x rot = ((x >> 8) & br_low_bytes(k)) | ((x & 0xff) << (8 * k));
        br_u128x m = br_low_bytes(k + 1);
        br_st128(d + lo, (rot & m) | (x & ~m));
    } else {
        char x = d[lo];
        __builtin_memmove(d + lo, d + lo + 1, (size_t)k);
        d[hi] = x;
    }
    return true;
}
static inline bool brine_bytes_shl(char *d, int64_t n, int64_t lo, int64_t hi) {
    if (lo >= hi) return true;
    if (lo < 0 || hi >= n) return false;
    if (hi - lo < 16) {
        br_u128x x = br_ld128(d + lo);
        br_u128x m = br_low_bytes(hi - lo);
        br_st128(d + lo, ((x >> 8) & m) | (x & ~m));
    } else
        __builtin_memmove(d + lo, d + lo + 1, (size_t)(hi - lo));
    return true;
}
static inline bool brine_bytes_copy(char *db, int64_t nb, const char *da, int64_t na, int64_t lo, int64_t hi) {
    if (lo >= hi) return true;
    if (lo < 0 || hi > na || hi > nb) return false;
    if (hi == nb && hi - lo <= 16)
        /* whole 16 bytes (past the end of b is spare), so reads of b right
           after get the stores forwarded */
        br_st128(db + lo, br_ld128(da + lo));
    else
        __builtin_memcpy(db + lo, da + lo, (size_t)(hi - lo));
    return true;
}
void brine_arr_push(brine_arr **pa, const void *elem);
void brine_arr_pop(brine_arr **pa, void *out, const char *loc);
void brine_arr_pop_at(brine_arr **pa, int64_t i, void *out, const char *loc);
/* takes ownership of *elem */
void brine_arr_insert(brine_arr **pa, int64_t i, const void *elem, const char *loc);
void brine_arr_extend(brine_arr **pa, brine_arr *b);
void brine_arr_reverse(brine_arr **pa);
void brine_arr_clear(brine_arr **pa);
/* push, pop and clear with their common case inline: the array is this
   variable's alone and, for a push, has room. `esz` is the element size, a
   constant the compiler passes so the copy becomes a store. */
static inline void brine_arr_push_in(brine_arr **pa, const void *elem, int64_t esz) {
    brine_arr *a = *pa;
    if (BR_LIKELY(a->len < a->cap && __atomic_load_n(&a->rc, __ATOMIC_ACQUIRE) == 1)) {
        __builtin_memcpy(a->data + a->len * esz, elem, (size_t)esz);
        a->len++;
        return;
    }
    brine_arr_push(pa, elem);
}
static inline void brine_arr_pop_in(brine_arr **pa, void *out, int64_t esz, const char *loc) {
    brine_arr *a = *pa;
    if (BR_LIKELY(a->len > 0 && __atomic_load_n(&a->rc, __ATOMIC_ACQUIRE) == 1)) {
        a->len--;
        __builtin_memcpy(out, a->data + a->len * esz, (size_t)esz);
        return;
    }
    brine_arr_pop(pa, out, loc);
}
static inline void brine_arr_clear_in(brine_arr **pa) {
    brine_arr *a = *pa;
    if (BR_LIKELY(a->release == NULL && __atomic_load_n(&a->rc, __ATOMIC_ACQUIRE) == 1)) {
        a->len = 0;
        return;
    }
    brine_arr_clear(pa);
}
/* stable sort; cmp returns <0, 0 or >0 */
void brine_arr_sort(brine_arr **pa, int (*cmp)(const void *, const void *));
brine_arr *brine_arr_slice(brine_arr *a, int64_t lo, int64_t hi, const char *loc);

/* ------------------------------------------------------------ maps */
brine_map *brine_map_new(const brine_map_ty *ty);
void brine_map_unique(brine_map **pm);
int64_t brine_map_find(const brine_map *m, const void *k);
/* the value for `k`; panics if missing */
void *brine_map_at(brine_map *m, const void *k, const char *loc);
/* same, after making the map unique so the value can be changed */
void *brine_map_at_mut(brine_map **pm, const void *k, const char *loc);
/* takes ownership of *k and *v */
void brine_map_set(brine_map **pm, void *k, void *v);
bool brine_map_remove(brine_map **pm, const void *k);
brine_arr *brine_map_keys(brine_map *m);
brine_arr *brine_map_values(brine_map *m);
bool brine_map_eq(brine_map *a, brine_map *b);
static inline bool brine_map_has(brine_map *m, const void *k) { return brine_map_find(m, k) >= 0; }

/* hashing helpers for generated hash functions */
static inline uint64_t brine_hash_u64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}
static inline uint64_t brine_hash_mix(uint64_t h, uint64_t x) { return brine_hash_u64(h ^ (x + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2))); }
static inline uint64_t brine_hash_u128(brine_u128 x) { return brine_hash_mix(brine_hash_u64((uint64_t)x), (uint64_t)(x >> 64)); }
/* A one-to-one mix for string hashes: an odd multiply carries every bit
   upwards, and the byte swap brings the top bits, which depend on all of
   the word, down to the bits a map's index uses. Cheaper than
   brine_hash_u64, which matters for short keys. */
static inline uint64_t brine_hash_word(uint64_t x) { return __builtin_bswap64(x * 0x9e3779b97f4a7c15ULL); }
uint64_t brine_hash_long(const char *p, uint64_t n);
/* A string of up to 7 bytes is its bytes and its length in one word, mixed by
   brine_hash_word, a bijection: two such strings hash equally only if they
   are equal. Longer strings hash to words with the top bit set before the
   same mixing, so they never hash like a short one. A map can then take
   equal hashes of short keys as equal keys (brine_map_probe). */
static inline uint64_t brine_hash_str(brine_str *s) {
    uint64_t n = (uint64_t)s->len;
    if (BR_LIKELY(n < 8)) return brine_hash_word((brine_ld8(s->data) & brine_lowmask(n)) | n << 56);
    return brine_hash_long(s->data, n);
}
/* Entry number of key k (hash h) in m, or -1. keq compares two keys of ksz
   bytes; `exact` says h was made by a hash that is one-to-one for k (see
   brine_hash_str), so an equal hash is an equal key. The C backend inlines
   this with keq known. */
static inline __attribute__((always_inline)) int64_t brine_map_probe(const brine_map *m, const void *k, uint64_t h, void *keqp,
                                                                     int64_t ksz, bool exact) {
    brine_eq_fn keq = (brine_eq_fn)keqp;
    if (m->len == 0) return -1;
    uint64_t mask = (uint64_t)(m->nidx - 1);
    const int64_t *ix = m->index;
    const uint64_t *hs = m->hashes;
    for (uint64_t i = h & mask;; i = (i + 1) & mask) {
        int64_t e = ix[i];
        if (e == -1) return -1;
        if (e >= 0 && hs[e] == h && (exact || keq(m->keys + e * ksz, k))) return e;
    }
}
/* the value of entry e (from brine_map_probe; -1 panics for the missing key k) */
static inline void *brine_map_val(brine_map *m, int64_t e, const void *k, int64_t vsz, const char *loc) {
    if (BR_UNLIKELY(e < 0)) return brine_map_at(m, k, loc);
    return m->vals + e * vsz;
}
void *brine_map_val_mut_slow(brine_map **pm, int64_t e, const void *k, const char *loc);
/* same, after making the map unique so the value can be changed */
static inline void *brine_map_val_mut(brine_map **pm, int64_t e, const void *k, int64_t vsz, const char *loc) {
    brine_map *m = *pm;
    if (BR_LIKELY(e >= 0 && __atomic_load_n(&m->rc, __ATOMIC_ACQUIRE) == 1)) return m->vals + e * vsz;
    return brine_map_val_mut_slow(pm, e, k, loc);
}

static inline int64_t brine_idx(int64_t i, int64_t len, const char *loc) {
    if (BR_UNLIKELY((uint64_t)i >= (uint64_t)len)) brine_panic_idx(i, len, loc);
    return i;
}

/* ------------------------------------------------------------ string builder */
void brine_sb_init(brine_sb *sb);
void brine_sb_cat(brine_sb *sb, const char *p, int64_t n);
void brine_sb_str(brine_sb *sb, brine_str *s);
void brine_sb_repr_str(brine_sb *sb, brine_str *s);
void brine_sb_i64(brine_sb *sb, int64_t v);
void brine_sb_u64(brine_sb *sb, uint64_t v);
void brine_sb_i128(brine_sb *sb, brine_i128 v);
void brine_sb_u128(brine_sb *sb, brine_u128 v);
void brine_sb_f64(brine_sb *sb, double v);
void brine_sb_f32(brine_sb *sb, float v);
void brine_sb_bool(brine_sb *sb, bool v);
/* formatted: kind is 0, 'f', 'e', 'x', 'b'; prec < 0 means default */
void brine_sb_fmt_i64(brine_sb *sb, int64_t v, int kind);
void brine_sb_fmt_u64(brine_sb *sb, uint64_t v, int kind);
void brine_sb_fmt_i128(brine_sb *sb, brine_i128 v, int kind);
void brine_sb_fmt_u128(brine_sb *sb, brine_u128 v, int kind);
void brine_sb_fmt_f64(brine_sb *sb, double v, int prec, int kind);
void brine_sb_pad(brine_sb *sb, int64_t start, int64_t width, bool left, bool zero);
brine_str *brine_sb_finish(brine_sb *sb);
void brine_out_sb(brine_sb *sb);
void brine_err_sb(brine_sb *sb);
void brine_print_sb(brine_sb *sb);
void brine_eprint_sb(brine_sb *sb);

/* ------------------------------------------------------------ misc */
double brine_clock(void);
brine_arr *brine_args(void);
/* argv[i] of `main` (NULL past the end), for the test runner */
const char *brine_arg(int64_t i);
/* files and processes: false means failure, with the message in the out string */
bool brine_read_file(brine_str *path, brine_str **out);
bool brine_write_file(brine_str *path, brine_str *text, brine_str **err);
bool brine_file_exists(brine_str *path);
brine_str *brine_getenv(brine_str *name);
bool brine_run(brine_arr *argv, int64_t *code, brine_str **err);
/* the `os` module. Functions returning bool report failure with an error text in *err
 * (the same text Rust's io::Error prints: "<reason> (os error N)"). */
bool brine_os_list_dir(brine_str *path, brine_arr **out, brine_str **err);
bool brine_os_is_dir(brine_str *path);
bool brine_os_is_file(brine_str *path);
bool brine_os_make_dir(brine_str *path, brine_str **err);
bool brine_os_remove_file(brine_str *path, brine_str **err);
bool brine_os_rename(brine_str *src, brine_str *dst, brine_str **err);
brine_str *brine_os_cwd(void);
bool brine_os_set_cwd(brine_str *path, brine_str **err);
/* writes the bytes of a `[u8]` */
bool brine_os_write_bytes(brine_str *path, brine_arr *data, brine_str **err);
brine_str *brine_os_exe_path(void);
brine_str *brine_os_temp_dir(void);
bool brine_os_abs_path(brine_str *path, brine_str **out, brine_str **err);
brine_str *brine_os_name(void);
brine_str *brine_os_arch(void);
int64_t brine_os_pid(void);
int64_t brine_os_cpu_count(void);
int64_t brine_os_thread_count(void);
void brine_os_sleep(double seconds);
bool brine_os_is_terminal(int64_t fd);
void brine_os_enable_ansi(void);
bool brine_os_free_ports(int64_t n, brine_arr **out, brine_str **err);
bool brine_os_spawn(brine_arr *cmd, brine_map *env, bool capture, bool in, int64_t *handle, brine_str **err);
int64_t brine_os_wait(int64_t handle, const char *loc);
bool brine_os_poll(int64_t handle, int64_t *code, const char *loc);
void brine_os_kill(int64_t handle, const char *loc);
void brine_os_output(int64_t handle, brine_str **out, brine_str **err, const char *loc);
void brine_flush(void);
void brine_rt_init(int argc, char **argv);
void brine_rt_shutdown(void);

/* math wrappers (generated code never calls libm names directly). The exact
 * operations (sqrt, rounding, fabs, fmod) are the C library's: IEEE fixes
 * their results. The others come from brine_math.c, the same code on every
 * target, since C libraries round them differently; f32 goes through f64. */
#define BR_MATH1(name) \
    static inline double brine_f_##name(double x) { return name(x); } \
    static inline float brine_f_##name##f(float x) { return name##f(x); }
BR_MATH1(sqrt)
BR_MATH1(floor)
BR_MATH1(ceil)
BR_MATH1(round)
BR_MATH1(fabs)
#define BR_MMATH1(name) \
    double brine_m_##name(double x); \
    static inline double brine_f_##name(double x) { return brine_m_##name(x); } \
    static inline float brine_f_##name##f(float x) { return (float)brine_m_##name((double)x); }
BR_MMATH1(sin)
BR_MMATH1(cos)
BR_MMATH1(tan)
BR_MMATH1(asin)
BR_MMATH1(acos)
BR_MMATH1(atan)
BR_MMATH1(sinh)
BR_MMATH1(cosh)
BR_MMATH1(tanh)
BR_MMATH1(exp)
BR_MMATH1(exp2)
BR_MMATH1(expm1)
BR_MMATH1(log)
BR_MMATH1(log2)
BR_MMATH1(log10)
BR_MMATH1(log1p)
BR_MMATH1(cbrt)
double brine_m_pow(double a, double b);
double brine_m_atan2(double y, double x);
double brine_m_hypot(double x, double y);
static inline double brine_f_pow(double a, double b) { return brine_m_pow(a, b); }
static inline double brine_f_atan2(double y, double x) { return brine_m_atan2(y, x); }
static inline double brine_f_hypot(double x, double y) { return brine_m_hypot(x, y); }
/* float `%` also takes the sign of the divisor, like Python */
static inline double brine_f_fmod(double a, double b) { double r = fmod(a, b); return (r != 0 && ((r < 0) != (b < 0))) ? r + b : r; }
static inline float brine_f_fmodf(float a, float b) { float r = fmodf(a, b); return (r != 0 && ((r < 0) != (b < 0))) ? r + b : r; }
/* float min and max: a NaN loses to a number, and -0.0 is less than 0.0 (C's
 * fmin may return either zero), so the result is the same on every platform
 * and on a GPU (ptx.br). With AVX (the x86-64-v3 and v4 builds of a release)
 * they are two selects and no branch, so loops that use them vectorize; equal
 * values are merged bitwise, which only matters for the sign of a zero.
 * Without vector blends the selects become slower than the branches. */
#if defined(__AVX__)
#define BR_MINMAX(T, U, S)                                                      \
static inline T brine_f_fmin##S(T a, T b) {                                     \
    U x, y; T e;                                                                \
    __builtin_memcpy(&x, &a, sizeof x); __builtin_memcpy(&y, &b, sizeof y);     \
    x |= y; __builtin_memcpy(&e, &x, sizeof e);                                 \
    T r = ((a < b) | (b != b)) ? a : b;                                         \
    return a == b ? e : r;                                                      \
}                                                                               \
static inline T brine_f_fmax##S(T a, T b) {                                     \
    U x, y; T e;                                                                \
    __builtin_memcpy(&x, &a, sizeof x); __builtin_memcpy(&y, &b, sizeof y);     \
    x &= y; __builtin_memcpy(&e, &x, sizeof e);                                 \
    T r = ((a > b) | (b != b)) ? a : b;                                         \
    return a == b ? e : r;                                                      \
}
#else
#define BR_MINMAX(T, U, S)                                                      \
static inline T brine_f_fmin##S(T a, T b) {                                     \
    if (a < b || b != b) return a;                                              \
    if (b < a || a != a) return b;                                              \
    return signbit(a) ? a : b;                                                  \
}                                                                               \
static inline T brine_f_fmax##S(T a, T b) {                                     \
    if (a > b || b != b) return a;                                              \
    if (b > a || a != a) return b;                                              \
    return signbit(a) ? b : a;                                                  \
}
#endif
BR_MINMAX(double, uint64_t, )
BR_MINMAX(float, uint32_t, f)

/* ------------------------------------------------------------ checked integer arithmetic */
/* r = a * b; true when it overflows. Clang turns a signed 128-bit overflow
 * check into a call to __muloti4, which only its own runtime library has, and
 * on Linux it links libgcc instead, so there i128 goes through an unsigned
 * multiply. */
#define BR_MULO(T, S) \
    static inline bool brine_mulo_##S(T a, T b, T *r) { return __builtin_mul_overflow(a, b, r); }
BR_MULO(int8_t, i8)
BR_MULO(int16_t, i16)
BR_MULO(int32_t, i32)
BR_MULO(int64_t, i64)
BR_MULO(uint8_t, u8)
BR_MULO(uint16_t, u16)
BR_MULO(uint32_t, u32)
BR_MULO(uint64_t, u64)
BR_MULO(brine_u128, u128)
#if defined(__clang__)
static inline bool brine_mulo_i128(brine_i128 a, brine_i128 b, brine_i128 *r) {
    brine_u128 ua = a < 0 ? (brine_u128)0 - (brine_u128)a : (brine_u128)a;
    brine_u128 ub = b < 0 ? (brine_u128)0 - (brine_u128)b : (brine_u128)b;
    brine_u128 p;
    bool ovf = __builtin_mul_overflow(ua, ub, &p);
    bool neg = (a < 0) != (b < 0) && p != 0;
    /* the largest magnitude: 2^127 for a negative result, 2^127 - 1 otherwise */
    brine_u128 lim = ((brine_u128)1 << 127) - (neg ? 0 : 1);
    *r = (brine_i128)(neg ? (brine_u128)0 - p : p);
    return ovf || p > lim;
}
#else
BR_MULO(brine_i128, i128)
#endif

/* UT is the unsigned type shifts are done in */
#define BR_COMMON(T, S, UT)                                                                   \
    static inline T brine_add_##S(T a, T b, const char *loc) {                                 \
        T r;                                                                                   \
        if (BR_UNLIKELY(__builtin_add_overflow(a, b, &r))) brine_panic_ovf("addition", loc);   \
        return r;                                                                              \
    }                                                                                          \
    static inline T brine_sub_##S(T a, T b, const char *loc) {                                 \
        T r;                                                                                   \
        if (BR_UNLIKELY(__builtin_sub_overflow(a, b, &r))) brine_panic_ovf("subtraction", loc); \
        return r;                                                                              \
    }                                                                                          \
    static inline T brine_mul_##S(T a, T b, const char *loc) {                                 \
        T r;                                                                                   \
        if (BR_UNLIKELY(brine_mulo_##S(a, b, &r))) brine_panic_ovf("multiplication", loc); \
        return r;                                                                              \
    }                                                                                          \
    static inline T brine_shl_##S(T a, int64_t b, const char *loc) {                           \
        if (BR_UNLIKELY(b < 0 || b >= (int64_t)(sizeof(T) * 8))) brine_panic_at(loc, "shift amount out of range"); \
        return (T)((UT)a << b);                                                                \
    }                                                                                          \
    static inline T brine_shr_##S(T a, int64_t b, const char *loc) {                           \
        if (BR_UNLIKELY(b < 0 || b >= (int64_t)(sizeof(T) * 8))) brine_panic_at(loc, "shift amount out of range"); \
        return (T)(a >> b);                                                                    \
    }                                                                                          \
    static inline T brine_conv_##S##_from_i64(int64_t x, const char *loc) {                    \
        T r;                                                                                   \
        if (BR_UNLIKELY(__builtin_add_overflow(x, (int64_t)0, &r))) brine_panic_at(loc, "integer conversion out of range for " #S); \
        return r;                                                                              \
    }                                                                                          \
    static inline T brine_conv_##S##_from_u64(uint64_t x, const char *loc) {                   \
        T r;                                                                                   \
        if (BR_UNLIKELY(__builtin_add_overflow(x, (uint64_t)0, &r))) brine_panic_at(loc, "integer conversion out of range for " #S); \
        return r;                                                                              \
    }                                                                                          \
    static inline T brine_conv_##S##_from_i128(brine_i128 x, const char *loc) {                \
        T r;                                                                                   \
        if (BR_UNLIKELY(__builtin_add_overflow(x, (brine_i128)0, &r))) brine_panic_at(loc, "integer conversion out of range for " #S); \
        return r;                                                                              \
    }                                                                                          \
    static inline T brine_conv_##S##_from_u128(brine_u128 x, const char *loc) {                \
        T r;                                                                                   \
        if (BR_UNLIKELY(__builtin_add_overflow(x, (brine_u128)0, &r))) brine_panic_at(loc, "integer conversion out of range for " #S); \
        return r;                                                                              \
    }

#define BR_SIGNED(T, S, MIN, MAX, UT)                                                          \
    BR_COMMON(T, S, UT)                                                                        \
    /* integer division floors and `%` takes the sign of the divisor, like Python */           \
    static inline T brine_fdiv_##S(T a, T b) {                                                 \
        T q = (T)(a / b);                                                                      \
        return (T)((a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q);                          \
    }                                                                                          \
    static inline T brine_fmod_##S(T a, T b) {                                                 \
        if (b == -1) return 0;                                                                 \
        T r = (T)(a % b);                                                                      \
        return (T)((r != 0 && ((r < 0) != (b < 0))) ? r + b : r);                              \
    }                                                                                          \
    static inline T brine_div_##S(T a, T b, const char *loc) {                                 \
        if (BR_UNLIKELY(b == 0)) brine_panic_at(loc, "division by zero");                      \
        if (BR_UNLIKELY(b == -1 && a == MIN)) brine_panic_ovf("division", loc);                \
        return brine_fdiv_##S(a, b);                                                           \
    }                                                                                          \
    static inline T brine_mod_##S(T a, T b, const char *loc) {                                 \
        if (BR_UNLIKELY(b == 0)) brine_panic_at(loc, "remainder by zero");                     \
        return brine_fmod_##S(a, b);                                                           \
    }                                                                                          \
    static inline T brine_neg_##S(T a, const char *loc) {                                     \
        if (BR_UNLIKELY(a == MIN)) brine_panic_ovf("negation", loc);                           \
        return -a;                                                                             \
    }                                                                                          \
    static inline T brine_abs_##S(T a, const char *loc) {                                     \
        if (BR_UNLIKELY(a == MIN)) brine_panic_ovf("abs", loc);                                \
        return a < 0 ? -a : a;                                                                 \
    }                                                                                          \
    static inline T brine_conv_##S##_from_f64(double x, const char *loc) {                     \
        if (BR_UNLIKELY(!(x >= (double)MIN && x < (double)MAX + 1.0)))                         \
            brine_panic_at(loc, "float to integer conversion out of range (or NaN) for " #S);   \
        return (T)x;                                                                           \
    }

#define BR_UNSIGNED(T, S, MAX)                                                                 \
    BR_COMMON(T, S, T)                                                                         \
    static inline T brine_fdiv_##S(T a, T b) { return (T)(a / b); }                            \
    static inline T brine_fmod_##S(T a, T b) { return (T)(a % b); }                            \
    static inline T brine_div_##S(T a, T b, const char *loc) {                                 \
        if (BR_UNLIKELY(b == 0)) brine_panic_at(loc, "division by zero");                      \
        return a / b;                                                                          \
    }                                                                                          \
    static inline T brine_mod_##S(T a, T b, const char *loc) {                                 \
        if (BR_UNLIKELY(b == 0)) brine_panic_at(loc, "remainder by zero");                     \
        return a % b;                                                                          \
    }                                                                                          \
    static inline T brine_abs_##S(T a, const char *loc) {                                     \
        (void)loc;                                                                             \
        return a;                                                                              \
    }                                                                                          \
    static inline T brine_conv_##S##_from_f64(double x, const char *loc) {                     \
        if (BR_UNLIKELY(!(x >= 0.0 && x < (double)MAX + 1.0)))                                 \
            brine_panic_at(loc, "float to integer conversion out of range (or NaN) for " #S);   \
        return (T)x;                                                                           \
    }

BR_SIGNED(int8_t, i8, INT8_MIN, INT8_MAX, uint64_t)
BR_SIGNED(int16_t, i16, INT16_MIN, INT16_MAX, uint64_t)
BR_SIGNED(int32_t, i32, INT32_MIN, INT32_MAX, uint64_t)
BR_SIGNED(int64_t, i64, INT64_MIN, INT64_MAX, uint64_t)
BR_SIGNED(brine_i128, i128, BR_I128_MIN, BR_I128_MAX, brine_u128)
BR_UNSIGNED(uint8_t, u8, UINT8_MAX)
BR_UNSIGNED(uint16_t, u16, UINT16_MAX)
BR_UNSIGNED(uint32_t, u32, UINT32_MAX)
BR_UNSIGNED(uint64_t, u64, UINT64_MAX)
BR_UNSIGNED(brine_u128, u128, BR_U128_MAX)

/* ------------------------------------------------------------ parallelism */
typedef void (*brine_chunk_fn)(void *ctx, int64_t chunk, int64_t lo, int64_t hi);
typedef void (*brine_task_fn)(void *ctx, int64_t idx);
/* Number of chunks for n iterations. Depends only on n, so reductions are
 * deterministic regardless of thread count. */
static inline int64_t brine_par_plan(int64_t n) { return n <= 0 ? 0 : (n < 256 ? n : 256); }
void brine_par_run(int64_t lo, int64_t hi, int64_t nchunks, brine_chunk_fn fn, void *ctx);
void brine_par_tasks(int64_t k, brine_task_fn fn, void *ctx);

/* Parallel reductions (`par sum(...)` etc.) have one combination order, which
 * depends only on the element count n: elements are split into leaves of
 * BRINE_LEAF consecutive elements, each folded left to right by the leaf
 * function, and the leaves are combined by a balanced binary tree, level by
 * level, adjacent pairs (a node without a partner moves up a level as it is).
 * So float results have the same bits on any machine, thread count or device,
 * and integer overflow traps at the same partial result. */
#define BRINE_LEAF 16
enum { BRINE_RED_SUM, BRINE_RED_PRODUCT, BRINE_RED_MIN, BRINE_RED_MAX, BRINE_RED_ANY, BRINE_RED_ALL };
/* value kinds, in the order of `K` in compiler/ir.br */
enum {
    BRINE_VK_I8, BRINE_VK_I16, BRINE_VK_I32, BRINE_VK_I64, BRINE_VK_I128,
    BRINE_VK_U8, BRINE_VK_U16, BRINE_VK_U32, BRINE_VK_U64, BRINE_VK_U128,
    BRINE_VK_F32, BRINE_VK_F64, BRINE_VK_PTR, BRINE_VK_BOOL
};
/* folds elements [lo, hi) of leaf `leaf` into *val; sets *has unless all were filtered out */
typedef void (*brine_leaf_fn)(void *ctx, int64_t leaf, int64_t lo, int64_t hi, void *val, bool *has);

/* A parallel construct, as the compiler describes it for the GPU (lower.br,
 * `gpu_emit`). `ptx` is NULL when the body can't run on a GPU. */
typedef struct brine_kernel {
    BR_P(const char *, ptx);   /* the program's GPU module: PTX, or MSL on macOS */
    BR_P(const char *, entry); /* the kernel */
    BR_P(const char *, where); /* file:line:col, for BRINE_GPU_LOG */
    int64_t kind;      /* 0 `par for`, 1 map, 2 reduction */
    int64_t op, vk;    /* reductions: BRINE_RED_*, BRINE_VK_* */
    int64_t ctx_size;
    /* the arrays in the context: [nfields, (offset, flags, node)*, nodes...],
     * a node [element size, n, (offset in an element, node)*], nodes numbered
     * from the first; flags 1: written by the body, 2: not read */
    BR_P(const int64_t *, ptrs);
    int64_t cost, cost64;       /* estimated instructions per element (f64 ones) */
    BR_P(const char *, comb);   /* reductions: the combine kernel */
    BR_P(void *, state);        /* the runtime's */
    BR_P(const char *, why);    /* why it can't run on a GPU */
} brine_kernel;

/* Runs fn over [0, n) on the thread pool, or on a GPU when that pays. */
void brine_par_for(brine_kernel *k, void *ctx, int64_t n, brine_chunk_fn fn);
/* *val and *has: the accumulator (the reducer's start value, false); set when an element had a value */
void brine_par_reduce(brine_kernel *k, int64_t n, brine_leaf_fn fn, void *ctx, void *val, bool *has, const char *loc);

/* ------------------------------------------------------------ world (multi-process) */
int64_t brine_world_rank(void);
int64_t brine_world_size(void);
void brine_world_barrier(const char *loc);
void brine_world_shift_edge(const char *d, const void *v, void *out, int64_t by, const void *edge, const char *loc);
void brine_world_shift(const char *d, const void *v, void *out, int64_t by, const char *loc);
brine_arr *brine_world_exchange(const char *ed, brine_arr *xs, const char *loc);
void brine_world_bcast(const char *d, const void *v, void *out, int64_t root, const char *loc);
brine_arr *brine_world_gather(const char *ed, const void *v, const char *loc);
int64_t brine_world_reduce_i64(int64_t v, int op, const char *loc);
double brine_world_reduce_f64(double v, int op, const char *loc);
brine_arr *brine_world_reduce_arr(const char *ed, brine_arr *xs, int op, const char *loc);

#endif
