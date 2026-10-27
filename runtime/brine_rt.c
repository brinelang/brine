/* brine runtime — implementation. Compiled as a separate translation unit. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif

#include "brine_rt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#endif

/* ============================================================ platform layer */
#ifdef _WIN32
typedef CRITICAL_SECTION br_mutex;
typedef CONDITION_VARIABLE br_cond;
typedef HANDLE br_thread;
static void br_mutex_init(br_mutex *m) { InitializeCriticalSection(m); }
static void br_lock(br_mutex *m) { EnterCriticalSection(m); }
static void br_unlock(br_mutex *m) { LeaveCriticalSection(m); }
static int br_trylock(br_mutex *m) { return TryEnterCriticalSection(m) ? 1 : 0; }
static void br_cond_init(br_cond *c) { InitializeConditionVariable(c); }
static void br_cond_wait(br_cond *c, br_mutex *m) { SleepConditionVariableCS(c, m, INFINITE); }
static void br_cond_broadcast(br_cond *c) { WakeAllConditionVariable(c); }
typedef struct { void (*fn)(void *); void *arg; } br_tramp;
static DWORD WINAPI br_thread_main(LPVOID p) {
    br_tramp t = *(br_tramp *)p;
    free(p);
    t.fn(t.arg);
    return 0;
}
static int br_thread_start(br_thread *th, void (*fn)(void *), void *arg) {
    br_tramp *t = (br_tramp *)malloc(sizeof(br_tramp));
    t->fn = fn;
    t->arg = arg;
    *th = CreateThread(NULL, 64u << 20, br_thread_main, t, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    return *th != NULL;
}
static void br_thread_join(br_thread th) {
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
}
static int br_ncpu(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
}
static void br_sleep_ms(int ms) { Sleep(ms); }
typedef SOCKET br_sock;
#define BR_BAD_SOCK INVALID_SOCKET
static void br_sock_close(br_sock s) { closesocket(s); }
#else
typedef pthread_mutex_t br_mutex;
typedef pthread_cond_t br_cond;
typedef pthread_t br_thread;
static void br_mutex_init(br_mutex *m) { pthread_mutex_init(m, NULL); }
static void br_lock(br_mutex *m) { pthread_mutex_lock(m); }
static void br_unlock(br_mutex *m) { pthread_mutex_unlock(m); }
static int br_trylock(br_mutex *m) { return pthread_mutex_trylock(m) == 0; }
static void br_cond_init(br_cond *c) { pthread_cond_init(c, NULL); }
static void br_cond_wait(br_cond *c, br_mutex *m) { pthread_cond_wait(c, m); }
static void br_cond_broadcast(br_cond *c) { pthread_cond_broadcast(c); }
typedef struct { void (*fn)(void *); void *arg; } br_tramp;
static void *br_thread_main(void *p) {
    br_tramp t = *(br_tramp *)p;
    free(p);
    t.fn(t.arg);
    return NULL;
}
static int br_thread_start(br_thread *th, void (*fn)(void *), void *arg) {
    br_tramp *t = (br_tramp *)malloc(sizeof(br_tramp));
    t->fn = fn;
    t->arg = arg;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, (size_t)64 << 20);
    int ok = pthread_create(th, &at, br_thread_main, t) == 0;
    pthread_attr_destroy(&at);
    return ok;
}
static void br_thread_join(br_thread th) { pthread_join(th, NULL); }
static int br_ncpu(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}
static void br_sleep_ms(int ms) {
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}
typedef int br_sock;
#define BR_BAD_SOCK (-1)
static void br_sock_close(br_sock s) { close(s); }
#endif

/* ============================================================ globals */
static br_mutex br_io_mu;
static int br_argc;
static char **br_argv;
static int64_t br_rank = 0, br_size = 1;

/* ============================================================ panics */
static void br_flush(void) { fflush(stdout); }

void brine_panic_at(const char *loc, const char *msg) {
    br_flush();
    if (br_size > 1)
        fprintf(stderr, "\npanic (rank %lld): %s\n  --> %s\n", (long long)br_rank, msg, loc);
    else
        fprintf(stderr, "\npanic: %s\n  --> %s\n", msg, loc);
    fflush(stderr);
    exit(101);
}

static BR_NORETURN void br_panicf(const char *loc, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    brine_panic_at(loc, buf);
}

void brine_panic_idx(int64_t i, int64_t len, const char *loc) {
    br_panicf(loc, "index %lld is out of bounds for an array of length %lld", (long long)i, (long long)len);
}
void brine_panic_ovf(const char *op, const char *loc) { br_panicf(loc, "integer overflow in %s", op); }
void brine_panic_str(const char *loc, brine_str *msg) { br_panicf(loc, "%.*s", (int)msg->len, msg->data); }
void brine_assert_fail(const char *loc, const char *src, brine_str *msg) {
    if (msg)
        br_panicf(loc, "assertion failed: %s\n  message: %.*s", src, (int)msg->len, msg->data);
    br_panicf(loc, "assertion failed: %s", src);
}
void brine_exit(int64_t code) {
    br_flush();
    exit((int)code);
}
void brine_unreachable(void) {
    br_flush();
    fprintf(stderr, "internal error: reached unreachable code\n");
    abort();
}

/* ============================================================ memory */
void *brine_alloc(int64_t n) {
    void *p = malloc(n > 0 ? (size_t)n : 1);
    if (!p) {
        fprintf(stderr, "out of memory\n");
        abort();
    }
    return p;
}
void brine_free(void *p) { free(p); }
/* live string/array count, reported at exit when BRINE_LEAKCHECK is set */
static int64_t br_live;
#define BR_LIVE(d) __atomic_fetch_add(&br_live, (d), __ATOMIC_RELAXED)
void brine_str_free(brine_str *s) {
    BR_LIVE(-1);
    free(s);
}
void brine_arr_free(brine_arr *a) {
    BR_LIVE(-1);
    if (a->release)
        for (int64_t i = 0; i < a->len; i++) a->release(a->data + i * a->esz);
    free(a->data);
    free(a);
}
void *brine_box_new(int64_t size, int64_t tag) {
    brine_box *b = (brine_box *)brine_alloc(size);
    BR_LIVE(1);
    b->rc = 1;
    b->tag = tag;
    return b;
}
void brine_box_free(void *p) {
    BR_LIVE(-1);
    free(p);
}
void brine_rt_str(void *p) { brine_str_retain(*(brine_str **)p); }
void brine_rl_str(void *p) { brine_str_release(*(brine_str **)p); }
void brine_rt_arr(void *p) { brine_arr_retain(*(brine_arr **)p); }
void brine_rl_arr(void *p) { brine_arr_release(*(brine_arr **)p); }

/* ============================================================ strings */
static brine_str *br_str_alloc(int64_t n) {
    brine_str *s = (brine_str *)brine_alloc((int64_t)sizeof(brine_str) + n + 1);
    BR_LIVE(1);
    s->rc = 1;
    s->len = n;
    s->data[n] = 0;
    return s;
}
brine_str *brine_str_new(const char *p, int64_t n) {
    brine_str *s = br_str_alloc(n);
    if (n) memcpy(s->data, p, (size_t)n);
    return s;
}
brine_str *brine_str_concat(brine_str *a, brine_str *b) {
    brine_str *s = br_str_alloc(a->len + b->len);
    memcpy(s->data, a->data, (size_t)a->len);
    memcpy(s->data + a->len, b->data, (size_t)b->len);
    return s;
}
bool brine_str_eq(brine_str *a, brine_str *b) {
    return a == b || (a->len == b->len && memcmp(a->data, b->data, (size_t)a->len) == 0);
}
bool brine_str_contains(brine_str *s, brine_str *sub) {
    if (sub->len == 0) return true;
    for (int64_t i = 0; i + sub->len <= s->len; i++)
        if (s->data[i] == sub->data[0] && memcmp(s->data + i, sub->data, (size_t)sub->len) == 0) return true;
    return false;
}
/* one-byte strings for `s[i]`, immortal so indexing never allocates */
typedef struct {
    int64_t rc, len;
    char d[2];
} br_str1;
#define BR_B1(i) {-1, 1, {(char)(i), 0}}
#define BR_B4(i) BR_B1(i), BR_B1((i) + 1), BR_B1((i) + 2), BR_B1((i) + 3)
#define BR_B16(i) BR_B4(i), BR_B4((i) + 4), BR_B4((i) + 8), BR_B4((i) + 12)
#define BR_B64(i) BR_B16(i), BR_B16((i) + 16), BR_B16((i) + 32), BR_B16((i) + 48)
static br_str1 br_bytes[256] = {BR_B64(0), BR_B64(64), BR_B64(128), BR_B64(192)};

brine_str *brine_str_byte(brine_str *s, int64_t i) { return (brine_str *)&br_bytes[(unsigned char)s->data[i]]; }

static brine_str *br_sub(brine_str *s, int64_t lo, int64_t hi) {
    if (lo == 0 && hi == s->len) {
        brine_str_retain(s);
        return s;
    }
    if (hi - lo == 1) return (brine_str *)&br_bytes[(unsigned char)s->data[lo]];
    return brine_str_new(s->data + lo, hi - lo);
}
brine_str *brine_str_slice(brine_str *s, int64_t lo, int64_t hi, const char *loc) {
    if (BR_UNLIKELY(lo < 0 || lo > hi || hi > s->len))
        br_panicf(loc, "slice [%lld:%lld] is out of bounds for a string of length %lld", (long long)lo, (long long)hi, (long long)s->len);
    return br_sub(s, lo, hi);
}
/* length of the UTF-8 character starting at p (1 for invalid bytes) */
static int64_t br_utf8_len(const unsigned char *p, int64_t avail) {
    int64_t n = p[0] < 0x80 ? 1 : (p[0] >> 5) == 6 ? 2 : (p[0] >> 4) == 14 ? 3 : (p[0] >> 3) == 30 ? 4 : 1;
    if (n > avail) return 1;
    for (int64_t i = 1; i < n; i++)
        if ((p[i] & 0xc0) != 0x80) return 1;
    return n;
}
int64_t brine_ord(brine_str *s, const char *loc) {
    const unsigned char *p = (const unsigned char *)s->data;
    int64_t n = s->len ? br_utf8_len(p, s->len) : 0;
    if (n == 0 || n != s->len) br_panicf(loc, "ord() needs exactly one character, got a string of length %lld", (long long)s->len);
    if (n == 1) return p[0];
    int64_t c = p[0] & (0x7f >> n);
    for (int64_t i = 1; i < n; i++) c = (c << 6) | (p[i] & 0x3f);
    return c;
}
brine_str *brine_chr(int64_t c, const char *loc) {
    char b[4];
    int n;
    if (c < 0 || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) br_panicf(loc, "chr(%lld) is not a valid character", (long long)c);
    if (c < 0x80) return (brine_str *)&br_bytes[c];
    if (c < 0x800) {
        b[0] = (char)(0xc0 | (c >> 6));
        b[1] = (char)(0x80 | (c & 0x3f));
        n = 2;
    } else if (c < 0x10000) {
        b[0] = (char)(0xe0 | (c >> 12));
        b[1] = (char)(0x80 | ((c >> 6) & 0x3f));
        b[2] = (char)(0x80 | (c & 0x3f));
        n = 3;
    } else {
        b[0] = (char)(0xf0 | (c >> 18));
        b[1] = (char)(0x80 | ((c >> 12) & 0x3f));
        b[2] = (char)(0x80 | ((c >> 6) & 0x3f));
        b[3] = (char)(0x80 | (c & 0x3f));
        n = 4;
    }
    return brine_str_new(b, n);
}
static int64_t br_find(brine_str *s, brine_str *sub, int64_t from) {
    for (int64_t i = from; i + sub->len <= s->len; i++)
        if (memcmp(s->data + i, sub->data, (size_t)sub->len) == 0) return i;
    return -1;
}
int64_t brine_s_find(brine_str *s, brine_str *sub, const char *loc) {
    (void)loc;
    return br_find(s, sub, 0);
}
static brine_arr *br_strs(void) { return brine_arr_new(0, sizeof(brine_str *), brine_rt_str, brine_rl_str, ""); }
static void br_push_str(brine_arr **a, brine_str *s) { brine_arr_push(a, &s); }
brine_arr *brine_s_split(brine_str *s, brine_str *sep, const char *loc) {
    if (sep->len == 0) brine_panic_at(loc, "split() separator is empty");
    brine_arr *out = br_strs();
    int64_t at = 0;
    for (int64_t i; (i = br_find(s, sep, at)) >= 0; at = i + sep->len) br_push_str(&out, br_sub(s, at, i));
    br_push_str(&out, br_sub(s, at, s->len));
    return out;
}
static bool br_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }
brine_arr *brine_s_split_ws(brine_str *s, const char *loc) {
    (void)loc;
    brine_arr *out = br_strs();
    int64_t i = 0;
    while (i < s->len) {
        while (i < s->len && br_ws(s->data[i])) i++;
        int64_t j = i;
        while (j < s->len && !br_ws(s->data[j])) j++;
        if (j > i) br_push_str(&out, br_sub(s, i, j));
        i = j;
    }
    return out;
}
brine_str *brine_s_join(brine_str *sep, brine_arr *parts, const char *loc) {
    (void)loc;
    brine_sb sb;
    brine_sb_init(&sb);
    for (int64_t i = 0; i < parts->len; i++) {
        if (i) brine_sb_str(&sb, sep);
        brine_sb_str(&sb, ((brine_str **)parts->data)[i]);
    }
    return brine_sb_finish(&sb);
}
bool brine_s_starts_with(brine_str *s, brine_str *p, const char *loc) {
    (void)loc;
    return p->len <= s->len && memcmp(s->data, p->data, (size_t)p->len) == 0;
}
bool brine_s_ends_with(brine_str *s, brine_str *p, const char *loc) {
    (void)loc;
    return p->len <= s->len && memcmp(s->data + s->len - p->len, p->data, (size_t)p->len) == 0;
}
static brine_str *br_strip(brine_str *s, bool left, bool right) {
    int64_t lo = 0, hi = s->len;
    while (left && lo < hi && br_ws(s->data[lo])) lo++;
    while (right && hi > lo && br_ws(s->data[hi - 1])) hi--;
    if (lo == hi) return brine_str_new("", 0);
    return br_sub(s, lo, hi);
}
brine_str *brine_s_strip(brine_str *s, const char *loc) {
    (void)loc;
    return br_strip(s, true, true);
}
brine_str *brine_s_lstrip(brine_str *s, const char *loc) {
    (void)loc;
    return br_strip(s, true, false);
}
brine_str *brine_s_rstrip(brine_str *s, const char *loc) {
    (void)loc;
    return br_strip(s, false, true);
}
brine_str *brine_s_replace(brine_str *s, brine_str *a, brine_str *b, const char *loc) {
    if (a->len == 0) brine_panic_at(loc, "replace() pattern is empty");
    brine_sb sb;
    brine_sb_init(&sb);
    int64_t at = 0;
    for (int64_t i; (i = br_find(s, a, at)) >= 0; at = i + a->len) {
        brine_sb_cat(&sb, s->data + at, i - at);
        brine_sb_str(&sb, b);
    }
    brine_sb_cat(&sb, s->data + at, s->len - at);
    return brine_sb_finish(&sb);
}
static brine_str *br_case(brine_str *s, bool up) {
    brine_str *r = brine_str_new(s->data, s->len);
    for (int64_t i = 0; i < r->len; i++) {
        char c = r->data[i];
        if (up && c >= 'a' && c <= 'z') r->data[i] = (char)(c - 32);
        if (!up && c >= 'A' && c <= 'Z') r->data[i] = (char)(c + 32);
    }
    return r;
}
brine_str *brine_s_upper(brine_str *s, const char *loc) {
    (void)loc;
    return br_case(s, true);
}
brine_str *brine_s_lower(brine_str *s, const char *loc) {
    (void)loc;
    return br_case(s, false);
}
brine_arr *brine_s_lines(brine_str *s, const char *loc) {
    (void)loc;
    brine_arr *out = br_strs();
    int64_t at = 0;
    while (at < s->len) {
        int64_t j = at;
        while (j < s->len && s->data[j] != '\n') j++;
        int64_t end = (j > at && s->data[j - 1] == '\r') ? j - 1 : j;
        br_push_str(&out, end > at ? br_sub(s, at, end) : brine_str_new("", 0));
        at = j + 1;
    }
    return out;
}
#define BR_CLASS(name, test)                                   \
    bool brine_s_##name(brine_str *s, const char *loc) {       \
        (void)loc;                                             \
        if (s->len == 0) return false;                         \
        for (int64_t i = 0; i < s->len; i++) {                 \
            char c = s->data[i];                               \
            if (!(test)) return false;                         \
        }                                                      \
        return true;                                           \
    }
BR_CLASS(is_digit, c >= '0' && c <= '9')
BR_CLASS(is_alpha, (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
BR_CLASS(is_space, br_ws(c))
BR_CLASS(is_alnum, (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
int64_t brine_s_count(brine_str *s, brine_str *sub, const char *loc) {
    if (sub->len == 0) brine_panic_at(loc, "count() pattern is empty");
    int64_t n = 0;
    for (int64_t at = 0, i; (i = br_find(s, sub, at)) >= 0; at = i + sub->len) n++;
    return n;
}
brine_arr *brine_s_bytes(brine_str *s, const char *loc) {
    brine_arr *a = brine_arr_new(s->len, 1, NULL, NULL, loc);
    memcpy(a->data, s->data, (size_t)s->len);
    return a;
}
brine_arr *brine_s_chars(brine_str *s, const char *loc) {
    (void)loc;
    brine_arr *out = br_strs();
    for (int64_t i = 0; i < s->len;) {
        int64_t n = br_utf8_len((const unsigned char *)s->data + i, s->len - i);
        br_push_str(&out, br_sub(s, i, i + n));
        i += n;
    }
    return out;
}
brine_str *brine_s_repeat(brine_str *s, int64_t n, const char *loc) {
    if (n < 0) br_panicf(loc, "repeat() count %lld is negative", (long long)n);
    brine_sb sb;
    brine_sb_init(&sb);
    for (int64_t i = 0; i < n; i++) brine_sb_str(&sb, s);
    return brine_sb_finish(&sb);
}
uint64_t brine_hash_str(brine_str *s) {
    uint64_t h = 0xcbf29ce484222325ULL; /* FNV-1a, then mixed */
    for (int64_t i = 0; i < s->len; i++) {
        h ^= (unsigned char)s->data[i];
        h *= 0x100000001b3ULL;
    }
    return brine_hash_u64(h ^ (uint64_t)s->len);
}
int brine_str_cmp(brine_str *a, brine_str *b) {
    int64_t n = a->len < b->len ? a->len : b->len;
    int c = memcmp(a->data, b->data, (size_t)n);
    if (c) return c;
    return a->len < b->len ? -1 : (a->len > b->len ? 1 : 0);
}
int64_t brine_parse_i64(brine_str *s, const char *loc) {
    char *end;
    errno = 0;
    long long v = strtoll(s->data, &end, 10);
    while (*end == ' ' || *end == '\n' || *end == '\r' || *end == '\t') end++;
    if (s->len == 0 || errno || end != s->data + s->len)
        br_panicf(loc, "invalid integer: \"%.*s\"", (int)s->len, s->data);
    return (int64_t)v;
}
/* 128-bit parsing by hand: the C library stops at 64 bits */
static bool br_parse_128(brine_str *s, bool *neg, brine_u128 *out) {
    const char *p = (const char *)s->data, *e = p + s->len;
    while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
    while (e > p && (e[-1] == ' ' || e[-1] == '\n' || e[-1] == '\r' || e[-1] == '\t')) e--;
    *neg = false;
    if (p < e && (*p == '+' || *p == '-')) *neg = *p++ == '-';
    if (p == e) return false;
    brine_u128 v = 0;
    for (; p < e; p++) {
        if (*p < '0' || *p > '9') return false;
        unsigned d = (unsigned)(*p - '0');
        if (v > (BR_U128_MAX - d) / 10) return false;
        v = v * 10 + d;
    }
    *out = v;
    return true;
}
brine_i128 brine_parse_i128(brine_str *s, const char *loc) {
    bool neg;
    brine_u128 u;
    if (!br_parse_128(s, &neg, &u) || u > (brine_u128)BR_I128_MAX + (neg ? 1 : 0))
        br_panicf(loc, "invalid integer: \"%.*s\"", (int)s->len, s->data);
    return neg ? (brine_i128)(0 - u) : (brine_i128)u;
}
brine_u128 brine_parse_u128(brine_str *s, const char *loc) {
    bool neg;
    brine_u128 u;
    if (!br_parse_128(s, &neg, &u) || (neg && u != 0)) br_panicf(loc, "invalid integer: \"%.*s\"", (int)s->len, s->data);
    return u;
}
double brine_parse_f64(brine_str *s, const char *loc) {
    char *end;
    errno = 0;
    double v = strtod(s->data, &end);
    while (*end == ' ' || *end == '\n' || *end == '\r' || *end == '\t') end++;
    if (s->len == 0 || end != s->data + s->len) br_panicf(loc, "invalid float: \"%.*s\"", (int)s->len, s->data);
    return v;
}

/* ============================================================ arrays */
brine_arr *brine_arr_new(int64_t len, int64_t esz, brine_elem_fn rt, brine_elem_fn rl, const char *loc) {
    if (len < 0) br_panicf(loc, "cannot create an array with negative length %lld", (long long)len);
    brine_arr *a = (brine_arr *)brine_alloc(sizeof(brine_arr));
    BR_LIVE(1);
    a->rc = 1;
    a->len = len;
    a->cap = len;
    a->esz = esz;
    a->retain = rt;
    a->release = rl;
    a->data = (char *)calloc(len > 0 ? (size_t)len : 1, (size_t)esz);
    if (!a->data) {
        fprintf(stderr, "out of memory\n");
        abort();
    }
    return a;
}
void brine_arr_unique(brine_arr **pa) {
    brine_arr *a = *pa;
    if (__atomic_load_n(&a->rc, __ATOMIC_ACQUIRE) == 1) return;
    brine_arr *n = (brine_arr *)brine_alloc(sizeof(brine_arr));
    *n = *a;
    BR_LIVE(1);
    n->rc = 1;
    n->cap = a->len;
    n->data = (char *)brine_alloc(a->len * a->esz);
    memcpy(n->data, a->data, (size_t)(a->len * a->esz));
    if (a->retain)
        for (int64_t i = 0; i < a->len; i++) a->retain(n->data + i * a->esz);
    brine_arr_release(a);
    *pa = n;
}
static void br_arr_reserve(brine_arr *a, int64_t need) {
    if (need <= a->cap) return;
    int64_t nc = a->cap ? a->cap * 2 : 4;
    if (nc < need) nc = need;
    char *d = (char *)realloc(a->data, (size_t)(nc * a->esz));
    if (!d) {
        fprintf(stderr, "out of memory\n");
        abort();
    }
    a->data = d;
    a->cap = nc;
}
void brine_arr_push(brine_arr **pa, const void *elem) {
    brine_arr_unique(pa);
    brine_arr *a = *pa;
    br_arr_reserve(a, a->len + 1);
    memcpy(a->data + a->len * a->esz, elem, (size_t)a->esz);
    a->len++;
}
void brine_arr_pop(brine_arr **pa, void *out, const char *loc) {
    if ((*pa)->len == 0) brine_panic_at(loc, "pop() from an empty array");
    brine_arr_unique(pa);
    brine_arr *a = *pa;
    a->len--;
    memcpy(out, a->data + a->len * a->esz, (size_t)a->esz);
}
void brine_arr_pop_at(brine_arr **pa, int64_t i, void *out, const char *loc) {
    brine_idx(i, (*pa)->len, loc);
    brine_arr_unique(pa);
    brine_arr *a = *pa;
    memcpy(out, a->data + i * a->esz, (size_t)a->esz);
    memmove(a->data + i * a->esz, a->data + (i + 1) * a->esz, (size_t)((a->len - i - 1) * a->esz));
    a->len--;
}
void brine_arr_insert(brine_arr **pa, int64_t i, const void *elem, const char *loc) {
    if (i < 0 || i > (*pa)->len) {
        if ((*pa)->release) (*pa)->release((void *)elem);
        br_panicf(loc, "insert index %lld is out of bounds for length %lld", (long long)i, (long long)(*pa)->len);
    }
    brine_arr_unique(pa);
    brine_arr *a = *pa;
    br_arr_reserve(a, a->len + 1);
    memmove(a->data + (i + 1) * a->esz, a->data + i * a->esz, (size_t)((a->len - i) * a->esz));
    memcpy(a->data + i * a->esz, elem, (size_t)a->esz);
    a->len++;
}
void brine_arr_extend(brine_arr **pa, brine_arr *b) {
    int64_t n = b->len;
    if (n == 0) return;
    brine_arr_retain(b); /* b may be *pa itself */
    brine_arr_unique(pa);
    brine_arr *a = *pa;
    br_arr_reserve(a, a->len + n);
    memcpy(a->data + a->len * a->esz, b->data, (size_t)(n * a->esz));
    if (a->retain)
        for (int64_t i = 0; i < n; i++) a->retain(a->data + (a->len + i) * a->esz);
    a->len += n;
    brine_arr_release(b);
}
void brine_arr_reverse(brine_arr **pa) {
    brine_arr_unique(pa);
    brine_arr *a = *pa;
    char tmp[64];
    char *t = a->esz <= 64 ? tmp : (char *)brine_alloc(a->esz);
    for (int64_t i = 0, j = a->len - 1; i < j; i++, j--) {
        memcpy(t, a->data + i * a->esz, (size_t)a->esz);
        memcpy(a->data + i * a->esz, a->data + j * a->esz, (size_t)a->esz);
        memcpy(a->data + j * a->esz, t, (size_t)a->esz);
    }
    if (t != tmp) free(t);
}
void brine_arr_clear(brine_arr **pa) {
    brine_arr *a = *pa;
    if (__atomic_load_n(&a->rc, __ATOMIC_ACQUIRE) != 1) {
        *pa = brine_arr_new(0, a->esz, a->retain, a->release, "");
        brine_arr_release(a);
        return;
    }
    if (a->release)
        for (int64_t i = 0; i < a->len; i++) a->release(a->data + i * a->esz);
    a->len = 0;
}
static void br_msort(char *d, char *tmp, int64_t n, int64_t esz, int (*cmp)(const void *, const void *)) {
    if (n < 2) return;
    int64_t h = n / 2;
    br_msort(d, tmp, h, esz, cmp);
    br_msort(d + h * esz, tmp, n - h, esz, cmp);
    if (cmp(d + (h - 1) * esz, d + h * esz) <= 0) return;
    memcpy(tmp, d, (size_t)(h * esz));
    int64_t i = 0, j = h, k = 0;
    while (i < h && j < n) {
        if (cmp(d + j * esz, tmp + i * esz) < 0) memcpy(d + k++ * esz, d + j++ * esz, (size_t)esz);
        else memcpy(d + k++ * esz, tmp + i++ * esz, (size_t)esz);
    }
    while (i < h) memcpy(d + k++ * esz, tmp + i++ * esz, (size_t)esz);
}
void brine_arr_sort(brine_arr **pa, int (*cmp)(const void *, const void *)) {
    if ((*pa)->len < 2) return;
    brine_arr_unique(pa);
    brine_arr *a = *pa;
    char *tmp = (char *)brine_alloc((a->len / 2 + 1) * a->esz);
    br_msort(a->data, tmp, a->len, a->esz, cmp);
    free(tmp);
}
brine_arr *brine_arr_slice(brine_arr *a, int64_t lo, int64_t hi, const char *loc) {
    if (BR_UNLIKELY(lo < 0 || lo > hi || hi > a->len))
        br_panicf(loc, "slice [%lld:%lld] is out of bounds for an array of length %lld", (long long)lo, (long long)hi, (long long)a->len);
    if (lo == 0 && hi == a->len) {
        brine_arr_retain(a);
        return a;
    }
    brine_arr *r = brine_arr_new(hi - lo, a->esz, a->retain, a->release, loc);
    memcpy(r->data, a->data + lo * a->esz, (size_t)((hi - lo) * a->esz));
    if (r->retain)
        for (int64_t i = 0; i < r->len; i++) r->retain(r->data + i * r->esz);
    return r;
}

/* ============================================================ maps */
void brine_rt_map(void *p) { brine_map_retain(*(brine_map **)p); }
void brine_rl_map(void *p) { brine_map_release(*(brine_map **)p); }

brine_map *brine_map_new(const brine_map_ty *ty) {
    brine_map *m = (brine_map *)brine_alloc(sizeof(brine_map));
    BR_LIVE(1);
    memset(m, 0, sizeof *m);
    m->rc = 1;
    m->ty = ty;
    return m;
}
void brine_map_free(brine_map *m) {
    BR_LIVE(-1);
    const brine_map_ty *t = m->ty;
    for (int64_t i = 0; i < m->used; i++) {
        if (!m->live[i]) continue;
        if (t->krelease) t->krelease(m->keys + i * t->ksz);
        if (t->vrelease) t->vrelease(m->vals + i * t->vsz);
    }
    free(m->keys);
    free(m->vals);
    free(m->hashes);
    free(m->live);
    free(m->index);
    free(m);
}
static void br_map_reindex(brine_map *m) {
    int64_t n = 8;
    while (n < 2 * m->cap) n *= 2;
    free(m->index);
    m->nidx = n;
    m->index = (int64_t *)brine_alloc(n * (int64_t)sizeof(int64_t));
    memset(m->index, 0xff, (size_t)n * sizeof(int64_t)); /* all -1 */
    for (int64_t e = 0; e < m->used; e++) {
        if (!m->live[e]) continue;
        uint64_t i = m->hashes[e] & (uint64_t)(n - 1);
        while (m->index[i] != -1) i = (i + 1) & (uint64_t)(n - 1);
        m->index[i] = e;
    }
}
/* rebuilds the entry arrays with room for `cap` entries, dropping removed ones */
static void br_map_resize(brine_map *m, int64_t cap) {
    const brine_map_ty *t = m->ty;
    char *k = (char *)brine_alloc(cap * t->ksz + 1);
    char *v = (char *)brine_alloc(cap * t->vsz + 1);
    uint64_t *h = (uint64_t *)brine_alloc(cap * (int64_t)sizeof(uint64_t));
    bool *l = (bool *)brine_alloc(cap);
    int64_t j = 0;
    for (int64_t i = 0; i < m->used; i++) {
        if (!m->live[i]) continue;
        memcpy(k + j * t->ksz, m->keys + i * t->ksz, (size_t)t->ksz);
        memcpy(v + j * t->vsz, m->vals + i * t->vsz, (size_t)t->vsz);
        h[j] = m->hashes[i];
        l[j] = true;
        j++;
    }
    free(m->keys);
    free(m->vals);
    free(m->hashes);
    free(m->live);
    m->keys = k;
    m->vals = v;
    m->hashes = h;
    m->live = l;
    m->used = j;
    m->cap = cap;
    br_map_reindex(m);
}
void brine_map_unique(brine_map **pm) {
    brine_map *a = *pm;
    if (__atomic_load_n(&a->rc, __ATOMIC_ACQUIRE) == 1) return;
    const brine_map_ty *t = a->ty;
    brine_map *n = brine_map_new(t);
    /* copy the entry arrays; br_map_resize then compacts them and builds the index */
    n->used = a->used;
    n->keys = (char *)brine_alloc(a->used * t->ksz + 1);
    n->vals = (char *)brine_alloc(a->used * t->vsz + 1);
    n->hashes = (uint64_t *)brine_alloc(a->used * (int64_t)sizeof(uint64_t) + 1);
    n->live = (bool *)brine_alloc(a->used + 1);
    memcpy(n->keys, a->keys, (size_t)(a->used * t->ksz));
    memcpy(n->vals, a->vals, (size_t)(a->used * t->vsz));
    memcpy(n->hashes, a->hashes, (size_t)a->used * sizeof(uint64_t));
    memcpy(n->live, a->live, (size_t)a->used);
    n->len = a->len;
    br_map_resize(n, a->len > 4 ? a->len : 4);
    for (int64_t i = 0; i < n->used; i++) {
        if (t->kretain) t->kretain(n->keys + i * t->ksz);
        if (t->vretain) t->vretain(n->vals + i * t->vsz);
    }
    brine_map_release(a);
    *pm = n;
}
static int64_t br_map_lookup(const brine_map *m, const void *k, uint64_t h) {
    if (m->len == 0) return -1;
    uint64_t mask = (uint64_t)(m->nidx - 1);
    for (uint64_t i = h & mask;; i = (i + 1) & mask) {
        int64_t e = m->index[i];
        if (e == -1) return -1;
        if (e >= 0 && m->hashes[e] == h && m->ty->keq(m->keys + e * m->ty->ksz, k)) return e;
    }
}
int64_t brine_map_find(const brine_map *m, const void *k) {
    if (m->len == 0) return -1;
    return br_map_lookup(m, k, m->ty->hash(k));
}
BR_NORETURN static void br_map_missing(const brine_map *m, const void *k, const char *loc) {
    brine_sb sb;
    brine_sb_init(&sb);
    brine_sb_cat(&sb, "key ", 4);
    m->ty->kfmt(&sb, k);
    brine_sb_cat(&sb, " is not in the map", 18);
    brine_sb_cat(&sb, "", 1);
    brine_panic_at(loc, sb.p);
}
void *brine_map_at(brine_map *m, const void *k, const char *loc) {
    int64_t e = brine_map_find(m, k);
    if (BR_UNLIKELY(e < 0)) br_map_missing(m, k, loc);
    return m->vals + e * m->ty->vsz;
}
void *brine_map_at_mut(brine_map **pm, const void *k, const char *loc) {
    int64_t e = brine_map_find(*pm, k);
    if (BR_UNLIKELY(e < 0)) br_map_missing(*pm, k, loc);
    brine_map *before = *pm;
    brine_map_unique(pm);
    if (*pm != before) e = brine_map_find(*pm, k); /* copying compacts the entries */
    return (*pm)->vals + e * (*pm)->ty->vsz;
}
void brine_map_set(brine_map **pm, void *k, void *v) {
    brine_map_unique(pm);
    brine_map *m = *pm;
    const brine_map_ty *t = m->ty;
    uint64_t h = t->hash(k);
    int64_t e = br_map_lookup(m, k, h);
    if (e >= 0) {
        if (t->krelease) t->krelease(k);
        if (t->vrelease) t->vrelease(m->vals + e * t->vsz);
        memcpy(m->vals + e * t->vsz, v, (size_t)t->vsz);
        return;
    }
    if (m->used == m->cap) br_map_resize(m, m->len * 2 > 8 ? m->len * 2 : 8);
    e = m->used++;
    memcpy(m->keys + e * t->ksz, k, (size_t)t->ksz);
    memcpy(m->vals + e * t->vsz, v, (size_t)t->vsz);
    m->hashes[e] = h;
    m->live[e] = true;
    m->len++;
    uint64_t mask = (uint64_t)(m->nidx - 1);
    uint64_t i = h & mask;
    while (m->index[i] >= 0) i = (i + 1) & mask;
    m->index[i] = e;
}
bool brine_map_remove(brine_map **pm, const void *k) {
    if (brine_map_find(*pm, k) < 0) return false;
    brine_map_unique(pm);
    brine_map *m = *pm;
    const brine_map_ty *t = m->ty;
    uint64_t h = t->hash(k);
    uint64_t mask = (uint64_t)(m->nidx - 1);
    for (uint64_t i = h & mask;; i = (i + 1) & mask) {
        int64_t e = m->index[i];
        if (e >= 0 && m->hashes[e] == h && t->keq(m->keys + e * t->ksz, k)) {
            m->index[i] = -2;
            m->live[e] = false;
            m->len--;
            if (t->krelease) t->krelease(m->keys + e * t->ksz);
            if (t->vrelease) t->vrelease(m->vals + e * t->vsz);
            return true;
        }
    }
}
static brine_arr *br_map_column(brine_map *m, bool keys) {
    const brine_map_ty *t = m->ty;
    int64_t sz = keys ? t->ksz : t->vsz;
    brine_arr *a = brine_arr_new(m->len, sz, keys ? t->kretain : t->vretain, keys ? t->krelease : t->vrelease, "");
    char *src = keys ? m->keys : m->vals;
    int64_t j = 0;
    for (int64_t i = 0; i < m->used; i++) {
        if (!m->live[i]) continue;
        memcpy(a->data + j * sz, src + i * sz, (size_t)sz);
        if (a->retain) a->retain(a->data + j * sz);
        j++;
    }
    return a;
}
brine_arr *brine_map_keys(brine_map *m) { return br_map_column(m, true); }
brine_arr *brine_map_values(brine_map *m) { return br_map_column(m, false); }
bool brine_map_eq(brine_map *a, brine_map *b) {
    if (a == b) return true;
    if (a->len != b->len) return false;
    const brine_map_ty *t = a->ty;
    for (int64_t i = 0; i < a->used; i++) {
        if (!a->live[i]) continue;
        int64_t e = br_map_lookup(b, a->keys + i * t->ksz, a->hashes[i]);
        if (e < 0 || !t->veq(a->vals + i * t->vsz, b->vals + e * t->vsz)) return false;
    }
    return true;
}

/* ============================================================ string builder */
void brine_sb_init(brine_sb *sb) {
    sb->cap = 64;
    sb->len = 0;
    sb->p = (char *)brine_alloc(sb->cap);
}
static void br_sb_reserve(brine_sb *sb, int64_t extra) {
    if (sb->len + extra <= sb->cap) return;
    int64_t nc = sb->cap * 2;
    while (nc < sb->len + extra) nc *= 2;
    sb->p = (char *)realloc(sb->p, (size_t)nc);
    if (!sb->p) abort();
    sb->cap = nc;
}
void brine_sb_cat(brine_sb *sb, const char *p, int64_t n) {
    br_sb_reserve(sb, n);
    memcpy(sb->p + sb->len, p, (size_t)n);
    sb->len += n;
}
static void br_sb_cstr(brine_sb *sb, const char *s) { brine_sb_cat(sb, s, (int64_t)strlen(s)); }
void brine_sb_str(brine_sb *sb, brine_str *s) { brine_sb_cat(sb, s->data, s->len); }
void brine_sb_repr_str(brine_sb *sb, brine_str *s) {
    brine_sb_cat(sb, "\"", 1);
    for (int64_t i = 0; i < s->len; i++) {
        unsigned char c = (unsigned char)s->data[i];
        switch (c) {
        case '"': br_sb_cstr(sb, "\\\""); break;
        case '\\': br_sb_cstr(sb, "\\\\"); break;
        case '\n': br_sb_cstr(sb, "\\n"); break;
        case '\t': br_sb_cstr(sb, "\\t"); break;
        case '\r': br_sb_cstr(sb, "\\r"); break;
        default:
            if (c < 0x20) {
                char b[16];
                snprintf(b, sizeof b, "\\u{%x}", c);
                br_sb_cstr(sb, b);
            } else
                brine_sb_cat(sb, (const char *)&s->data[i], 1);
        }
    }
    brine_sb_cat(sb, "\"", 1);
}
void brine_sb_i64(brine_sb *sb, int64_t v) {
    char b[32];
    int n = snprintf(b, sizeof b, "%lld", (long long)v);
    brine_sb_cat(sb, b, n);
}
void brine_sb_u64(brine_sb *sb, uint64_t v) {
    char b[32];
    int n = snprintf(b, sizeof b, "%llu", (unsigned long long)v);
    brine_sb_cat(sb, b, n);
}
static void br_sb_u128_base(brine_sb *sb, brine_u128 v, unsigned base) {
    char b[130];
    int n = sizeof b;
    do {
        unsigned d = (unsigned)(v % base);
        b[--n] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        v /= base;
    } while (v);
    brine_sb_cat(sb, b + n, (int64_t)(sizeof b - n));
}
void brine_sb_u128(brine_sb *sb, brine_u128 v) { br_sb_u128_base(sb, v, 10); }
void brine_sb_i128(brine_sb *sb, brine_i128 v) {
    if (v < 0) brine_sb_cat(sb, "-", 1);
    br_sb_u128_base(sb, v < 0 ? (brine_u128)(-(v + 1)) + 1 : (brine_u128)v, 10);
}
void brine_sb_fmt_u128(brine_sb *sb, brine_u128 v, int kind) {
    if (kind == 'x' || kind == 'b')
        br_sb_u128_base(sb, v, kind == 'x' ? 16 : 2);
    else if (kind == 'f' || kind == 'e')
        brine_sb_fmt_f64(sb, (double)v, -1, kind);
    else
        brine_sb_u128(sb, v);
}
void brine_sb_fmt_i128(brine_sb *sb, brine_i128 v, int kind) {
    if (kind == 'x' || kind == 'b') {
        if (v < 0) brine_sb_cat(sb, "-", 1);
        br_sb_u128_base(sb, v < 0 ? (brine_u128)(-(v + 1)) + 1 : (brine_u128)v, kind == 'x' ? 16 : 2);
    } else if (kind == 'f' || kind == 'e')
        brine_sb_fmt_f64(sb, (double)v, -1, kind);
    else
        brine_sb_i128(sb, v);
}
/* shortest round-trip representation, Python style */
static void br_fmt_float(brine_sb *sb, double v, int maxprec, int is_f32) {
    char e[64], buf[400];
    if (isnan(v)) {
        br_sb_cstr(sb, "nan");
        return;
    }
    if (isinf(v)) {
        br_sb_cstr(sb, v < 0 ? "-inf" : "inf");
        return;
    }
    int p;
    for (p = 1; p <= maxprec; p++) {
        snprintf(e, sizeof e, "%.*e", p - 1, v);
        double back = strtod(e, NULL);
        if (is_f32 ? ((float)back == (float)v) : (back == v)) break;
    }
    if (p > maxprec) p = maxprec;
    snprintf(e, sizeof e, "%.*e", p - 1, v);
    char *ep = strchr(e, 'e');
    int x = ep ? atoi(ep + 1) : 0;
    if (x >= -4 && x < 16) {
        int dec = p - 1 - x;
        if (dec < 0) dec = 0;
        snprintf(buf, sizeof buf, "%.*f", dec, v);
        if (!strchr(buf, '.')) strcat(buf, ".0");
        br_sb_cstr(sb, buf);
    } else {
        br_sb_cstr(sb, e);
    }
}
void brine_sb_f64(brine_sb *sb, double v) { br_fmt_float(sb, v, 17, 0); }
void brine_sb_f32(brine_sb *sb, float v) { br_fmt_float(sb, (double)v, 9, 1); }
void brine_sb_bool(brine_sb *sb, bool v) { br_sb_cstr(sb, v ? "true" : "false"); }
static void br_sb_bin(brine_sb *sb, uint64_t v) {
    char b[65];
    int n = 0;
    do {
        b[n++] = (char)('0' + (v & 1));
        v >>= 1;
    } while (v);
    for (int i = n - 1; i >= 0; i--) brine_sb_cat(sb, &b[i], 1);
}
void brine_sb_fmt_i64(brine_sb *sb, int64_t v, int kind) {
    if (kind == 'x' || kind == 'b') {
        uint64_t u = v < 0 ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
        if (v < 0) br_sb_cstr(sb, "-");
        if (kind == 'x') {
            char b[32];
            snprintf(b, sizeof b, "%llx", (unsigned long long)u);
            br_sb_cstr(sb, b);
        } else
            br_sb_bin(sb, u);
        return;
    }
    if (kind == 'f' || kind == 'e') {
        brine_sb_fmt_f64(sb, (double)v, -1, kind);
        return;
    }
    brine_sb_i64(sb, v);
}
void brine_sb_fmt_u64(brine_sb *sb, uint64_t v, int kind) {
    if (kind == 'x') {
        char b[32];
        snprintf(b, sizeof b, "%llx", (unsigned long long)v);
        br_sb_cstr(sb, b);
    } else if (kind == 'b')
        br_sb_bin(sb, v);
    else if (kind == 'f' || kind == 'e')
        brine_sb_fmt_f64(sb, (double)v, -1, kind);
    else
        brine_sb_u64(sb, v);
}
void brine_sb_fmt_f64(brine_sb *sb, double v, int prec, int kind) {
    char b[400];
    if (kind == 'f') {
        snprintf(b, sizeof b, "%.*f", prec < 0 ? 6 : prec, v);
        br_sb_cstr(sb, b);
    } else if (kind == 'e') {
        snprintf(b, sizeof b, "%.*e", prec < 0 ? 6 : prec, v);
        br_sb_cstr(sb, b);
    } else
        brine_sb_f64(sb, v);
}
void brine_sb_pad(brine_sb *sb, int64_t start, int64_t width, bool left, bool zero) {
    int64_t n = sb->len - start;
    if (n >= width) return;
    int64_t pad = width - n;
    br_sb_reserve(sb, pad);
    if (left) {
        memset(sb->p + sb->len, ' ', (size_t)pad);
    } else {
        int64_t at = start;
        if (zero && n > 0 && (sb->p[start] == '-' || sb->p[start] == '+')) at++;
        memmove(sb->p + at + pad, sb->p + at, (size_t)(sb->len - at));
        memset(sb->p + at, zero ? '0' : ' ', (size_t)pad);
    }
    sb->len += pad;
}
brine_str *brine_sb_finish(brine_sb *sb) {
    brine_str *s = brine_str_new(sb->p, sb->len);
    free(sb->p);
    sb->p = NULL;
    return s;
}
void brine_print_sb(brine_sb *sb) {
    brine_sb_cat(sb, "\n", 1);
    br_lock(&br_io_mu);
    fwrite(sb->p, 1, (size_t)sb->len, stdout);
    br_unlock(&br_io_mu);
    free(sb->p);
    sb->p = NULL;
}
void brine_eprint_sb(brine_sb *sb) {
    brine_sb_cat(sb, "\n", 1);
    br_lock(&br_io_mu);
    fflush(stdout);
    fwrite(sb->p, 1, (size_t)sb->len, stderr);
    br_unlock(&br_io_mu);
    free(sb->p);
    sb->p = NULL;
}

/* ============================================================ misc */
double brine_clock(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}
brine_arr *brine_args(void) {
    brine_arr *a = brine_arr_new(0, sizeof(brine_str *), brine_rt_str, brine_rl_str, "<args>");
    for (int i = 1; i < br_argc; i++) {
        brine_str *s = brine_str_new(br_argv[i], (int64_t)strlen(br_argv[i]));
        brine_arr_push(&a, &s);
    }
    return a;
}

/* ============================================================ thread pool */
typedef struct br_job {
    brine_chunk_fn fn;
    void *ctx;
    int64_t lo, n, k;
    int64_t next;
    int refs;
    uint64_t id;
} br_job;

static struct {
    br_mutex mu;
    br_cond cv_work, cv_done;
    br_mutex job_lock;
    int nthreads;
    int started;
    br_job *cur;
    uint64_t ids;
} br_pool;

static _Thread_local int br_in_par = 0;
static int64_t br_task_budget = 0;

static int br_threads(void) {
    if (br_pool.nthreads == 0) {
        const char *e = getenv("BRINE_THREADS");
        int n = e ? atoi(e) : 0;
        if (n <= 0) n = br_ncpu();
        if (n > 1024) n = 1024;
        br_pool.nthreads = n;
        __atomic_store_n(&br_task_budget, (int64_t)(n - 1), __ATOMIC_SEQ_CST);
    }
    return br_pool.nthreads;
}

static void br_run_chunks(br_job *j) {
    int64_t base = j->n / j->k, rem = j->n % j->k;
    for (;;) {
        int64_t c = __atomic_fetch_add(&j->next, 1, __ATOMIC_SEQ_CST);
        if (c >= j->k) break;
        int64_t a = c * base + (c < rem ? c : rem);
        int64_t b = a + base + (c < rem ? 1 : 0);
        j->fn(j->ctx, c, j->lo + a, j->lo + b);
    }
}

static void br_worker(void *arg) {
    (void)arg;
    br_in_par = 1;
    uint64_t last = 0;
    br_lock(&br_pool.mu);
    for (;;) {
        while (!(br_pool.cur && br_pool.cur->id != last)) br_cond_wait(&br_pool.cv_work, &br_pool.mu);
        br_job *j = br_pool.cur;
        last = j->id;
        j->refs++;
        br_unlock(&br_pool.mu);
        br_run_chunks(j);
        br_lock(&br_pool.mu);
        j->refs--;
        br_cond_broadcast(&br_pool.cv_done);
    }
}

void brine_par_run(int64_t lo, int64_t hi, int64_t nchunks, brine_chunk_fn fn, void *ctx) {
    if (hi <= lo || nchunks <= 0) return;
    br_job job;
    job.fn = fn;
    job.ctx = ctx;
    job.lo = lo;
    job.n = hi - lo;
    job.k = nchunks;
    job.next = 0;
    job.refs = 0;
    if (nchunks == 1 || br_in_par || br_threads() <= 1 || !br_trylock(&br_pool.job_lock)) {
        br_run_chunks(&job);
        return;
    }
    if (!br_pool.started) {
        br_pool.started = 1;
        for (int i = 0; i < br_pool.nthreads - 1; i++) {
            br_thread th;
            br_thread_start(&th, br_worker, NULL);
        }
    }
    br_lock(&br_pool.mu);
    job.id = ++br_pool.ids;
    br_pool.cur = &job;
    br_cond_broadcast(&br_pool.cv_work);
    br_unlock(&br_pool.mu);
    br_in_par = 1;
    br_run_chunks(&job);
    br_in_par = 0;
    br_lock(&br_pool.mu);
    br_pool.cur = NULL;
    while (job.refs > 0) br_cond_wait(&br_pool.cv_done, &br_pool.mu);
    br_unlock(&br_pool.mu);
    br_unlock(&br_pool.job_lock);
}

typedef struct {
    brine_task_fn fn;
    void *ctx;
    int64_t idx;
} br_task;

static void br_task_main(void *p) {
    br_task *t = (br_task *)p;
    t->fn(t->ctx, t->idx);
}

void brine_par_tasks(int64_t k, brine_task_fn fn, void *ctx) {
    br_threads();
    br_task *tasks = (br_task *)brine_alloc(k * (int64_t)sizeof(br_task));
    br_thread *ths = (br_thread *)brine_alloc(k * (int64_t)sizeof(br_thread));
    char *spawned = (char *)brine_alloc(k);
    for (int64_t i = 0; i < k; i++) {
        tasks[i].fn = fn;
        tasks[i].ctx = ctx;
        tasks[i].idx = i;
        spawned[i] = 0;
    }
    /* spawn all but the last task while thread budget remains */
    for (int64_t i = 0; i + 1 < k; i++) {
        if (__atomic_sub_fetch(&br_task_budget, 1, __ATOMIC_SEQ_CST) >= 0) {
            if (br_thread_start(&ths[i], br_task_main, &tasks[i]))
                spawned[i] = 1;
            else
                __atomic_add_fetch(&br_task_budget, 1, __ATOMIC_SEQ_CST);
        } else {
            __atomic_add_fetch(&br_task_budget, 1, __ATOMIC_SEQ_CST);
        }
    }
    for (int64_t i = 0; i < k; i++)
        if (!spawned[i]) fn(ctx, i);
    for (int64_t i = 0; i < k; i++)
        if (spawned[i]) {
            br_thread_join(ths[i]);
            __atomic_add_fetch(&br_task_budget, 1, __ATOMIC_SEQ_CST);
        }
    free(tasks);
    free(ths);
    free(spawned);
}

/* ============================================================ world: transport */
enum { BR_CH_USER = 0, BR_CH_COLL = 1 };
enum { BR_K_EMPTY = 0, BR_K_I64 = 1, BR_K_F64 = 2, BR_K_BOOL = 3, BR_K_STR = 4, BR_K_AI64 = 5, BR_K_AF64 = 6, BR_K_MSG = 7 };
static const char *br_kind_name(int64_t k) {
    switch (k) {
    case BR_K_I64: return "int";
    case BR_K_F64: return "float";
    case BR_K_BOOL: return "bool";
    case BR_K_STR: return "str";
    case BR_K_AI64: return "[int]";
    case BR_K_AF64: return "[float]";
    case BR_K_MSG: return "message";
    default: return "(internal)";
    }
}

typedef struct br_msg {
    struct br_msg *next;
    int64_t chan, kind, len;
    char data[];
} br_msg;

typedef struct {
    br_sock sock;
    br_mutex mu, send_mu;
    br_cond cv;
    br_msg *head, *tail;
    int closed;
    int64_t rank;
} br_peer;

static br_peer *br_peers;

static int br_write_all(br_sock s, const char *p, int64_t n) {
    while (n > 0) {
        int chunk = n > (1 << 30) ? (1 << 30) : (int)n;
#ifdef _WIN32
        int w = send(s, p, chunk, 0);
#elif defined(MSG_NOSIGNAL)
        ssize_t w = send(s, p, (size_t)chunk, MSG_NOSIGNAL);
#else
        ssize_t w = send(s, p, (size_t)chunk, 0);
#endif
        if (w <= 0) {
#ifndef _WIN32
            if (w < 0 && errno == EINTR) continue;
#endif
            return 0;
        }
        p += w;
        n -= w;
    }
    return 1;
}
static int br_read_all(br_sock s, char *p, int64_t n) {
    while (n > 0) {
        int chunk = n > (1 << 30) ? (1 << 30) : (int)n;
#ifdef _WIN32
        int r = recv(s, p, chunk, 0);
#else
        ssize_t r = recv(s, p, (size_t)chunk, 0);
#endif
        if (r <= 0) {
#ifndef _WIN32
            if (r < 0 && errno == EINTR) continue;
#endif
            return 0;
        }
        p += r;
        n -= r;
    }
    return 1;
}

static void br_enqueue(br_peer *pr, br_msg *m) {
    m->next = NULL;
    br_lock(&pr->mu);
    if (pr->tail)
        pr->tail->next = m;
    else
        pr->head = m;
    pr->tail = m;
    br_cond_broadcast(&pr->cv);
    br_unlock(&pr->mu);
}

static void br_receiver(void *arg) {
    br_peer *pr = (br_peer *)arg;
    for (;;) {
        int64_t hdr[3];
        if (!br_read_all(pr->sock, (char *)hdr, sizeof hdr)) break;
        if (hdr[2] < 0) break;
        br_msg *m = (br_msg *)brine_alloc((int64_t)sizeof(br_msg) + hdr[2]);
        m->chan = hdr[0];
        m->kind = hdr[1];
        m->len = hdr[2];
        if (hdr[2] && !br_read_all(pr->sock, m->data, hdr[2])) {
            free(m);
            break;
        }
        br_enqueue(pr, m);
    }
    br_lock(&pr->mu);
    pr->closed = 1;
    br_cond_broadcast(&pr->cv);
    br_unlock(&pr->mu);
}

static void br_check_rank(int64_t r, const char *what, const char *loc) {
    if (r < 0 || r >= br_size)
        br_panicf(loc, "%s rank %lld is out of range (world size is %lld)", what, (long long)r, (long long)br_size);
}

static void br_send(int64_t dest, int64_t chan, int64_t kind, const void *data, int64_t len, const char *loc) {
    br_check_rank(dest, "destination", loc);
    br_peer *pr = &br_peers[dest];
    if (dest == br_rank) {
        br_msg *m = (br_msg *)brine_alloc((int64_t)sizeof(br_msg) + len);
        m->chan = chan;
        m->kind = kind;
        m->len = len;
        if (len) memcpy(m->data, data, (size_t)len);
        br_enqueue(pr, m);
        return;
    }
    int64_t hdr[3] = {chan, kind, len};
    br_lock(&pr->send_mu);
    int ok = br_write_all(pr->sock, (const char *)hdr, sizeof hdr) && (len == 0 || br_write_all(pr->sock, (const char *)data, len));
    br_unlock(&pr->send_mu);
    if (!ok) br_panicf(loc, "failed to send to rank %lld (did it exit?)", (long long)dest);
}

static br_msg *br_recv(int64_t src, int64_t chan, int64_t kind, const char *loc) {
    br_check_rank(src, "source", loc);
    br_peer *pr = &br_peers[src];
    br_lock(&pr->mu);
    for (;;) {
        br_msg *prev = NULL;
        for (br_msg *m = pr->head; m; prev = m, m = m->next) {
            if (m->chan != chan) continue;
            if (prev)
                prev->next = m->next;
            else
                pr->head = m->next;
            if (pr->tail == m) pr->tail = prev;
            br_unlock(&pr->mu);
            if (m->kind != kind)
                br_panicf(loc, "type mismatch: rank %lld sent a value of type %s, but this receive expects %s", (long long)src,
                          br_kind_name(m->kind), br_kind_name(kind));
            return m;
        }
        if (pr->closed) {
            br_unlock(&pr->mu);
            br_panicf(loc, "rank %lld exited before sending the expected message", (long long)src);
        }
        br_cond_wait(&pr->cv, &pr->mu);
    }
}

static br_sock br_connect(const char *host, const char *port) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) return BR_BAD_SOCK;
    br_sock s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == BR_BAD_SOCK) {
        freeaddrinfo(res);
        return BR_BAD_SOCK;
    }
    if (connect(s, res->ai_addr, (int)res->ai_addrlen) != 0) {
        br_sock_close(s);
        freeaddrinfo(res);
        return BR_BAD_SOCK;
    }
    freeaddrinfo(res);
    return s;
}

static void br_nodelay(br_sock s) {
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
}

static void br_world_init(void) {
    const char *er = getenv("BRINE_RANK"), *es = getenv("BRINE_WORLD_SIZE"), *ep = getenv("BRINE_PEERS");
    if (!er || !es || !ep) return;
    br_rank = atoll(er);
    br_size = atoll(es);
    if (br_size <= 1) {
        br_size = 1;
        br_rank = 0;
        return;
    }
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#else
    signal(SIGPIPE, SIG_IGN);
#endif
    br_peers = (br_peer *)calloc((size_t)br_size, sizeof(br_peer));
    char **hosts = (char **)calloc((size_t)br_size, sizeof(char *));
    char **ports = (char **)calloc((size_t)br_size, sizeof(char *));
    char *list = strdup(ep);
    char *p = list;
    for (int64_t i = 0; i < br_size; i++) {
        char *comma = strchr(p, ',');
        if (comma) *comma = 0;
        char *colon = strrchr(p, ':');
        if (!colon) {
            fprintf(stderr, "brine: invalid BRINE_PEERS entry '%s'\n", p);
            exit(2);
        }
        *colon = 0;
        hosts[i] = p;
        ports[i] = colon + 1;
        p = comma ? comma + 1 : p + strlen(p);
    }
    for (int64_t i = 0; i < br_size; i++) {
        br_mutex_init(&br_peers[i].mu);
        br_mutex_init(&br_peers[i].send_mu);
        br_cond_init(&br_peers[i].cv);
        br_peers[i].rank = i;
        br_peers[i].sock = BR_BAD_SOCK;
    }
    /* listen on our own port */
    br_sock ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)atoi(ports[br_rank]));
    if (bind(ls, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(ls, (int)br_size + 4) != 0) {
        fprintf(stderr, "brine: rank %lld could not listen on port %s\n", (long long)br_rank, ports[br_rank]);
        exit(2);
    }
    /* connect to lower ranks */
    for (int64_t r = 0; r < br_rank; r++) {
        br_sock s = BR_BAD_SOCK;
        for (int attempt = 0; attempt < 600 && s == BR_BAD_SOCK; attempt++) {
            s = br_connect(hosts[r], ports[r]);
            if (s == BR_BAD_SOCK) br_sleep_ms(50);
        }
        if (s == BR_BAD_SOCK) {
            fprintf(stderr, "brine: rank %lld could not connect to rank %lld at %s:%s\n", (long long)br_rank, (long long)r, hosts[r],
                    ports[r]);
            exit(2);
        }
        br_nodelay(s);
        int64_t me = br_rank;
        br_write_all(s, (const char *)&me, sizeof me);
        br_peers[r].sock = s;
    }
    /* accept higher ranks */
    for (int64_t n = br_rank + 1; n < br_size; n++) {
        br_sock s = accept(ls, NULL, NULL);
        if (s == BR_BAD_SOCK) {
            fprintf(stderr, "brine: accept failed on rank %lld\n", (long long)br_rank);
            exit(2);
        }
        br_nodelay(s);
        int64_t who = -1;
        br_read_all(s, (char *)&who, sizeof who);
        if (who <= br_rank || who >= br_size) {
            fprintf(stderr, "brine: bad handshake on rank %lld\n", (long long)br_rank);
            exit(2);
        }
        br_peers[who].sock = s;
    }
    br_sock_close(ls);
    for (int64_t r = 0; r < br_size; r++) {
        if (r == br_rank) continue;
        br_thread th;
        br_thread_start(&th, br_receiver, &br_peers[r]);
    }
    free(hosts);
    free(ports);
}

/* ============================================================ world: API */
int64_t brine_world_rank(void) { return br_rank; }
int64_t brine_world_size(void) { return br_size; }

void brine_world_barrier(void) {
    if (br_size <= 1) return;
    const char *loc = "<barrier>";
    if (br_rank == 0) {
        for (int64_t r = 1; r < br_size; r++) free(br_recv(r, BR_CH_COLL, BR_K_EMPTY, loc));
        for (int64_t r = 1; r < br_size; r++) br_send(r, BR_CH_COLL, BR_K_EMPTY, NULL, 0, loc);
    } else {
        br_send(0, BR_CH_COLL, BR_K_EMPTY, NULL, 0, loc);
        free(br_recv(0, BR_CH_COLL, BR_K_EMPTY, loc));
    }
}

static int64_t br_get8(br_msg *m) {
    int64_t v;
    memcpy(&v, m->data, 8);
    free(m);
    return v;
}
static brine_arr *br_msg_arr(br_msg *m) {
    brine_arr *a = brine_arr_new(m->len / 8, 8, NULL, NULL, "<recv>");
    memcpy(a->data, m->data, (size_t)m->len);
    free(m);
    return a;
}
/* Messages are encoded from a type description string, which the compiler
 * derives from the static type:  i = int, f = float, b = bool, s = str,
 * a<T> = array of <T>.  Every rank reaches the same collective call (the
 * compiler checks this), so both sides always agree on the description. */
typedef struct {
    char *p;
    int64_t len, cap;
} br_buf;

static void br_buf_put(br_buf *b, const void *data, int64_t n) {
    if (b->len + n > b->cap) {
        int64_t c = b->cap ? b->cap * 2 : 64;
        while (c < b->len + n) c *= 2;
        b->p = (char *)realloc(b->p, (size_t)c);
        if (!b->p) brine_panic_at("<world>", "out of memory");
        b->cap = c;
    }
    if (n) memcpy(b->p + b->len, data, (size_t)n);
    b->len += n;
}

static const char *br_desc_skip(const char *d) {
    while (*d == 'a') d++;
    return d + 1;
}
static int64_t br_desc_size(const char *d) {
    switch (*d) {
    case 'b': return (int64_t)sizeof(bool);
    case 's': return (int64_t)sizeof(brine_str *);
    case 'a': return (int64_t)sizeof(brine_arr *);
    default: return 8;
    }
}
static void br_desc_fns(const char *d, brine_elem_fn *rt, brine_elem_fn *rl) {
    if (*d == 's') {
        *rt = brine_rt_str;
        *rl = brine_rl_str;
    } else if (*d == 'a') {
        *rt = brine_rt_arr;
        *rl = brine_rl_arr;
    } else {
        *rt = NULL;
        *rl = NULL;
    }
}

static void br_encode(br_buf *b, const char *d, const void *v) {
    switch (*d) {
    case 'i':
    case 'f': br_buf_put(b, v, 8); break;
    case 'b': {
        char c = *(const bool *)v ? 1 : 0;
        br_buf_put(b, &c, 1);
        break;
    }
    case 's': {
        const brine_str *s = *(brine_str *const *)v;
        br_buf_put(b, &s->len, 8);
        br_buf_put(b, s->data, s->len);
        break;
    }
    case 'a': {
        const brine_arr *a = *(brine_arr *const *)v;
        br_buf_put(b, &a->len, 8);
        const char *e = d + 1;
        if (*e == 'i' || *e == 'f')
            br_buf_put(b, a->data, a->len * 8);
        else
            for (int64_t i = 0; i < a->len; i++) br_encode(b, e, a->data + i * a->esz);
        break;
    }
    }
}

/* decode one value at *p into `out` (an owned reference for str/arrays) */
static void br_decode(const char **p, const char *d, void *out, const char *loc) {
    switch (*d) {
    case 'i':
    case 'f':
        memcpy(out, *p, 8);
        *p += 8;
        break;
    case 'b':
        *(bool *)out = **p != 0;
        *p += 1;
        break;
    case 's': {
        int64_t n;
        memcpy(&n, *p, 8);
        *p += 8;
        *(brine_str **)out = brine_str_new(*p, n);
        *p += n;
        break;
    }
    case 'a': {
        int64_t n;
        memcpy(&n, *p, 8);
        *p += 8;
        const char *e = d + 1;
        brine_elem_fn rt, rl;
        br_desc_fns(e, &rt, &rl);
        brine_arr *a = brine_arr_new(n, br_desc_size(e), rt, rl, loc);
        if (*e == 'i' || *e == 'f') {
            memcpy(a->data, *p, (size_t)(n * 8));
            *p += n * 8;
        } else {
            for (int64_t i = 0; i < n; i++) br_decode(p, e, a->data + i * a->esz, loc);
        }
        *(brine_arr **)out = a;
        break;
    }
    }
}

/* copy a value into `out`, taking a new reference */
static void br_copy_val(const char *d, const void *v, void *out) {
    int64_t sz = br_desc_size(d);
    memcpy(out, v, (size_t)sz);
    if (*d == 's')
        brine_str_retain(*(brine_str **)out);
    else if (*d == 'a')
        brine_arr_retain(*(brine_arr **)out);
}

static void br_put(int64_t dest, const char *d, const void *v, const char *loc) {
    br_buf b = {0};
    br_encode(&b, d, v);
    br_send(dest, BR_CH_COLL, BR_K_MSG, b.p, b.len, loc);
    free(b.p);
}
static void br_take(int64_t src, const char *d, void *out, const char *loc) {
    br_msg *m = br_recv(src, BR_CH_COLL, BR_K_MSG, loc);
    const char *p = m->data;
    br_decode(&p, d, out, loc);
    free(m);
}

void brine_world_shift(const char *d, const void *v, void *out, int64_t by, const char *loc) {
    int64_t k = by % br_size;
    if (k < 0) k += br_size;
    if (k == 0) {
        br_copy_val(d, v, out);
        return;
    }
    br_put((br_rank + k) % br_size, d, v, loc);
    br_take((br_rank - k + br_size) % br_size, d, out, loc);
}

brine_arr *brine_world_exchange(const char *ed, brine_arr *xs, const char *loc) {
    if (xs->len != br_size)
        br_panicf(loc, "world.exchange needs one element per rank: got %lld elements, but the world size is %lld", (long long)xs->len,
                  (long long)br_size);
    brine_elem_fn rt, rl;
    br_desc_fns(ed, &rt, &rl);
    brine_arr *res = brine_arr_new(br_size, br_desc_size(ed), rt, rl, loc);
    for (int64_t j = 0; j < br_size; j++)
        if (j != br_rank) br_put(j, ed, xs->data + j * xs->esz, loc);
    br_copy_val(ed, xs->data + br_rank * xs->esz, res->data + br_rank * res->esz);
    for (int64_t j = 0; j < br_size; j++)
        if (j != br_rank) br_take(j, ed, res->data + j * res->esz, loc);
    return res;
}

void brine_world_bcast(const char *d, const void *v, void *out, int64_t root, const char *loc) {
    br_check_rank(root, "root", loc);
    if (br_rank == root) {
        for (int64_t r = 0; r < br_size; r++)
            if (r != root) br_put(r, d, v, loc);
        br_copy_val(d, v, out);
    } else {
        br_take(root, d, out, loc);
    }
}

/* every rank gets [v from rank 0, v from rank 1, ...] */
brine_arr *brine_world_gather(const char *ed, const void *v, const char *loc) {
    brine_elem_fn rt, rl;
    br_desc_fns(ed, &rt, &rl);
    brine_arr *res = brine_arr_new(br_size, br_desc_size(ed), rt, rl, loc);
    for (int64_t j = 0; j < br_size; j++)
        if (j != br_rank) br_put(j, ed, v, loc);
    br_copy_val(ed, v, res->data + br_rank * res->esz);
    for (int64_t j = 0; j < br_size; j++)
        if (j != br_rank) br_take(j, ed, res->data + j * res->esz, loc);
    return res;
}

/* reductions: gather to rank 0 in rank order (deterministic), combine, broadcast */
int64_t brine_world_reduce_i64(int64_t v, int op, const char *loc) {
    if (br_size <= 1) return v;
    if (br_rank == 0) {
        int64_t acc = v;
        for (int64_t r = 1; r < br_size; r++) {
            int64_t x = br_get8(br_recv(r, BR_CH_COLL, BR_K_I64, loc));
            if (op == 0)
                acc = brine_add_i64(acc, x, loc);
            else if (op == 1)
                acc = x < acc ? x : acc;
            else
                acc = x > acc ? x : acc;
        }
        for (int64_t r = 1; r < br_size; r++) br_send(r, BR_CH_COLL, BR_K_I64, &acc, 8, loc);
        return acc;
    }
    br_send(0, BR_CH_COLL, BR_K_I64, &v, 8, loc);
    return br_get8(br_recv(0, BR_CH_COLL, BR_K_I64, loc));
}
double brine_world_reduce_f64(double v, int op, const char *loc) {
    if (br_size <= 1) return v;
    double acc = v;
    if (br_rank == 0) {
        for (int64_t r = 1; r < br_size; r++) {
            br_msg *m = br_recv(r, BR_CH_COLL, BR_K_F64, loc);
            double x;
            memcpy(&x, m->data, 8);
            free(m);
            if (op == 0)
                acc += x;
            else if (op == 1)
                acc = x < acc ? x : acc;
            else
                acc = x > acc ? x : acc;
        }
        for (int64_t r = 1; r < br_size; r++) br_send(r, BR_CH_COLL, BR_K_F64, &acc, 8, loc);
        return acc;
    }
    br_send(0, BR_CH_COLL, BR_K_F64, &v, 8, loc);
    br_msg *m = br_recv(0, BR_CH_COLL, BR_K_F64, loc);
    memcpy(&acc, m->data, 8);
    free(m);
    return acc;
}
/* ============================================================ init / shutdown */
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <shellapi.h>
/* Windows hands `main` arguments in the local code page; brine strings are UTF-8 */
static void br_utf8_args(void) {
    int n = 0;
    wchar_t **w = CommandLineToArgvW(GetCommandLineW(), &n);
    if (!w) return;
    char **out = (char **)brine_alloc((int64_t)sizeof(char *) * (n + 1));
    for (int i = 0; i < n; i++) {
        int m = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, NULL, 0, NULL, NULL);
        out[i] = (char *)brine_alloc(m > 0 ? m : 1);
        if (m > 0) WideCharToMultiByte(CP_UTF8, 0, w[i], -1, out[i], m, NULL, NULL);
        else out[i][0] = 0;
    }
    out[n] = NULL;
    LocalFree(w);
    br_argc = n;
    br_argv = out;
}
#endif

void brine_rt_init(int argc, char **argv) {
    br_argc = argc;
    br_argv = argv;
#ifdef _WIN32
    br_utf8_args();
    /* output is UTF-8 with LF line endings on every platform */
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    SetConsoleOutputCP(CP_UTF8);
#endif
    br_mutex_init(&br_io_mu);
    br_mutex_init(&br_pool.mu);
    br_mutex_init(&br_pool.job_lock);
    br_cond_init(&br_pool.cv_work);
    br_cond_init(&br_pool.cv_done);
    static char outbuf[1 << 16];
    setvbuf(stdout, outbuf, _IOFBF, sizeof outbuf);
    br_world_init();
}

static void br_procs_free(void);

void brine_rt_shutdown(void) {
    br_procs_free();
    br_flush();
    if (br_size > 1) brine_world_barrier();
    br_flush();
    if (getenv("BRINE_LEAKCHECK")) fprintf(stderr, "brine: %lld live objects at exit\n", (long long)__atomic_load_n(&br_live, __ATOMIC_RELAXED));
}

/* ============================================================ files and processes */
#ifndef _WIN32
#include <spawn.h>
#include <sys/wait.h>
#include <sys/stat.h>
extern char **environ;
#endif

static void *br_grow(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        fprintf(stderr, "out of memory\n");
        abort();
    }
    return q;
}

/* a NUL-terminated copy of s (NULL if s holds a NUL byte, which no path or argument can) */
static char *br_cstr(brine_str *s) {
    if (s->len && memchr(s->data, 0, (size_t)s->len)) return NULL;
    char *p = (char *)brine_alloc(s->len + 1);
    memcpy(p, s->data, (size_t)s->len);
    p[s->len] = 0;
    return p;
}

static brine_str *br_strf(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    if (n >= (int)sizeof buf) n = (int)sizeof buf - 1;
    return brine_str_new(buf, n);
}

static int br_valid_utf8(const unsigned char *p, int64_t n) {
    int64_t i = 0;
    while (i < n) {
        if (p[i] < 0x80) {
            i++;
            continue;
        }
        int64_t k = br_utf8_len(p + i, n - i);
        if (k == 1) return 0;
        i += k;
    }
    return 1;
}

#ifdef _WIN32
static wchar_t *br_wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = (wchar_t *)brine_alloc((int64_t)sizeof(wchar_t) * (n > 0 ? n : 1));
    if (n <= 0) w[0] = 0;
    else MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}
#endif

static FILE *br_open(const char *path, const char *mode) {
#ifdef _WIN32
    wchar_t *wp = br_wide(path), *wm = br_wide(mode);
    FILE *f = _wfopen(wp, wm);
    free(wp);
    free(wm);
    return f;
#else
    return fopen(path, mode);
#endif
}

bool brine_read_file(brine_str *path, brine_str **out) {
    char *p = br_cstr(path);
    if (!p) {
        *out = br_strf("cannot read a path containing a NUL byte");
        return false;
    }
    FILE *f = br_open(p, "rb");
    if (!f) {
        *out = br_strf("cannot read %s: %s", p, strerror(errno));
        free(p);
        return false;
    }
    char *buf = NULL;
    size_t len = 0, cap = 0;
    for (;;) {
        if (len == cap) {
            cap = cap ? cap * 2 : 65536;
            buf = (char *)br_grow(buf, cap);
        }
        size_t got = fread(buf + len, 1, cap - len, f);
        len += got;
        if (got == 0) break;
    }
    int bad = ferror(f);
    fclose(f);
    if (bad) {
        *out = br_strf("cannot read %s: read error", p);
    } else if (!br_valid_utf8((const unsigned char *)buf, (int64_t)len)) {
        *out = br_strf("cannot read %s: the file is not valid UTF-8 text", p);
        bad = 1;
    } else {
        *out = brine_str_new(buf, (int64_t)len);
    }
    free(buf);
    free(p);
    return !bad;
}

bool brine_write_file(brine_str *path, brine_str *text, brine_str **err) {
    char *p = br_cstr(path);
    if (!p) {
        *err = br_strf("cannot write a path containing a NUL byte");
        return false;
    }
    FILE *f = br_open(p, "wb");
    if (!f) {
        *err = br_strf("cannot write %s: %s", p, strerror(errno));
        free(p);
        return false;
    }
    size_t put = text->len ? fwrite(text->data, 1, (size_t)text->len, f) : 0;
    int bad = put != (size_t)text->len;
    if (fclose(f) != 0) bad = 1;
    if (bad) *err = br_strf("cannot write %s: write error (is the disk full?)", p);
    free(p);
    return !bad;
}

bool brine_file_exists(brine_str *path) {
    char *p = br_cstr(path);
    if (!p) return false;
#ifdef _WIN32
    wchar_t *w = br_wide(p);
    DWORD a = GetFileAttributesW(w);
    free(w);
    free(p);
    return a != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    int r = stat(p, &st);
    free(p);
    return r == 0;
#endif
}

/* the variable's value, or NULL when it isn't set */
brine_str *brine_getenv(brine_str *name) {
    char *n = br_cstr(name);
    if (!n || !n[0] || strchr(n, '=')) {
        free(n);
        return NULL;
    }
#ifdef _WIN32
    wchar_t *w = br_wide(n);
    free(n);
    SetLastError(0);
    DWORD need = GetEnvironmentVariableW(w, NULL, 0);
    if (need == 0) {
        free(w);
        return NULL;
    }
    wchar_t *buf = (wchar_t *)brine_alloc((int64_t)sizeof(wchar_t) * need);
    DWORD got = GetEnvironmentVariableW(w, buf, need);
    free(w);
    int m = got ? WideCharToMultiByte(CP_UTF8, 0, buf, (int)got, NULL, 0, NULL, NULL) : 0;
    char *u = (char *)brine_alloc(m > 0 ? m : 1);
    if (m > 0) WideCharToMultiByte(CP_UTF8, 0, buf, (int)got, u, m, NULL, NULL);
    brine_str *s = brine_str_new(u, m > 0 ? m : 0);
    free(u);
    free(buf);
    return s;
#else
    const char *v = getenv(n);
    free(n);
    return v ? brine_str_new(v, (int64_t)strlen(v)) : NULL;
#endif
}

#ifdef _WIN32
/* append one argument to a command line, quoted so the child's argv parser gets it back unchanged */
static void br_quote_arg(brine_sb *sb, const char *a) {
    if (a[0] && !strpbrk(a, " \t\n\v\"")) {
        brine_sb_cat(sb, a, (int64_t)strlen(a));
        return;
    }
    brine_sb_cat(sb, "\"", 1);
    for (const char *p = a;; p++) {
        int64_t slashes = 0;
        while (*p == '\\') {
            slashes++;
            p++;
        }
        if (!*p) {
            for (int64_t i = 0; i < slashes * 2; i++) brine_sb_cat(sb, "\\", 1);
            break;
        }
        int64_t k = *p == '"' ? slashes * 2 + 1 : slashes;
        for (int64_t i = 0; i < k; i++) brine_sb_cat(sb, "\\", 1);
        brine_sb_cat(sb, p, 1);
    }
    brine_sb_cat(sb, "\"", 1);
}
#endif

/* Run a program and wait for it. false (with *err set) if it couldn't be started. */
bool brine_run(brine_arr *argv, int64_t *code, brine_str **err) {
    *code = 0;
    if (argv->len == 0) {
        *err = br_strf("run() needs a program name, but the command list is empty");
        return false;
    }
    brine_str **items = (brine_str **)argv->data;
    int64_t n = argv->len;
    char **cv = (char **)brine_alloc((int64_t)sizeof(char *) * (n + 1));
    bool ok = true;
    for (int64_t i = 0; i < n; i++) {
        cv[i] = br_cstr(items[i]);
        if (!cv[i] && ok) {
            *err = br_strf("run(): argument %lld contains a NUL byte", (long long)i);
            ok = false;
        }
    }
    cv[n] = NULL;
    br_flush();
    fflush(stderr);
    if (ok) {
#ifdef _WIN32
        brine_sb sb;
        brine_sb_init(&sb);
        for (int64_t i = 0; i < n; i++) {
            if (i) brine_sb_cat(&sb, " ", 1);
            br_quote_arg(&sb, cv[i]);
        }
        brine_str *line = brine_sb_finish(&sb);
        char *lc = br_cstr(line);
        brine_str_release(line);
        wchar_t *wl = br_wide(lc);
        free(lc);
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        memset(&si, 0, sizeof si);
        si.cb = sizeof si;
        if (!CreateProcessW(NULL, wl, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
            DWORD e = GetLastError();
            *err = e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? br_strf("cannot run %s: program not found", cv[0])
                                                                          : br_strf("cannot run %s: error %lu", cv[0], (unsigned long)e);
            ok = false;
        } else {
            WaitForSingleObject(pi.hProcess, INFINITE);
            DWORD c = 0;
            GetExitCodeProcess(pi.hProcess, &c);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            *code = (int64_t)(int32_t)c;
        }
        free(wl);
#else
        pid_t pid;
        int r = posix_spawnp(&pid, cv[0], NULL, NULL, cv, environ);
        if (r != 0) {
            *err = r == ENOENT ? br_strf("cannot run %s: program not found", cv[0]) : br_strf("cannot run %s: %s", cv[0], strerror(r));
            ok = false;
        } else {
            int st = 0;
            while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
            }
            if (WIFEXITED(st)) *code = WEXITSTATUS(st);
            else if (WIFSIGNALED(st)) *code = 128 + WTERMSIG(st);
            else *code = -1;
        }
#endif
    }
    for (int64_t i = 0; i < n; i++) free(cv[i]);
    free(cv);
    return ok;
}

/* ============================================================ the os module */
#ifndef _WIN32
#include <dirent.h>
#include <fcntl.h>
/* XSI, so not declared under plain _POSIX_C_SOURCE, but in every POSIX libc */
char *realpath(const char *path, char *resolved);
#endif

#ifdef _WIN32
/* UTF-16 (n units, or NUL-terminated when n < 0) to a brine string; bad surrogates become U+FFFD */
static brine_str *br_from_wide(const wchar_t *w, int n) {
    if (n < 0) n = (int)wcslen(w);
    int m = n ? WideCharToMultiByte(CP_UTF8, 0, w, n, NULL, 0, NULL, NULL) : 0;
    char *u = (char *)brine_alloc(m > 0 ? m : 1);
    if (m > 0) WideCharToMultiByte(CP_UTF8, 0, w, n, u, m, NULL, NULL);
    brine_str *s = brine_str_new(u, m > 0 ? m : 0);
    free(u);
    return s;
}
#endif

/* the text Rust's io::Error prints for an OS error code */
static brine_str *br_os_err(int code) {
#ifdef _WIN32
    wchar_t buf[2048];
    DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, (DWORD)code, 0, buf, 2048, NULL);
    if (n == 0) return br_strf("OS Error %d (FormatMessageW() returned error %lu) (os error %d)", code, (unsigned long)GetLastError(), code);
    while (n > 0 && (buf[n - 1] == L'\r' || buf[n - 1] == L'\n' || buf[n - 1] == L' ' || buf[n - 1] == L'\t')) n--;
    brine_str *m = br_from_wide(buf, (int)n);
    brine_sb sb;
    brine_sb_init(&sb);
    brine_sb_str(&sb, m);
    brine_str_release(m);
    char tail[48];
    int k = snprintf(tail, sizeof tail, " (os error %d)", code);
    brine_sb_cat(&sb, tail, k);
    return brine_sb_finish(&sb);
#else
    return br_strf("%s (os error %d)", strerror(code), code);
#endif
}

static brine_str *br_nul_err(void) { return br_strf("file name contained an unexpected NUL byte"); }

/* bytes to a string, replacing invalid UTF-8 like Rust's String::from_utf8_lossy */
static brine_str *br_lossy(const unsigned char *p, size_t n) {
    brine_sb sb;
    brine_sb_init(&sb);
    size_t i = 0, run = 0; /* run: start of valid bytes not yet copied */
    while (i < n) {
        unsigned char b = p[i];
        if (b < 0x80) {
            i++;
            continue;
        }
        size_t w = (b >= 0xC2 && b <= 0xDF) ? 2 : (b >= 0xE0 && b <= 0xEF) ? 3 : (b >= 0xF0 && b <= 0xF4) ? 4 : 0;
        size_t j = i + 1;
        if (w) {
            unsigned char lo = 0x80, hi = 0xBF;
            if (b == 0xE0) lo = 0xA0;
            else if (b == 0xED) hi = 0x9F;
            else if (b == 0xF0) lo = 0x90;
            else if (b == 0xF4) hi = 0x8F;
            if (j < n && p[j] >= lo && p[j] <= hi) {
                j++;
                while (j - i < w && j < n && p[j] >= 0x80 && p[j] <= 0xBF) j++;
                if (j - i == w) {
                    i = j;
                    continue;
                }
            }
        }
        brine_sb_cat(&sb, (const char *)p + run, (int64_t)(i - run));
        brine_sb_cat(&sb, "\xEF\xBF\xBD", 3);
        i = j;
        run = i;
    }
    brine_sb_cat(&sb, (const char *)p + run, (int64_t)(n - run));
    return brine_sb_finish(&sb);
}

static int br_cmp_strp(const void *a, const void *b) { return brine_str_cmp(*(brine_str *const *)a, *(brine_str *const *)b); }

bool brine_os_list_dir(brine_str *path, brine_arr **out, brine_str **err) {
    char *p = br_cstr(path);
    if (!p) {
        *err = br_nul_err();
        return false;
    }
    brine_arr *a = brine_arr_new(0, sizeof(brine_str *), brine_rt_str, brine_rl_str, "<os.list_dir>");
#ifdef _WIN32
    size_t n = strlen(p);
    char *pat = (char *)brine_alloc((int64_t)n + 3);
    memcpy(pat, p, n);
    if (n > 0 && p[n - 1] != '\\' && p[n - 1] != '/') pat[n++] = '\\';
    pat[n++] = '*';
    pat[n] = 0;
    wchar_t *w = br_wide(pat);
    free(pat);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(w, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
    free(w);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        /* only an empty root directory has no entries at all */
        if (e != ERROR_FILE_NOT_FOUND) {
            free(p);
            brine_arr_release(a);
            *err = br_os_err((int)e);
            return false;
        }
    } else {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            brine_str *s = br_from_wide(fd.cFileName, -1);
            brine_arr_push(&a, &s);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
#else
    DIR *d = opendir(p);
    if (!d) {
        int e = errno;
        free(p);
        brine_arr_release(a);
        *err = br_os_err(e);
        return false;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        brine_str *s = brine_str_new(ent->d_name, (int64_t)strlen(ent->d_name));
        brine_arr_push(&a, &s);
    }
    closedir(d);
#endif
    free(p);
    brine_arr_sort(&a, br_cmp_strp);
    *out = a;
    return true;
}

/* 0: missing, 1: regular file, 2: directory, 3: something else. Follows links. */
static int br_kind(const char *p) {
#ifdef _WIN32
    wchar_t *w = br_wide(p);
    DWORD a = GetFileAttributesW(w);
    int k = a == INVALID_FILE_ATTRIBUTES ? 0 : (a & FILE_ATTRIBUTE_DIRECTORY) ? 2 : 1;
    if (k && (a & FILE_ATTRIBUTE_REPARSE_POINT)) {
        /* a link: look at its target */
        HANDLE h = CreateFileW(w, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
        BY_HANDLE_FILE_INFORMATION info;
        if (h == INVALID_HANDLE_VALUE) k = 0;
        else {
            k = GetFileInformationByHandle(h, &info) ? ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 2 : 1) : 0;
            CloseHandle(h);
        }
    }
    free(w);
    return k;
#else
    struct stat st;
    if (stat(p, &st) != 0) return 0;
    return S_ISDIR(st.st_mode) ? 2 : S_ISREG(st.st_mode) ? 1 : 3;
#endif
}

bool brine_os_is_dir(brine_str *path) {
    char *p = br_cstr(path);
    if (!p) return false;
    int k = br_kind(p);
    free(p);
    return k == 2;
}

bool brine_os_is_file(brine_str *path) {
    char *p = br_cstr(path);
    if (!p) return false;
    int k = br_kind(p);
    free(p);
    return k == 1;
}

static int br_is_sep(char c) {
#ifdef _WIN32
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

/* length of the root prefix: "/" or, on Windows, "C:", "C:\" or "\" */
static size_t br_root_len(const char *p) {
#ifdef _WIN32
    if (((p[0] >= 'a' && p[0] <= 'z') || (p[0] >= 'A' && p[0] <= 'Z')) && p[1] == ':') return br_is_sep(p[2]) ? 3 : 2;
#endif
    return br_is_sep(p[0]) ? 1 : 0;
}

/* Rust's Path::parent: NULL when there is none (a root); "" for a bare name */
static char *br_parent(const char *p) {
    size_t root = br_root_len(p), n = strlen(p);
    while (n > root && br_is_sep(p[n - 1])) n--;
    if (n <= root) return NULL;
    size_t i = n;
    while (i > root && !br_is_sep(p[i - 1])) i--;
    while (i > root && br_is_sep(p[i - 1])) i--;
    char *q = (char *)brine_alloc((int64_t)i + 1);
    memcpy(q, p, i);
    q[i] = 0;
    return q;
}

/* 0 on success, else the OS error code */
static int br_mkdir1(const char *p) {
#ifdef _WIN32
    wchar_t *w = br_wide(p);
    int e = CreateDirectoryW(w, NULL) ? 0 : (int)GetLastError();
    free(w);
    return e;
#else
    return mkdir(p, 0777) == 0 ? 0 : errno;
#endif
}

static int br_not_found(int e) {
#ifdef _WIN32
    return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND;
#else
    return e == ENOENT;
#endif
}

/* Rust's fs::create_dir_all; -1 when the tree can't be made at all */
static int br_mkdir_all(const char *p) {
    if (!p[0]) return 0;
    int e = br_mkdir1(p);
    if (!e) return 0;
    if (!br_not_found(e)) return br_kind(p) == 2 ? 0 : e;
    char *parent = br_parent(p);
    if (!parent) return -1;
    int pe = br_mkdir_all(parent);
    free(parent);
    if (pe) return pe;
    e = br_mkdir1(p);
    return !e || br_kind(p) == 2 ? 0 : e;
}

bool brine_os_make_dir(brine_str *path, brine_str **err) {
    char *p = br_cstr(path);
    if (!p) {
        *err = br_nul_err();
        return false;
    }
    int e = br_mkdir_all(p);
    free(p);
    if (e == -1) *err = br_strf("failed to create whole tree");
    else if (e) *err = br_os_err(e);
    return e == 0;
}

bool brine_os_remove_file(brine_str *path, brine_str **err) {
    char *p = br_cstr(path);
    if (!p) {
        *err = br_nul_err();
        return false;
    }
#ifdef _WIN32
    wchar_t *w = br_wide(p);
    int e = DeleteFileW(w) ? 0 : (int)GetLastError();
    free(w);
#else
    int e = unlink(p) == 0 ? 0 : errno;
#endif
    free(p);
    if (e) *err = br_os_err(e);
    return e == 0;
}

bool brine_os_rename(brine_str *src, brine_str *dst, brine_str **err) {
    char *a = br_cstr(src), *b = br_cstr(dst);
    if (!a || !b) {
        free(a);
        free(b);
        *err = br_nul_err();
        return false;
    }
#ifdef _WIN32
    wchar_t *wa = br_wide(a), *wb = br_wide(b);
    int e = MoveFileExW(wa, wb, MOVEFILE_REPLACE_EXISTING) ? 0 : (int)GetLastError();
    free(wa);
    free(wb);
#else
    int e = rename(a, b) == 0 ? 0 : errno;
#endif
    free(a);
    free(b);
    if (e) *err = br_os_err(e);
    return e == 0;
}

brine_str *brine_os_cwd(void) {
#ifdef _WIN32
    DWORD n = GetCurrentDirectoryW(0, NULL);
    wchar_t *w = (wchar_t *)brine_alloc((int64_t)sizeof(wchar_t) * (n + 1));
    DWORD got = GetCurrentDirectoryW(n + 1, w);
    brine_str *s = br_from_wide(w, (int)got);
    free(w);
    return s;
#else
    size_t cap = 256;
    for (;;) {
        char *buf = (char *)brine_alloc((int64_t)cap);
        if (getcwd(buf, cap)) {
            brine_str *s = brine_str_new(buf, (int64_t)strlen(buf));
            free(buf);
            return s;
        }
        free(buf);
        if (errno != ERANGE) return brine_str_new(".", 1);
        cap *= 2;
    }
#endif
}

bool brine_os_set_cwd(brine_str *path, brine_str **err) {
    char *p = br_cstr(path);
    if (!p) {
        *err = br_nul_err();
        return false;
    }
#ifdef _WIN32
    wchar_t *w = br_wide(p);
    int e = SetCurrentDirectoryW(w) ? 0 : (int)GetLastError();
    free(w);
#else
    int e = chdir(p) == 0 ? 0 : errno;
#endif
    free(p);
    if (e) *err = br_os_err(e);
    return e == 0;
}

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
/* the absolute path of the running executable */
brine_str *brine_os_exe_path(void) {
#ifdef _WIN32
    for (DWORD cap = 260; cap < (1u << 20); cap *= 2) {
        wchar_t *w = (wchar_t *)brine_alloc((int64_t)sizeof(wchar_t) * cap);
        DWORD n = GetModuleFileNameW(NULL, w, cap);
        if (n > 0 && n < cap) {
            brine_str *s = br_from_wide(w, (int)n);
            free(w);
            return s;
        }
        free(w);
        if (n == 0) break;
    }
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(NULL, &size);
    char *buf = (char *)brine_alloc((int64_t)size + 1);
    if (_NSGetExecutablePath(buf, &size) == 0) {
        char *r = realpath(buf, NULL);
        brine_str *s = brine_str_new(r ? r : buf, (int64_t)strlen(r ? r : buf));
        free(r);
        free(buf);
        return s;
    }
    free(buf);
#elif defined(__linux__) || defined(__ANDROID__)
    for (size_t cap = 256; cap < (1u << 20); cap *= 2) {
        char *buf = (char *)brine_alloc((int64_t)cap);
        ssize_t n = readlink("/proc/self/exe", buf, cap);
        if (n > 0 && (size_t)n < cap) {
            brine_str *s = brine_str_new(buf, (int64_t)n);
            free(buf);
            return s;
        }
        free(buf);
        if (n <= 0) break;
    }
#endif
    const char *a0 = br_argv && br_argv[0] ? br_argv[0] : "";
    return brine_str_new(a0, (int64_t)strlen(a0));
}

brine_str *brine_os_temp_dir(void) {
    brine_str *s;
#ifdef _WIN32
    typedef DWORD(WINAPI * temp_fn)(DWORD, LPWSTR);
    temp_fn f = (temp_fn)(void (*)(void))GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetTempPath2W");
    if (!f) f = GetTempPathW;
    wchar_t w[MAX_PATH + 2];
    DWORD n = f(MAX_PATH + 1, w);
    if (n > MAX_PATH + 1) n = 0;
    s = br_from_wide(w, (int)n);
#else
    const char *t = getenv("TMPDIR");
    if (!t) t = "/tmp";
    s = brine_str_new(t, (int64_t)strlen(t));
#endif
    /* no trailing separator, unless the whole path is a root */
    char *c = br_cstr(s);
    int64_t root = c ? (int64_t)br_root_len(c) : 0;
    free(c);
    int64_t n2 = s->len;
    while (n2 > root && br_is_sep(s->data[n2 - 1])) n2--;
    if (n2 != s->len) {
        brine_str *t2 = brine_str_new(s->data, n2);
        brine_str_release(s);
        s = t2;
    }
    return s;
}

bool brine_os_abs_path(brine_str *path, brine_str **out, brine_str **err) {
    char *p = br_cstr(path);
    if (!p) {
        *err = br_nul_err();
        return false;
    }
#ifdef _WIN32
    /* what Rust's fs::canonicalize does: the final path of an open handle, `\\?\C:\...` */
    wchar_t *w = br_wide(p);
    free(p);
    HANDLE h = CreateFileW(w, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) {
        *err = br_os_err((int)GetLastError());
        return false;
    }
    DWORD need = GetFinalPathNameByHandleW(h, NULL, 0, VOLUME_NAME_DOS);
    wchar_t *buf = (wchar_t *)brine_alloc((int64_t)sizeof(wchar_t) * (need + 1));
    DWORD got = need ? GetFinalPathNameByHandleW(h, buf, need + 1, VOLUME_NAME_DOS) : 0;
    int e = got == 0 || got > need ? (int)GetLastError() : 0;
    CloseHandle(h);
    if (e || got == 0) {
        free(buf);
        *err = br_os_err(e ? e : ERROR_INVALID_DATA);
        return false;
    }
    *out = br_from_wide(buf, (int)got);
    free(buf);
    return true;
#else
    char *r = realpath(p, NULL);
    int e = errno;
    free(p);
    if (!r) {
        *err = br_os_err(e);
        return false;
    }
    *out = brine_str_new(r, (int64_t)strlen(r));
    free(r);
    return true;
#endif
}

/* the names Rust uses in std::env::consts */
brine_str *brine_os_name(void) {
#if defined(_WIN32)
    const char *n = "windows";
#elif defined(__ANDROID__)
    const char *n = "android";
#elif defined(__APPLE__)
    const char *n = "macos";
#elif defined(__linux__)
    const char *n = "linux";
#elif defined(__FreeBSD__)
    const char *n = "freebsd";
#elif defined(__OpenBSD__)
    const char *n = "openbsd";
#elif defined(__NetBSD__)
    const char *n = "netbsd";
#elif defined(__DragonFly__)
    const char *n = "dragonfly";
#elif defined(__sun)
    const char *n = "solaris";
#else
    const char *n = "unknown";
#endif
    return brine_str_new(n, (int64_t)strlen(n));
}

brine_str *brine_os_arch(void) {
#if defined(__x86_64__) || defined(_M_X64)
    const char *n = "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    const char *n = "aarch64";
#elif defined(__i386__) || defined(_M_IX86)
    const char *n = "x86";
#elif defined(__arm__) || defined(_M_ARM)
    const char *n = "arm";
#elif defined(__riscv) && __riscv_xlen == 64
    const char *n = "riscv64";
#elif defined(__powerpc64__)
    const char *n = "powerpc64";
#elif defined(__s390x__)
    const char *n = "s390x";
#elif defined(__loongarch64)
    const char *n = "loongarch64";
#else
    const char *n = "unknown";
#endif
    return brine_str_new(n, (int64_t)strlen(n));
}

int64_t brine_os_pid(void) {
#ifdef _WIN32
    return (int64_t)GetCurrentProcessId();
#else
    return (int64_t)getpid();
#endif
}

int64_t brine_os_cpu_count(void) {
    int n = br_ncpu();
    return n > 0 ? n : 1;
}

void brine_os_sleep(double seconds) {
    if (!(seconds > 0)) return;
#ifdef _WIN32
    double ms = ceil(seconds * 1000.0);
    while (ms > 0) {
        DWORD part = ms > 4000000000.0 ? 4000000000u : (DWORD)ms;
        Sleep(part);
        ms -= part;
    }
#else
    struct timespec ts;
    ts.tv_sec = seconds > 1e15 ? (time_t)1e15 : (time_t)seconds;
    ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1e9);
    if (ts.tv_nsec < 0) ts.tv_nsec = 0;
    if (ts.tv_nsec >= 1000000000L) ts.tv_nsec = 999999999L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
#endif
}

#ifdef _WIN32
/* an MSYS/Cygwin terminal is a named pipe called like `msys-...-pty0-to-master` (same test as Rust) */
static int br_msys_tty(HANDLE h) {
    if (GetFileType(h) != FILE_TYPE_PIPE) return 0;
    struct {
        DWORD len;
        WCHAR name[MAX_PATH];
    } info;
    memset(&info, 0, sizeof info);
    if (!GetFileInformationByHandleEx(h, FileNameInfo, &info, sizeof info)) return 0;
    int n = (int)(info.len / sizeof(WCHAR));
    if (n > MAX_PATH) n = MAX_PATH;
    brine_str *s = br_from_wide(info.name, n);
    int64_t start = 0;
    for (int64_t i = 0; i < s->len; i++)
        if (s->data[i] == '\\') start = i + 1;
    int64_t bl = s->len - start;
    char *b = (char *)brine_alloc(bl + 1);
    memcpy(b, s->data + start, (size_t)bl);
    b[bl] = 0;
    int r = (!strncmp(b, "msys-", 5) || !strncmp(b, "cygwin-", 7)) && strstr(b, "-pty") != NULL;
    free(b);
    brine_str_release(s);
    return r;
}
#endif

bool brine_os_is_terminal(int64_t fd) {
    if (fd < 0 || fd > 2) return false;
#ifdef _WIN32
    DWORD which = fd == 0 ? STD_INPUT_HANDLE : fd == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE;
    HANDLE h = GetStdHandle(which);
    if (h == NULL || h == INVALID_HANDLE_VALUE) return false;
    DWORD mode;
    if (GetConsoleMode(h, &mode)) return true;
    return br_msys_tty(h) != 0;
#else
    return isatty((int)fd) == 1;
#endif
}

void brine_os_enable_ansi(void) {
#ifdef _WIN32
    DWORD which[2] = {STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    for (int i = 0; i < 2; i++) {
        HANDLE h = GetStdHandle(which[i]);
        DWORD m = 0;
        if (GetConsoleMode(h, &m)) SetConsoleMode(h, m | 0x0004 /* ENABLE_VIRTUAL_TERMINAL_PROCESSING */);
    }
#endif
}

bool brine_os_free_ports(int64_t n, brine_arr **out, brine_str **err) {
#ifdef _WIN32
    static int wsa_ready;
    if (!wsa_ready) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        wsa_ready = 1;
    }
#define BR_SOCK_ERR ((int)WSAGetLastError())
#else
#define BR_SOCK_ERR errno
#endif
    if (n < 0) n = 0;
    brine_arr *a = brine_arr_new(0, sizeof(int64_t), NULL, NULL, "<os.free_ports>");
    br_sock *socks = (br_sock *)brine_alloc((int64_t)sizeof(br_sock) * (n ? n : 1));
    int64_t open = 0;
    int e = 0;
    /* hold every listener until all ports are known, so they are distinct */
    for (int64_t i = 0; i < n; i++) {
        br_sock s = socket(AF_INET, SOCK_STREAM, 0);
        if (s == BR_BAD_SOCK) {
            e = BR_SOCK_ERR;
            break;
        }
        socks[open++] = s;
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sa.sin_port = 0;
#ifdef _WIN32
        int len = (int)sizeof sa;
#else
        socklen_t len = (socklen_t)sizeof sa;
#endif
        if (bind(s, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(s, 128) != 0 || getsockname(s, (struct sockaddr *)&sa, &len) != 0) {
            e = BR_SOCK_ERR;
            break;
        }
        int64_t port = (int64_t)ntohs(sa.sin_port);
        brine_arr_push(&a, &port);
    }
#undef BR_SOCK_ERR
    for (int64_t i = 0; i < open; i++) br_sock_close(socks[i]);
    free(socks);
    if (e) {
        brine_arr_release(a);
        *err = br_os_err(e);
        return false;
    }
    *out = a;
    return true;
}

/* ------------------------------------------------------------ processes */
typedef struct {
#ifdef _WIN32
    HANDLE proc;
    HANDLE out_f, err_f; /* capture files (deleted when closed) */
#else
    pid_t pid;
    int out_fd, err_fd;
#endif
    bool capture, done;
    int64_t code;
    brine_str *out, *err; /* what it wrote, once it has exited */
} br_proc;

/* every process started by os.spawn; a handle is an index + 1 */
static br_proc *br_procs;
static int64_t br_nprocs, br_cap_procs;
static br_mutex br_proc_mu;
static int br_proc_mu_ready; /* set by the first os.spawn (spawning can't happen in `par`) */

static br_proc *br_proc_get(int64_t h, const char *what, const char *loc) {
    if (h < 1 || h > br_nprocs) {
        if (br_proc_mu_ready) br_unlock(&br_proc_mu);
        char msg[160];
        snprintf(msg, sizeof msg, "%s: %lld is not a process handle from os.spawn", what, (long long)h);
        brine_panic_at(loc, msg);
    }
    return &br_procs[h - 1];
}

#ifdef _WIN32
/* a new empty temporary file, deleted when its last handle closes */
static HANDLE br_capture_file(void) {
    static LONG counter;
    brine_str *dir = brine_os_temp_dir();
    HANDLE h = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 100; attempt++) {
        char name[128];
        snprintf(name, sizeof name, "\\brine-out-%lu-%ld-%lu.tmp", (unsigned long)GetCurrentProcessId(), (long)InterlockedIncrement(&counter),
                 (unsigned long)GetTickCount());
        brine_sb sb;
        brine_sb_init(&sb);
        brine_sb_str(&sb, dir);
        brine_sb_cat(&sb, name, (int64_t)strlen(name));
        brine_str *p = brine_sb_finish(&sb);
        char *c = br_cstr(p);
        brine_str_release(p);
        wchar_t *w = br_wide(c);
        free(c);
        h = CreateFileW(w, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, CREATE_NEW,
                        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
        DWORD e = GetLastError();
        free(w);
        if (h != INVALID_HANDLE_VALUE || (e != ERROR_FILE_EXISTS && e != ERROR_ALREADY_EXISTS)) break;
    }
    brine_str_release(dir);
    return h;
}

static brine_str *br_read_capture(HANDLE h) {
    if (h == NULL || h == INVALID_HANDLE_VALUE) return brine_str_new("", 0);
    SetFilePointer(h, 0, NULL, FILE_BEGIN);
    char *buf = NULL;
    size_t len = 0, cap = 0;
    for (;;) {
        if (len == cap) {
            cap = cap ? cap * 2 : 65536;
            buf = (char *)br_grow(buf, cap);
        }
        DWORD got = 0;
        if (!ReadFile(h, buf + len, (DWORD)(cap - len), &got, NULL) || got == 0) break;
        len += got;
    }
    CloseHandle(h);
    brine_str *s = br_lossy((const unsigned char *)buf, len);
    free(buf);
    return s;
}

/* an inheritable copy of h for the child (NULL when there is none) */
static HANDLE br_inheritable(HANDLE h) {
    if (h == NULL || h == INVALID_HANDLE_VALUE) return NULL;
    HANDLE d = NULL;
    if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &d, 0, TRUE, DUPLICATE_SAME_ACCESS)) return NULL;
    return d;
}

/* Rust's command-line quoting: the program is always quoted as is; other arguments
 * when they hold a space or tab or are empty, with backslashes before quotes doubled */
static void br_append_arg(brine_sb *sb, const char *a, int program) {
    int quote = program || !a[0] || strpbrk(a, " \t") != NULL;
    if (quote) brine_sb_cat(sb, "\"", 1);
    int64_t backslashes = 0;
    for (const char *p = a; *p; p++) {
        if (!program) {
            if (*p == '\\') backslashes++;
            else {
                if (*p == '"')
                    for (int64_t i = 0; i <= backslashes; i++) brine_sb_cat(sb, "\\", 1);
                backslashes = 0;
            }
        }
        brine_sb_cat(sb, p, 1);
    }
    if (quote) {
        if (!program)
            for (int64_t i = 0; i < backslashes; i++) brine_sb_cat(sb, "\\", 1);
        brine_sb_cat(sb, "\"", 1);
    }
}

static int br_wfile_exists(const wchar_t *w) {
    DWORD a = GetFileAttributesW(w);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* dir + "\" + name if that file exists, else NULL */
static wchar_t *br_try_dir(const wchar_t *dir, size_t dl, const wchar_t *name) {
    size_t wl = wcslen(name);
    if (dl == 0) return NULL;
    wchar_t *c = (wchar_t *)brine_alloc((int64_t)sizeof(wchar_t) * (dl + wl + 2));
    memcpy(c, dir, dl * sizeof(wchar_t));
    size_t at = dl;
    if (c[at - 1] != L'\\' && c[at - 1] != L'/') c[at++] = L'\\';
    memcpy(c + at, name, (wl + 1) * sizeof(wchar_t));
    if (br_wfile_exists(c)) return c;
    free(c);
    return NULL;
}

/* Find a program the way Rust's Command does on Windows: a name without a path is
 * looked up in the exe's directory, the system directories, then PATH; ".exe" is
 * added when the name has no extension. NULL if not found. */
static wchar_t *br_find_program(const char *prog, const wchar_t *path_var) {
    size_t n = strlen(prog);
    const char *base = prog;
    for (const char *q = prog; *q; q++)
        if (*q == '/' || *q == '\\') base = q + 1;
    char *name = (char *)brine_alloc((int64_t)n + 5);
    memcpy(name, prog, n + 1);
    if (!strchr(base, '.')) memcpy(name + n, ".exe", 5);
    wchar_t *wname = br_wide(name);
    free(name);
    if (base != prog) return wname; /* a path: CreateProcess reports it if it's missing */
    wchar_t dirs[3][MAX_PATH + 1];
    int nd = 0;
    DWORD k = GetModuleFileNameW(NULL, dirs[nd], MAX_PATH);
    if (k > 0 && k < MAX_PATH) {
        wchar_t *slash = wcsrchr(dirs[nd], L'\\');
        if (slash) {
            *slash = 0;
            nd++;
        }
    }
    k = GetSystemDirectoryW(dirs[nd], MAX_PATH);
    if (k > 0 && k < MAX_PATH) nd++;
    k = GetWindowsDirectoryW(dirs[nd], MAX_PATH);
    if (k > 0 && k < MAX_PATH) nd++;
    wchar_t *found = NULL;
    for (int i = 0; i < nd && !found; i++) found = br_try_dir(dirs[i], wcslen(dirs[i]), wname);
    for (const wchar_t *p = path_var; p && *p && !found;) {
        const wchar_t *e = wcschr(p, L';');
        size_t dl = e ? (size_t)(e - p) : wcslen(p);
        const wchar_t *d = p;
        p = e ? e + 1 : p + dl;
        if (dl >= 2 && d[0] == L'"' && d[dl - 1] == L'"') { /* entries may be quoted */
            d++;
            dl -= 2;
        }
        found = br_try_dir(d, dl, wname);
    }
    free(wname);
    return found;
}

/* does environment entry `e` ("NAME=value") have this name? (case-insensitive, like Windows) */
static int br_env_is(const wchar_t *e, const wchar_t *name) {
    const wchar_t *eq = wcschr(e + 1, L'='); /* hidden names start with '=' */
    size_t el = eq ? (size_t)(eq - e) : wcslen(e), nl = wcslen(name);
    return el == nl && CompareStringOrdinal(e, (int)el, name, (int)nl, TRUE) == CSTR_EQUAL;
}

/* the parent's environment block with the overrides applied (NULL: inherit as is) */
static wchar_t *br_env_block(char **ek, char **ev, int64_t ne) {
    if (ne == 0) return NULL;
    wchar_t **wk = (wchar_t **)brine_alloc((int64_t)sizeof(wchar_t *) * ne);
    wchar_t **wv = (wchar_t **)brine_alloc((int64_t)sizeof(wchar_t *) * ne);
    for (int64_t i = 0; i < ne; i++) {
        wk[i] = br_wide(ek[i]);
        wv[i] = br_wide(ev[i]);
    }
    wchar_t *cur = GetEnvironmentStringsW();
    size_t cap = 2, len = 0;
    for (const wchar_t *e = cur; e && *e; e += wcslen(e) + 1) cap += wcslen(e) + 1;
    for (int64_t i = 0; i < ne; i++) cap += wcslen(wk[i]) + wcslen(wv[i]) + 2;
    wchar_t *block = (wchar_t *)brine_alloc((int64_t)sizeof(wchar_t) * cap);
    for (const wchar_t *e = cur; e && *e; e += wcslen(e) + 1) {
        int skip = 0;
        for (int64_t i = 0; i < ne && !skip; i++) skip = br_env_is(e, wk[i]);
        if (skip) continue;
        size_t l = wcslen(e) + 1;
        memcpy(block + len, e, l * sizeof(wchar_t));
        len += l;
    }
    if (cur) FreeEnvironmentStringsW(cur);
    for (int64_t i = 0; i < ne; i++) {
        size_t kl = wcslen(wk[i]), vl = wcslen(wv[i]);
        memcpy(block + len, wk[i], kl * sizeof(wchar_t));
        len += kl;
        block[len++] = L'=';
        memcpy(block + len, wv[i], vl * sizeof(wchar_t));
        len += vl;
        block[len++] = 0;
        free(wk[i]);
        free(wv[i]);
    }
    block[len++] = 0;
    if (len == 1) block[len++] = 0; /* an empty block still needs two NULs */
    free(wk);
    free(wv);
    return block;
}

/* start the process; false with *err set if it couldn't be started */
static bool br_start(br_proc *rec, char **cv, int64_t n, char **ek, char **ev, int64_t ne, bool capture, bool in, brine_str **err) {
    wchar_t *path_var = NULL; /* the program is looked up on the child's PATH */
    for (int64_t i = 0; i < ne; i++)
        if (!_stricmp(ek[i], "PATH")) {
            free(path_var);
            path_var = br_wide(ev[i]);
        }
    if (!path_var) {
        DWORD need = GetEnvironmentVariableW(L"PATH", NULL, 0);
        if (need) {
            path_var = (wchar_t *)brine_alloc((int64_t)sizeof(wchar_t) * need);
            if (!GetEnvironmentVariableW(L"PATH", path_var, need)) path_var[0] = 0;
        }
    }
    wchar_t *app = br_find_program(cv[0], path_var);
    free(path_var);
    if (!app) {
        *err = br_strf("program not found");
        return false;
    }
    brine_sb sb;
    brine_sb_init(&sb);
    br_append_arg(&sb, cv[0], 1);
    for (int64_t i = 1; i < n; i++) {
        brine_sb_cat(&sb, " ", 1);
        br_append_arg(&sb, cv[i], 0);
    }
    brine_str *line = brine_sb_finish(&sb);
    char *lc = br_cstr(line);
    brine_str_release(line);
    wchar_t *wl = br_wide(lc);
    free(lc);
    wchar_t *block = br_env_block(ek, ev, ne);
    HANDLE in_h = NULL, out_h = NULL, err_h = NULL;
    if (in) in_h = br_inheritable(GetStdHandle(STD_INPUT_HANDLE));
    else {
        SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
        in_h = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
        if (in_h == INVALID_HANDLE_VALUE) in_h = NULL;
    }
    bool ok = true;
    rec->out_f = rec->err_f = INVALID_HANDLE_VALUE;
    if (capture) {
        rec->out_f = br_capture_file();
        rec->err_f = rec->out_f != INVALID_HANDLE_VALUE ? br_capture_file() : INVALID_HANDLE_VALUE;
        if (rec->err_f == INVALID_HANDLE_VALUE) {
            *err = br_os_err((int)GetLastError());
            ok = false;
        }
        out_h = br_inheritable(rec->out_f);
        err_h = br_inheritable(rec->err_f);
    } else {
        out_h = br_inheritable(GetStdHandle(STD_OUTPUT_HANDLE));
        err_h = br_inheritable(GetStdHandle(STD_ERROR_HANDLE));
    }
    if (ok) {
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        memset(&si, 0, sizeof si);
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = in_h;
        si.hStdOutput = out_h;
        si.hStdError = err_h;
        if (!CreateProcessW(app, wl, NULL, NULL, TRUE, CREATE_UNICODE_ENVIRONMENT, block, NULL, &si, &pi)) {
            *err = br_os_err((int)GetLastError());
            ok = false;
        } else {
            CloseHandle(pi.hThread);
            rec->proc = pi.hProcess;
        }
    }
    if (in_h) CloseHandle(in_h);
    if (out_h) CloseHandle(out_h);
    if (err_h) CloseHandle(err_h);
    if (!ok) {
        if (rec->out_f != INVALID_HANDLE_VALUE) CloseHandle(rec->out_f);
        if (rec->err_f != INVALID_HANDLE_VALUE) CloseHandle(rec->err_f);
    }
    free(block);
    free(wl);
    free(app);
    return ok;
}
#else
static int br_capture_fd(void) {
    brine_str *dir = brine_os_temp_dir();
    brine_sb sb;
    brine_sb_init(&sb);
    brine_sb_str(&sb, dir);
    brine_str_release(dir);
    brine_sb_cat(&sb, "/brine-out-XXXXXX", 17);
    brine_str *p = brine_sb_finish(&sb);
    char *c = br_cstr(p);
    brine_str_release(p);
    int fd = c ? mkstemp(c) : -1;
    if (fd >= 0) {
        unlink(c);
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    free(c);
    return fd;
}

static brine_str *br_read_capture(int fd) {
    if (fd < 0) return brine_str_new("", 0);
    lseek(fd, 0, SEEK_SET);
    char *buf = NULL;
    size_t len = 0, cap = 0;
    for (;;) {
        if (len == cap) {
            cap = cap ? cap * 2 : 65536;
            buf = (char *)br_grow(buf, cap);
        }
        ssize_t got = read(fd, buf + len, cap - len);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        len += (size_t)got;
    }
    close(fd);
    brine_str *s = br_lossy((const unsigned char *)buf, len);
    free(buf);
    return s;
}

static bool br_start(br_proc *rec, char **cv, int64_t n, char **ek, char **ev, int64_t ne, bool capture, bool in, brine_str **err) {
    (void)n;
    /* environment: the parent's, minus overridden names, plus the overrides */
    int64_t np = 0;
    for (char **e = environ; *e; e++) np++;
    char **envp = (char **)brine_alloc((int64_t)sizeof(char *) * (np + ne + 1));
    char **owned = (char **)brine_alloc((int64_t)sizeof(char *) * (ne + 1));
    int64_t k = 0;
    for (char **e = environ; *e; e++) {
        int skip = 0;
        for (int64_t i = 0; i < ne && !skip; i++) {
            size_t kl = strlen(ek[i]);
            skip = !strncmp(*e, ek[i], kl) && (*e)[kl] == '=';
        }
        if (!skip) envp[k++] = *e;
    }
    for (int64_t i = 0; i < ne; i++) {
        size_t kl = strlen(ek[i]), vl = strlen(ev[i]);
        owned[i] = (char *)brine_alloc((int64_t)(kl + vl + 2));
        memcpy(owned[i], ek[i], kl);
        owned[i][kl] = '=';
        memcpy(owned[i] + kl + 1, ev[i], vl + 1);
        envp[k++] = owned[i];
    }
    envp[k] = NULL;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    /* like Rust: the child starts with SIGPIPE at its default action */
    sigset_t def;
    sigemptyset(&def);
    sigaddset(&def, SIGPIPE);
    posix_spawnattr_setsigdefault(&attr, &def);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF);
    rec->out_fd = rec->err_fd = -1;
    if (!in) posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    int r = 0;
    if (capture) {
        errno = 0;
        rec->out_fd = br_capture_fd();
        rec->err_fd = rec->out_fd >= 0 ? br_capture_fd() : -1;
        if (rec->out_fd < 0 || rec->err_fd < 0) r = errno ? errno : EIO;
        else {
            posix_spawn_file_actions_adddup2(&fa, rec->out_fd, 1);
            posix_spawn_file_actions_adddup2(&fa, rec->err_fd, 2);
        }
    }
    if (!r) r = posix_spawnp(&rec->pid, cv[0], &fa, &attr, cv, envp);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    if (r) {
        *err = br_os_err(r);
        if (rec->out_fd >= 0) close(rec->out_fd);
        if (rec->err_fd >= 0) close(rec->err_fd);
    }
    for (int64_t i = 0; i < ne; i++) free(owned[i]);
    free(owned);
    free(envp);
    return r == 0;
}
#endif

bool brine_os_spawn(brine_arr *cmd, brine_map *env, bool capture, bool in, int64_t *handle, brine_str **err) {
    *handle = 0;
    if (!br_proc_mu_ready) {
        br_mutex_init(&br_proc_mu);
        br_proc_mu_ready = 1;
    }
    if (cmd->len == 0) {
        *err = br_strf("os.spawn needs a program name, but the command list is empty");
        return false;
    }
    brine_str **items = (brine_str **)cmd->data;
    int64_t n = cmd->len;
    char **cv = (char **)brine_alloc((int64_t)sizeof(char *) * (n + 1));
    bool ok = true, bad_name = false;
    for (int64_t i = 0; i < n; i++) {
        cv[i] = br_cstr(items[i]);
        if (!cv[i]) ok = false;
    }
    cv[n] = NULL;
    /* the environment overrides */
    int64_t ne = 0;
    char **ek = (char **)brine_alloc((int64_t)sizeof(char *) * (env->len + 1));
    char **ev = (char **)brine_alloc((int64_t)sizeof(char *) * (env->len + 1));
    for (int64_t i = 0; i < env->used; i++) {
        if (!env->live[i]) continue;
        ek[ne] = br_cstr(((brine_str **)env->keys)[i]);
        ev[ne] = br_cstr(((brine_str **)env->vals)[i]);
        if (!ek[ne] || !ev[ne]) ok = false;
        else if (!ek[ne][0] || strchr(ek[ne] + 1, '=')) bad_name = true;
        ne++;
    }
    br_proc rec;
    memset(&rec, 0, sizeof rec);
    rec.capture = capture;
    if (!ok) {
        *err = br_strf("nul byte found in provided data");
    } else if (bad_name) {
        *err = br_strf("os.spawn: invalid environment variable name");
        ok = false;
    } else {
        br_flush();
        fflush(stderr);
        ok = br_start(&rec, cv, n, ek, ev, ne, capture, in, err);
    }
    for (int64_t i = 0; i < n; i++) free(cv[i]);
    free(cv);
    for (int64_t i = 0; i < ne; i++) {
        free(ek[i]);
        free(ev[i]);
    }
    free(ek);
    free(ev);
    if (!ok) return false;
    br_lock(&br_proc_mu);
    if (br_nprocs == br_cap_procs) {
        br_cap_procs = br_cap_procs ? br_cap_procs * 2 : 8;
        br_procs = (br_proc *)br_grow(br_procs, sizeof(br_proc) * (size_t)br_cap_procs);
    }
    br_procs[br_nprocs++] = rec;
    *handle = br_nprocs;
    br_unlock(&br_proc_mu);
    return true;
}

#ifndef _WIN32
static int64_t br_status_code(int st) { return WIFEXITED(st) ? (int64_t)WEXITSTATUS(st) : 101; }
#endif

/* Has it exited? (block: wait until it has.) Collects its output when it has. */
static bool br_proc_reap(br_proc *p, bool block) {
    if (p->done) return true;
    int64_t code;
#ifdef _WIN32
    if (WaitForSingleObject(p->proc, block ? INFINITE : 0) != WAIT_OBJECT_0) return false;
    DWORD c = 0;
    GetExitCodeProcess(p->proc, &c);
    CloseHandle(p->proc);
    p->proc = NULL;
    code = (int64_t)(int32_t)c;
    p->out = p->capture ? br_read_capture(p->out_f) : brine_str_new("", 0);
    p->err = p->capture ? br_read_capture(p->err_f) : brine_str_new("", 0);
#else
    int st = 0;
    pid_t r;
    while ((r = waitpid(p->pid, &st, block ? 0 : WNOHANG)) < 0 && errno == EINTR) {
    }
    if (r == 0) return false;
    code = r < 0 ? 101 : br_status_code(st);
    p->out = p->capture ? br_read_capture(p->out_fd) : brine_str_new("", 0);
    p->err = p->capture ? br_read_capture(p->err_fd) : brine_str_new("", 0);
#endif
    p->done = true;
    p->code = code;
    return true;
}

/* the process for a handle, with br_proc_mu held (panics on a bad handle) */
static br_proc *br_proc_lock(int64_t h, const char *what, const char *loc) {
    if (!br_proc_mu_ready) br_proc_get(h, what, loc); /* nothing was spawned: always invalid */
    br_lock(&br_proc_mu);
    return br_proc_get(h, what, loc);
}

int64_t brine_os_wait(int64_t handle, const char *loc) {
    br_proc *p = br_proc_lock(handle, "os.wait", loc);
    br_proc_reap(p, true);
    int64_t c = p->code;
    br_unlock(&br_proc_mu);
    return c;
}

bool brine_os_poll(int64_t handle, int64_t *code, const char *loc) {
    br_proc *p = br_proc_lock(handle, "os.poll", loc);
    bool done = br_proc_reap(p, false);
    *code = done ? p->code : 0;
    br_unlock(&br_proc_mu);
    return done;
}

void brine_os_kill(int64_t handle, const char *loc) {
    br_proc *p = br_proc_lock(handle, "os.kill", loc);
    if (!p->done) {
#ifdef _WIN32
        TerminateProcess(p->proc, 1);
#else
        kill(p->pid, SIGKILL);
#endif
        br_proc_reap(p, true);
    }
    br_unlock(&br_proc_mu);
}

void brine_os_output(int64_t handle, brine_str **out, brine_str **err, const char *loc) {
    br_proc *p = br_proc_lock(handle, "os.output", loc);
    if (!br_proc_reap(p, false)) {
        br_unlock(&br_proc_mu);
        brine_panic_at(loc, "os.output: the process is still running; call os.wait first");
    }
    brine_str_retain(p->out);
    brine_str_retain(p->err);
    *out = p->out;
    *err = p->err;
    br_unlock(&br_proc_mu);
}

/* at exit: drop what finished processes wrote (running ones are left alone) */
static void br_procs_free(void) {
    for (int64_t i = 0; i < br_nprocs; i++) {
        if (!br_procs[i].done) continue;
        brine_str_release(br_procs[i].out);
        brine_str_release(br_procs[i].err);
        br_procs[i].out = br_procs[i].err = NULL;
    }
    free(br_procs);
    br_procs = NULL;
    br_nprocs = br_cap_procs = 0;
}
