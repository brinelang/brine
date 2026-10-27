/* brine runtime — public header included by generated code.
 * Keep this header free of platform headers (windows.h etc.) so user
 * identifiers can never collide with platform macros. */
#ifndef BRINE_RT_H
#define BRINE_RT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <math.h>

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
    brine_elem_fn retain;  /* NULL for plain-data elements */
    brine_elem_fn release;
    char *data;
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
typedef struct brine_map_ty {
    int64_t ksz, vsz;
    uint64_t (*hash)(const void *k);
    bool (*keq)(const void *a, const void *b);
    bool (*veq)(const void *a, const void *b); /* NULL: values can't be compared */
    void (*kfmt)(brine_sb *sb, const void *k);
    brine_elem_fn kretain, krelease, vretain, vrelease; /* NULL for plain data */
} brine_map_ty;

/* Hash map that remembers insertion order. Entries live in `keys`/`vals`
 * (slots [0, used)); `index` is an open-addressing table of entry numbers. */
typedef struct brine_map {
    int64_t rc;
    int64_t len;  /* live entries */
    int64_t used; /* entry slots used, including removed ones */
    int64_t cap;  /* entry slots allocated */
    int64_t nidx; /* index slots: 0 or a power of two >= 2 * cap */
    const brine_map_ty *ty;
    char *keys;
    char *vals;
    uint64_t *hashes;
    bool *live;
    int64_t *index; /* -1 empty, -2 removed, else an entry number */
} brine_map;

/* header shared by every enum box (enums whose variants carry data) */
typedef struct brine_box {
    int64_t rc; /* < 0 means immortal (variants without fields) */
    int64_t tag;
} brine_box;

struct brine_sb {
    char *p;
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
static inline void brine_str_release(brine_str *s) {
    if (__atomic_load_n(&s->rc, __ATOMIC_RELAXED) < 0) return;
    if (__atomic_sub_fetch(&s->rc, 1, __ATOMIC_ACQ_REL) == 0) brine_str_free(s);
}
static inline void brine_arr_retain(brine_arr *a) { __atomic_fetch_add(&a->rc, 1, __ATOMIC_RELAXED); }
static inline void brine_arr_release(brine_arr *a) {
    if (__atomic_sub_fetch(&a->rc, 1, __ATOMIC_ACQ_REL) == 0) brine_arr_free(a);
}
void *brine_box_new(int64_t size, int64_t tag);
void brine_box_free(void *p);
static inline void brine_box_retain(void *p) {
    brine_box *b = (brine_box *)p;
    if (__atomic_load_n(&b->rc, __ATOMIC_RELAXED) >= 0) __atomic_fetch_add(&b->rc, 1, __ATOMIC_RELAXED);
}
/* drops one reference; true when the box must be freed */
static inline bool brine_box_dec(void *p) {
    brine_box *b = (brine_box *)p;
    if (__atomic_load_n(&b->rc, __ATOMIC_RELAXED) < 0) return false;
    return __atomic_sub_fetch(&b->rc, 1, __ATOMIC_ACQ_REL) == 0;
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
bool brine_str_eq(brine_str *a, brine_str *b);
bool brine_str_contains(brine_str *s, brine_str *sub);
/* `s[i]`: the byte at i as a one-character string (never allocates) */
brine_str *brine_str_byte(brine_str *s, int64_t i);
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
void brine_arr_unique(brine_arr **pa);
void brine_arr_push(brine_arr **pa, const void *elem);
void brine_arr_pop(brine_arr **pa, void *out, const char *loc);
void brine_arr_pop_at(brine_arr **pa, int64_t i, void *out, const char *loc);
/* takes ownership of *elem */
void brine_arr_insert(brine_arr **pa, int64_t i, const void *elem, const char *loc);
void brine_arr_extend(brine_arr **pa, brine_arr *b);
void brine_arr_reverse(brine_arr **pa);
void brine_arr_clear(brine_arr **pa);
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
uint64_t brine_hash_str(brine_str *s);

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
void brine_os_sleep(double seconds);
bool brine_os_is_terminal(int64_t fd);
void brine_os_enable_ansi(void);
bool brine_os_free_ports(int64_t n, brine_arr **out, brine_str **err);
bool brine_os_spawn(brine_arr *cmd, brine_map *env, bool capture, bool in, int64_t *handle, brine_str **err);
int64_t brine_os_wait(int64_t handle, const char *loc);
bool brine_os_poll(int64_t handle, int64_t *code, const char *loc);
void brine_os_kill(int64_t handle, const char *loc);
void brine_os_output(int64_t handle, brine_str **out, brine_str **err, const char *loc);
void brine_rt_init(int argc, char **argv);
void brine_rt_shutdown(void);

/* math wrappers (generated code never calls libm names directly) */
#define BR_MATH1(name) \
    static inline double brine_f_##name(double x) { return name(x); } \
    static inline float brine_f_##name##f(float x) { return name##f(x); }
BR_MATH1(sqrt)
BR_MATH1(sin)
BR_MATH1(cos)
BR_MATH1(tan)
BR_MATH1(exp)
BR_MATH1(log)
BR_MATH1(floor)
BR_MATH1(ceil)
BR_MATH1(round)
BR_MATH1(fabs)
static inline double brine_f_pow(double a, double b) { return pow(a, b); }
/* float `%` also takes the sign of the divisor, like Python */
static inline double brine_f_fmod(double a, double b) { double r = fmod(a, b); return (r != 0 && ((r < 0) != (b < 0))) ? r + b : r; }
static inline float brine_f_fmodf(float a, float b) { float r = fmodf(a, b); return (r != 0 && ((r < 0) != (b < 0))) ? r + b : r; }
static inline double brine_f_fmin(double a, double b) { return fmin(a, b); }
static inline double brine_f_fmax(double a, double b) { return fmax(a, b); }
static inline float brine_f_fminf(float a, float b) { return fminf(a, b); }
static inline float brine_f_fmaxf(float a, float b) { return fmaxf(a, b); }

/* ------------------------------------------------------------ checked integer arithmetic */
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
        if (BR_UNLIKELY(__builtin_mul_overflow(a, b, &r))) brine_panic_ovf("multiplication", loc); \
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
    const char *ptx;   /* the program's PTX module */
    const char *entry; /* the kernel */
    const char *where; /* file:line:col, for BRINE_GPU_LOG */
    int64_t kind;      /* 0 `par for`, 1 map, 2 reduction */
    int64_t op, vk;    /* reductions: BRINE_RED_*, BRINE_VK_* */
    int64_t ctx_size;
    /* the arrays in the context: [nfields, (offset, flags, node)*, nodes...],
     * a node [element size, n, (offset in an element, node)*], nodes numbered
     * from the first; flags 1: written by the body, 2: not read */
    const int64_t *ptrs;
    int64_t cost, cost64; /* estimated instructions per element (f64 ones) */
    const char *comb;     /* reductions: the combine kernel */
    void *state;          /* the runtime's */
    const char *why;      /* why it can't run on a GPU */
} brine_kernel;

/* Runs fn over [0, n) on the thread pool, or on a GPU when that pays. */
void brine_par_for(brine_kernel *k, void *ctx, int64_t n, brine_chunk_fn fn);
/* *val and *has: the accumulator (the reducer's start value, false); set when an element had a value */
void brine_par_reduce(brine_kernel *k, int64_t n, brine_leaf_fn fn, void *ctx, void *val, bool *has, const char *loc);

/* ------------------------------------------------------------ world (multi-process) */
int64_t brine_world_rank(void);
int64_t brine_world_size(void);
void brine_world_barrier(void);
void brine_world_shift(const char *d, const void *v, void *out, int64_t by, const char *loc);
brine_arr *brine_world_exchange(const char *ed, brine_arr *xs, const char *loc);
void brine_world_bcast(const char *d, const void *v, void *out, int64_t root, const char *loc);
brine_arr *brine_world_gather(const char *ed, const void *v, const char *loc);
int64_t brine_world_reduce_i64(int64_t v, int op, const char *loc);
double brine_world_reduce_f64(double v, int op, const char *loc);

#endif
