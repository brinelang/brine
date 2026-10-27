/* brine runtime — implementation. Compiled as a separate translation unit. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
/* glibc keeps MAP_ANONYMOUS and MAP_NORESERVE behind this */
#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
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
#include <stddef.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winternl.h>
#else
#include <pthread.h>
#include <dlfcn.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#endif

/* ============================================================ platform layer */
static void br_heap_exit(void);
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
    br_heap_exit();
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
    br_heap_exit();
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
static void br_gpu_settle(void);

void brine_panic_at(const char *loc, const char *msg) {
    /* under the output lock, so other threads' output (the GPU log) can't
     * land in the middle */
    br_lock(&br_io_mu);
    br_flush();
    if (br_size > 1)
        fprintf(stderr, "\npanic (rank %lld): %s\n  --> %s\n", (long long)br_rank, msg, loc);
    else
        fprintf(stderr, "\npanic: %s\n  --> %s\n", msg, loc);
    fflush(stderr);
    br_unlock(&br_io_mu);
    br_gpu_settle();
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
    br_gpu_settle();
    exit((int)code);
}
void brine_unreachable(void) {
    br_flush();
    fprintf(stderr, "internal error: reached unreachable code\n");
    abort();
}

/* ============================================================ memory */
/* Small blocks (strings, boxes, array headers) come from a heap of our own:
   one big reserved address range cut into 64 KiB chunks, each holding blocks
   of one size class, with a free list per class and thread. A range check
   tells its blocks from malloc's, so br_free and br_realloc take either, and
   every free/realloc in this file goes through them (see the macros below).
   A thread that frees far more than it allocates (one consuming what others
   made), or that exits, hands its free blocks over in batches of about
   BR_SPARE bytes, and a thread that runs out takes one batch, so memory
   doesn't pile up on any one thread. */
#define BR_SMALL 256
#define BR_NCLASS 12
#define BR_CHUNK ((uintptr_t)1 << 16)
#define BR_RESERVE ((uintptr_t)1 << 36)
#define BR_SPARE ((int64_t)1 << 16) /* bytes a thread may keep free per class */
static uintptr_t br_heap_base, br_heap_end, br_heap_next;
static uint8_t br_chunk_class[BR_RESERVE / BR_CHUNK];
static br_mutex br_heap_mu;
/* a stack of batches per class: a batch is a free list, and its first block's
   second word links the next batch */
static void *br_batches[BR_NCLASS];

typedef struct {
    void *head[BR_NCLASS];
    int64_t count[BR_NCLASS];
    char *bump[BR_NCLASS], *end[BR_NCLASS];
} br_heap_th;
#if defined(_WIN32) && !defined(__clang__)
/* MinGW gcc emulates thread-locals with a library call per access, which
   would cost more than the allocation; a TLS slot in the thread block is one
   load */
static DWORD br_th_key = TLS_OUT_OF_INDEXES;
static br_heap_th *br_th_peek(void) {
    return br_th_key < 64 ? (br_heap_th *)((TEB *)NtCurrentTeb())->TlsSlots[br_th_key] : (br_heap_th *)TlsGetValue(br_th_key);
}
static br_heap_th *br_th_new(void) {
    br_heap_th *h = (br_heap_th *)calloc(1, sizeof *h);
    if (!h) {
        fprintf(stderr, "out of memory\n");
        abort();
    }
    TlsSetValue(br_th_key, h);
    return h;
}
static inline br_heap_th *br_th_get(void) {
    br_heap_th *h = br_th_peek();
    return h ? h : br_th_new();
}
static void br_th_done(br_heap_th *h) {
    TlsSetValue(br_th_key, NULL);
    free(h);
}
#else
static _Thread_local br_heap_th br_th;
static br_heap_th *br_th_peek(void) { return &br_th; }
static inline br_heap_th *br_th_get(void) { return &br_th; }
static void br_th_done(br_heap_th *h) { (void)h; }
#endif

static int br_class_of(size_t n) { return n <= 128 ? (int)((n + 15) / 16) - 1 : 8 + (int)((n - 129) / 32); }
static size_t br_class_size(int c) { return c < 8 ? (size_t)(c + 1) * 16 : 128 + (size_t)(c - 7) * 32; }
static int br_in_heap(const void *p) { return (uintptr_t)p - br_heap_base < br_heap_end - br_heap_base; }

/* once, before any thread starts; without the reservation everything is malloc */
static void br_heap_init(void) {
    if (br_heap_base) return;
    br_mutex_init(&br_heap_mu);
#ifdef _WIN32
#ifndef __clang__
    br_th_key = TlsAlloc();
    if (br_th_key == TLS_OUT_OF_INDEXES) return;
#endif
    /* reservations are 64 KiB aligned */
    uintptr_t b = (uintptr_t)VirtualAlloc(NULL, BR_RESERVE, MEM_RESERVE, PAGE_NOACCESS);
#else
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_NORESERVE
    flags |= MAP_NORESERVE;
#endif
    void *m = mmap(NULL, BR_RESERVE + BR_CHUNK, PROT_NONE, flags, -1, 0);
    uintptr_t b = m == MAP_FAILED ? 0 : ((uintptr_t)m + BR_CHUNK - 1) & ~(BR_CHUNK - 1);
#endif
    if (!b) return;
    br_heap_next = b;
    br_heap_end = b + BR_RESERVE;
    br_heap_base = b;
}

static char *br_chunk_new(int c) {
    uintptr_t ch = __atomic_fetch_add(&br_heap_next, BR_CHUNK, __ATOMIC_RELAXED);
    if (ch + BR_CHUNK > br_heap_end) return NULL;
#ifdef _WIN32
    if (!VirtualAlloc((void *)ch, BR_CHUNK, MEM_COMMIT, PAGE_READWRITE)) return NULL;
#else
    if (mprotect((void *)ch, BR_CHUNK, PROT_READ | PROT_WRITE) != 0) return NULL;
#endif
    br_chunk_class[(ch - br_heap_base) / BR_CHUNK] = (uint8_t)c;
    return (char *)ch;
}

static void *br_big(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) {
        fprintf(stderr, "out of memory\n");
        abort();
    }
    return p;
}

/* hands a thread's free list for class c over as one batch */
static void br_give(br_heap_th *h, int c) {
    void *p = h->head[c];
    if (!p) return;
    br_lock(&br_heap_mu);
    ((void **)p)[1] = br_batches[c];
    br_batches[c] = p;
    br_unlock(&br_heap_mu);
    h->head[c] = NULL;
    h->count[c] = 0;
}

static void *br_alloc_slow(br_heap_th *h, int c, size_t n) {
    size_t sz = br_class_size(c);
    if (h->bump[c] && (size_t)(h->end[c] - h->bump[c]) >= sz) {
        void *p = h->bump[c];
        h->bump[c] += sz;
        return p;
    }
    if (__atomic_load_n(&br_batches[c], __ATOMIC_RELAXED)) {
        br_lock(&br_heap_mu);
        void *b = br_batches[c];
        if (b) br_batches[c] = ((void **)b)[1];
        br_unlock(&br_heap_mu);
        if (b) {
            h->head[c] = *(void **)b;
            h->count[c] = 0;
            return b;
        }
    }
    char *ch = br_chunk_new(c);
    if (!ch) return br_big(n);
    h->bump[c] = ch + sz;
    h->end[c] = ch + BR_CHUNK;
    return ch;
}

static void *br_alloc(size_t n) {
    if (n > BR_SMALL || !br_heap_base) return br_big(n);
    int c = br_class_of(n ? n : 1);
    br_heap_th *h = br_th_get();
    void *p = h->head[c];
    if (p) {
        h->head[c] = *(void **)p;
        if (h->count[c] > 0) h->count[c]--;
        return p;
    }
    return br_alloc_slow(h, c, n);
}

static void br_free(void *p) {
    if (!br_in_heap(p)) {
        free(p);
        return;
    }
    int c = br_chunk_class[((uintptr_t)p - br_heap_base) / BR_CHUNK];
    br_heap_th *h = br_th_get();
    *(void **)p = h->head[c];
    h->head[c] = p;
    if (++h->count[c] * (int64_t)br_class_size(c) > BR_SPARE) br_give(h, c);
}

static void *br_realloc(void *p, size_t n) {
    if (!p) return br_alloc(n);
    if (!br_in_heap(p)) return realloc(p, n ? n : 1);
    size_t have = br_class_size(br_chunk_class[((uintptr_t)p - br_heap_base) / BR_CHUNK]);
    if (n <= have) return p;
    void *q = br_alloc(n);
    memcpy(q, p, have);
    br_free(p);
    return q;
}

/* a thread's last act: its free blocks, and what is left of its chunks, go
   to the shared lists */
static void br_heap_exit(void) {
    if (!br_heap_base) return;
    br_heap_th *h = br_th_peek();
    if (!h) return;
    for (int c = 0; c < BR_NCLASS; c++) {
        size_t sz = br_class_size(c);
        while (h->bump[c] && (size_t)(h->end[c] - h->bump[c]) >= sz) {
            void *p = h->bump[c];
            h->bump[c] += sz;
            *(void **)p = h->head[c];
            h->head[c] = p;
        }
        br_give(h, c);
    }
    br_th_done(h);
}

#define free(p) br_free(p)
#define realloc(p, n) br_realloc(p, n)

void *brine_alloc(int64_t n) { return br_alloc(n > 0 ? (size_t)n : 1); }
void brine_free(void *p) { br_free(p); }
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
#define br_str_size brine_str_size
static brine_str *br_str_alloc(int64_t n) {
    brine_str *s = (brine_str *)brine_alloc((int64_t)br_str_size(n));
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
/* `s += b`: grows s in place when nothing else holds it */
void brine_str_append(brine_str **ps, brine_str *b) {
    brine_str *a = *ps;
    if (b->len == 0) return;
    if (__atomic_load_n(&a->rc, __ATOMIC_ACQUIRE) != 1) {
        *ps = brine_str_concat(a, b);
        brine_str_release(a);
        return;
    }
    int64_t n = a->len + b->len, blen = b->len;
    bool self = a == b;
    brine_str *s = a;
    if (br_str_size(n) > br_str_size(a->len)) {
        s = (brine_str *)realloc(a, br_str_size(n));
        if (!s) {
            fprintf(stderr, "out of memory\n");
            abort();
        }
    }
    memcpy(s->data + s->len, self ? s->data : b->data, (size_t)blen);
    s->len = n;
    s->data[n] = 0;
    *ps = s;
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
#define BR_B1(i) {-1, 1, {(char)(i), 0}}
#define BR_B4(i) BR_B1(i), BR_B1((i) + 1), BR_B1((i) + 2), BR_B1((i) + 3)
#define BR_B16(i) BR_B4(i), BR_B4((i) + 4), BR_B4((i) + 8), BR_B4((i) + 12)
#define BR_B64(i) BR_B16(i), BR_B16((i) + 16), BR_B16((i) + 32), BR_B16((i) + 48)
brine_str1 brine_bytes[256] = {BR_B64(0), BR_B64(64), BR_B64(128), BR_B64(192)};

static brine_str *br_sub(brine_str *s, int64_t lo, int64_t hi) {
    if (lo == 0 && hi == s->len) {
        brine_str_retain(s);
        return s;
    }
    if (hi - lo == 1) return (brine_str *)&brine_bytes[(unsigned char)s->data[lo]];
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
    if (c < 0x80) return (brine_str *)&brine_bytes[c];
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
/* ============================================================ unicode */
typedef struct {
    uint32_t lo, hi;
    int32_t delta;
    uint32_t step;
} br_case_run;
typedef struct {
    uint32_t cp;
    const char *to;
} br_case_special;

/* Unicode 16.0.0 tables, from the Unicode Character Database (as Python's
   str.upper, str.lower, str.isalpha and str.isspace see it). A run maps
   lo, lo+step, ..., hi by adding delta. */
static const br_case_run br_upper_runs[] = {
    {0x61, 0x7a, -32, 1}, {0xb5, 0xb5, 743, 1}, {0xe0, 0xf6, -32, 1}, {0xf8, 0xfe, -32, 1},
    {0xff, 0xff, 121, 1}, {0x101, 0x12f, -1, 2}, {0x131, 0x131, -232, 1}, {0x133, 0x137, -1, 2},
    {0x13a, 0x148, -1, 2}, {0x14b, 0x177, -1, 2}, {0x17a, 0x17e, -1, 2}, {0x17f, 0x17f, -300, 1},
    {0x180, 0x180, 195, 1}, {0x183, 0x185, -1, 2}, {0x188, 0x188, -1, 1}, {0x18c, 0x18c, -1, 1},
    {0x192, 0x192, -1, 1}, {0x195, 0x195, 97, 1}, {0x199, 0x199, -1, 1}, {0x19a, 0x19a, 163, 1},
    {0x19b, 0x19b, 42561, 1}, {0x19e, 0x19e, 130, 1}, {0x1a1, 0x1a5, -1, 2}, {0x1a8, 0x1a8, -1, 1},
    {0x1ad, 0x1ad, -1, 1}, {0x1b0, 0x1b0, -1, 1}, {0x1b4, 0x1b6, -1, 2}, {0x1b9, 0x1b9, -1, 1},
    {0x1bd, 0x1bd, -1, 1}, {0x1bf, 0x1bf, 56, 1}, {0x1c5, 0x1c5, -1, 1}, {0x1c6, 0x1c6, -2, 1},
    {0x1c8, 0x1c8, -1, 1}, {0x1c9, 0x1c9, -2, 1}, {0x1cb, 0x1cb, -1, 1}, {0x1cc, 0x1cc, -2, 1},
    {0x1ce, 0x1dc, -1, 2}, {0x1dd, 0x1dd, -79, 1}, {0x1df, 0x1ef, -1, 2}, {0x1f2, 0x1f2, -1, 1},
    {0x1f3, 0x1f3, -2, 1}, {0x1f5, 0x1f5, -1, 1}, {0x1f9, 0x21f, -1, 2}, {0x223, 0x233, -1, 2},
    {0x23c, 0x23c, -1, 1}, {0x23f, 0x240, 10815, 1}, {0x242, 0x242, -1, 1}, {0x247, 0x24f, -1, 2},
    {0x250, 0x250, 10783, 1}, {0x251, 0x251, 10780, 1}, {0x252, 0x252, 10782, 1}, {0x253, 0x253, -210, 1},
    {0x254, 0x254, -206, 1}, {0x256, 0x257, -205, 1}, {0x259, 0x259, -202, 1}, {0x25b, 0x25b, -203, 1},
    {0x25c, 0x25c, 42319, 1}, {0x260, 0x260, -205, 1}, {0x261, 0x261, 42315, 1}, {0x263, 0x263, -207, 1},
    {0x264, 0x264, 42343, 1}, {0x265, 0x265, 42280, 1}, {0x266, 0x266, 42308, 1}, {0x268, 0x268, -209, 1},
    {0x269, 0x269, -211, 1}, {0x26a, 0x26a, 42308, 1}, {0x26b, 0x26b, 10743, 1}, {0x26c, 0x26c, 42305, 1},
    {0x26f, 0x26f, -211, 1}, {0x271, 0x271, 10749, 1}, {0x272, 0x272, -213, 1}, {0x275, 0x275, -214, 1},
    {0x27d, 0x27d, 10727, 1}, {0x280, 0x280, -218, 1}, {0x282, 0x282, 42307, 1}, {0x283, 0x283, -218, 1},
    {0x287, 0x287, 42282, 1}, {0x288, 0x288, -218, 1}, {0x289, 0x289, -69, 1}, {0x28a, 0x28b, -217, 1},
    {0x28c, 0x28c, -71, 1}, {0x292, 0x292, -219, 1}, {0x29d, 0x29d, 42261, 1}, {0x29e, 0x29e, 42258, 1},
    {0x345, 0x345, 84, 1}, {0x371, 0x373, -1, 2}, {0x377, 0x377, -1, 1}, {0x37b, 0x37d, 130, 1},
    {0x3ac, 0x3ac, -38, 1}, {0x3ad, 0x3af, -37, 1}, {0x3b1, 0x3c1, -32, 1}, {0x3c2, 0x3c2, -31, 1},
    {0x3c3, 0x3cb, -32, 1}, {0x3cc, 0x3cc, -64, 1}, {0x3cd, 0x3ce, -63, 1}, {0x3d0, 0x3d0, -62, 1},
    {0x3d1, 0x3d1, -57, 1}, {0x3d5, 0x3d5, -47, 1}, {0x3d6, 0x3d6, -54, 1}, {0x3d7, 0x3d7, -8, 1},
    {0x3d9, 0x3ef, -1, 2}, {0x3f0, 0x3f0, -86, 1}, {0x3f1, 0x3f1, -80, 1}, {0x3f2, 0x3f2, 7, 1},
    {0x3f3, 0x3f3, -116, 1}, {0x3f5, 0x3f5, -96, 1}, {0x3f8, 0x3f8, -1, 1}, {0x3fb, 0x3fb, -1, 1},
    {0x430, 0x44f, -32, 1}, {0x450, 0x45f, -80, 1}, {0x461, 0x481, -1, 2}, {0x48b, 0x4bf, -1, 2},
    {0x4c2, 0x4ce, -1, 2}, {0x4cf, 0x4cf, -15, 1}, {0x4d1, 0x52f, -1, 2}, {0x561, 0x586, -48, 1},
    {0x10d0, 0x10fa, 3008, 1}, {0x10fd, 0x10ff, 3008, 1}, {0x13f8, 0x13fd, -8, 1}, {0x1c80, 0x1c80, -6254, 1},
    {0x1c81, 0x1c81, -6253, 1}, {0x1c82, 0x1c82, -6244, 1}, {0x1c83, 0x1c84, -6242, 1}, {0x1c85, 0x1c85, -6243, 1},
    {0x1c86, 0x1c86, -6236, 1}, {0x1c87, 0x1c87, -6181, 1}, {0x1c88, 0x1c88, 35266, 1}, {0x1c8a, 0x1c8a, -1, 1},
    {0x1d79, 0x1d79, 35332, 1}, {0x1d7d, 0x1d7d, 3814, 1}, {0x1d8e, 0x1d8e, 35384, 1}, {0x1e01, 0x1e95, -1, 2},
    {0x1e9b, 0x1e9b, -59, 1}, {0x1ea1, 0x1eff, -1, 2}, {0x1f00, 0x1f07, 8, 1}, {0x1f10, 0x1f15, 8, 1},
    {0x1f20, 0x1f27, 8, 1}, {0x1f30, 0x1f37, 8, 1}, {0x1f40, 0x1f45, 8, 1}, {0x1f51, 0x1f57, 8, 2},
    {0x1f60, 0x1f67, 8, 1}, {0x1f70, 0x1f71, 74, 1}, {0x1f72, 0x1f75, 86, 1}, {0x1f76, 0x1f77, 100, 1},
    {0x1f78, 0x1f79, 128, 1}, {0x1f7a, 0x1f7b, 112, 1}, {0x1f7c, 0x1f7d, 126, 1}, {0x1fb0, 0x1fb1, 8, 1},
    {0x1fbe, 0x1fbe, -7205, 1}, {0x1fd0, 0x1fd1, 8, 1}, {0x1fe0, 0x1fe1, 8, 1}, {0x1fe5, 0x1fe5, 7, 1},
    {0x214e, 0x214e, -28, 1}, {0x2170, 0x217f, -16, 1}, {0x2184, 0x2184, -1, 1}, {0x24d0, 0x24e9, -26, 1},
    {0x2c30, 0x2c5f, -48, 1}, {0x2c61, 0x2c61, -1, 1}, {0x2c65, 0x2c65, -10795, 1}, {0x2c66, 0x2c66, -10792, 1},
    {0x2c68, 0x2c6c, -1, 2}, {0x2c73, 0x2c73, -1, 1}, {0x2c76, 0x2c76, -1, 1}, {0x2c81, 0x2ce3, -1, 2},
    {0x2cec, 0x2cee, -1, 2}, {0x2cf3, 0x2cf3, -1, 1}, {0x2d00, 0x2d25, -7264, 1}, {0x2d27, 0x2d27, -7264, 1},
    {0x2d2d, 0x2d2d, -7264, 1}, {0xa641, 0xa66d, -1, 2}, {0xa681, 0xa69b, -1, 2}, {0xa723, 0xa72f, -1, 2},
    {0xa733, 0xa76f, -1, 2}, {0xa77a, 0xa77c, -1, 2}, {0xa77f, 0xa787, -1, 2}, {0xa78c, 0xa78c, -1, 1},
    {0xa791, 0xa793, -1, 2}, {0xa794, 0xa794, 48, 1}, {0xa797, 0xa7a9, -1, 2}, {0xa7b5, 0xa7c3, -1, 2},
    {0xa7c8, 0xa7ca, -1, 2}, {0xa7cd, 0xa7cd, -1, 1}, {0xa7d1, 0xa7d1, -1, 1}, {0xa7d7, 0xa7db, -1, 2},
    {0xa7f6, 0xa7f6, -1, 1}, {0xab53, 0xab53, -928, 1}, {0xab70, 0xabbf, -38864, 1}, {0xff41, 0xff5a, -32, 1},
    {0x10428, 0x1044f, -40, 1}, {0x104d8, 0x104fb, -40, 1}, {0x10597, 0x105a1, -39, 1}, {0x105a3, 0x105b1, -39, 1},
    {0x105b3, 0x105b9, -39, 1}, {0x105bb, 0x105bc, -39, 1}, {0x10cc0, 0x10cf2, -64, 1}, {0x10d70, 0x10d85, -32, 1},
    {0x118c0, 0x118df, -32, 1}, {0x16e60, 0x16e7f, -32, 1}, {0x1e922, 0x1e943, -34, 1},
};
static const br_case_run br_lower_runs[] = {
    {0x41, 0x5a, 32, 1}, {0xc0, 0xd6, 32, 1}, {0xd8, 0xde, 32, 1}, {0x100, 0x12e, 1, 2},
    {0x132, 0x136, 1, 2}, {0x139, 0x147, 1, 2}, {0x14a, 0x176, 1, 2}, {0x178, 0x178, -121, 1},
    {0x179, 0x17d, 1, 2}, {0x181, 0x181, 210, 1}, {0x182, 0x184, 1, 2}, {0x186, 0x186, 206, 1},
    {0x187, 0x187, 1, 1}, {0x189, 0x18a, 205, 1}, {0x18b, 0x18b, 1, 1}, {0x18e, 0x18e, 79, 1},
    {0x18f, 0x18f, 202, 1}, {0x190, 0x190, 203, 1}, {0x191, 0x191, 1, 1}, {0x193, 0x193, 205, 1},
    {0x194, 0x194, 207, 1}, {0x196, 0x196, 211, 1}, {0x197, 0x197, 209, 1}, {0x198, 0x198, 1, 1},
    {0x19c, 0x19c, 211, 1}, {0x19d, 0x19d, 213, 1}, {0x19f, 0x19f, 214, 1}, {0x1a0, 0x1a4, 1, 2},
    {0x1a6, 0x1a6, 218, 1}, {0x1a7, 0x1a7, 1, 1}, {0x1a9, 0x1a9, 218, 1}, {0x1ac, 0x1ac, 1, 1},
    {0x1ae, 0x1ae, 218, 1}, {0x1af, 0x1af, 1, 1}, {0x1b1, 0x1b2, 217, 1}, {0x1b3, 0x1b5, 1, 2},
    {0x1b7, 0x1b7, 219, 1}, {0x1b8, 0x1b8, 1, 1}, {0x1bc, 0x1bc, 1, 1}, {0x1c4, 0x1c4, 2, 1},
    {0x1c5, 0x1c5, 1, 1}, {0x1c7, 0x1c7, 2, 1}, {0x1c8, 0x1c8, 1, 1}, {0x1ca, 0x1ca, 2, 1},
    {0x1cb, 0x1db, 1, 2}, {0x1de, 0x1ee, 1, 2}, {0x1f1, 0x1f1, 2, 1}, {0x1f2, 0x1f4, 1, 2},
    {0x1f6, 0x1f6, -97, 1}, {0x1f7, 0x1f7, -56, 1}, {0x1f8, 0x21e, 1, 2}, {0x220, 0x220, -130, 1},
    {0x222, 0x232, 1, 2}, {0x23a, 0x23a, 10795, 1}, {0x23b, 0x23b, 1, 1}, {0x23d, 0x23d, -163, 1},
    {0x23e, 0x23e, 10792, 1}, {0x241, 0x241, 1, 1}, {0x243, 0x243, -195, 1}, {0x244, 0x244, 69, 1},
    {0x245, 0x245, 71, 1}, {0x246, 0x24e, 1, 2}, {0x370, 0x372, 1, 2}, {0x376, 0x376, 1, 1},
    {0x37f, 0x37f, 116, 1}, {0x386, 0x386, 38, 1}, {0x388, 0x38a, 37, 1}, {0x38c, 0x38c, 64, 1},
    {0x38e, 0x38f, 63, 1}, {0x391, 0x3a1, 32, 1}, {0x3a3, 0x3ab, 32, 1}, {0x3cf, 0x3cf, 8, 1},
    {0x3d8, 0x3ee, 1, 2}, {0x3f4, 0x3f4, -60, 1}, {0x3f7, 0x3f7, 1, 1}, {0x3f9, 0x3f9, -7, 1},
    {0x3fa, 0x3fa, 1, 1}, {0x3fd, 0x3ff, -130, 1}, {0x400, 0x40f, 80, 1}, {0x410, 0x42f, 32, 1},
    {0x460, 0x480, 1, 2}, {0x48a, 0x4be, 1, 2}, {0x4c0, 0x4c0, 15, 1}, {0x4c1, 0x4cd, 1, 2},
    {0x4d0, 0x52e, 1, 2}, {0x531, 0x556, 48, 1}, {0x10a0, 0x10c5, 7264, 1}, {0x10c7, 0x10c7, 7264, 1},
    {0x10cd, 0x10cd, 7264, 1}, {0x13a0, 0x13ef, 38864, 1}, {0x13f0, 0x13f5, 8, 1}, {0x1c89, 0x1c89, 1, 1},
    {0x1c90, 0x1cba, -3008, 1}, {0x1cbd, 0x1cbf, -3008, 1}, {0x1e00, 0x1e94, 1, 2}, {0x1e9e, 0x1e9e, -7615, 1},
    {0x1ea0, 0x1efe, 1, 2}, {0x1f08, 0x1f0f, -8, 1}, {0x1f18, 0x1f1d, -8, 1}, {0x1f28, 0x1f2f, -8, 1},
    {0x1f38, 0x1f3f, -8, 1}, {0x1f48, 0x1f4d, -8, 1}, {0x1f59, 0x1f5f, -8, 2}, {0x1f68, 0x1f6f, -8, 1},
    {0x1f88, 0x1f8f, -8, 1}, {0x1f98, 0x1f9f, -8, 1}, {0x1fa8, 0x1faf, -8, 1}, {0x1fb8, 0x1fb9, -8, 1},
    {0x1fba, 0x1fbb, -74, 1}, {0x1fbc, 0x1fbc, -9, 1}, {0x1fc8, 0x1fcb, -86, 1}, {0x1fcc, 0x1fcc, -9, 1},
    {0x1fd8, 0x1fd9, -8, 1}, {0x1fda, 0x1fdb, -100, 1}, {0x1fe8, 0x1fe9, -8, 1}, {0x1fea, 0x1feb, -112, 1},
    {0x1fec, 0x1fec, -7, 1}, {0x1ff8, 0x1ff9, -128, 1}, {0x1ffa, 0x1ffb, -126, 1}, {0x1ffc, 0x1ffc, -9, 1},
    {0x2126, 0x2126, -7517, 1}, {0x212a, 0x212a, -8383, 1}, {0x212b, 0x212b, -8262, 1}, {0x2132, 0x2132, 28, 1},
    {0x2160, 0x216f, 16, 1}, {0x2183, 0x2183, 1, 1}, {0x24b6, 0x24cf, 26, 1}, {0x2c00, 0x2c2f, 48, 1},
    {0x2c60, 0x2c60, 1, 1}, {0x2c62, 0x2c62, -10743, 1}, {0x2c63, 0x2c63, -3814, 1}, {0x2c64, 0x2c64, -10727, 1},
    {0x2c67, 0x2c6b, 1, 2}, {0x2c6d, 0x2c6d, -10780, 1}, {0x2c6e, 0x2c6e, -10749, 1}, {0x2c6f, 0x2c6f, -10783, 1},
    {0x2c70, 0x2c70, -10782, 1}, {0x2c72, 0x2c72, 1, 1}, {0x2c75, 0x2c75, 1, 1}, {0x2c7e, 0x2c7f, -10815, 1},
    {0x2c80, 0x2ce2, 1, 2}, {0x2ceb, 0x2ced, 1, 2}, {0x2cf2, 0x2cf2, 1, 1}, {0xa640, 0xa66c, 1, 2},
    {0xa680, 0xa69a, 1, 2}, {0xa722, 0xa72e, 1, 2}, {0xa732, 0xa76e, 1, 2}, {0xa779, 0xa77b, 1, 2},
    {0xa77d, 0xa77d, -35332, 1}, {0xa77e, 0xa786, 1, 2}, {0xa78b, 0xa78b, 1, 1}, {0xa78d, 0xa78d, -42280, 1},
    {0xa790, 0xa792, 1, 2}, {0xa796, 0xa7a8, 1, 2}, {0xa7aa, 0xa7aa, -42308, 1}, {0xa7ab, 0xa7ab, -42319, 1},
    {0xa7ac, 0xa7ac, -42315, 1}, {0xa7ad, 0xa7ad, -42305, 1}, {0xa7ae, 0xa7ae, -42308, 1}, {0xa7b0, 0xa7b0, -42258, 1},
    {0xa7b1, 0xa7b1, -42282, 1}, {0xa7b2, 0xa7b2, -42261, 1}, {0xa7b3, 0xa7b3, 928, 1}, {0xa7b4, 0xa7c2, 1, 2},
    {0xa7c4, 0xa7c4, -48, 1}, {0xa7c5, 0xa7c5, -42307, 1}, {0xa7c6, 0xa7c6, -35384, 1}, {0xa7c7, 0xa7c9, 1, 2},
    {0xa7cb, 0xa7cb, -42343, 1}, {0xa7cc, 0xa7cc, 1, 1}, {0xa7d0, 0xa7d0, 1, 1}, {0xa7d6, 0xa7da, 1, 2},
    {0xa7dc, 0xa7dc, -42561, 1}, {0xa7f5, 0xa7f5, 1, 1}, {0xff21, 0xff3a, 32, 1}, {0x10400, 0x10427, 40, 1},
    {0x104b0, 0x104d3, 40, 1}, {0x10570, 0x1057a, 39, 1}, {0x1057c, 0x1058a, 39, 1}, {0x1058c, 0x10592, 39, 1},
    {0x10594, 0x10595, 39, 1}, {0x10c80, 0x10cb2, 64, 1}, {0x10d50, 0x10d65, 32, 1}, {0x118a0, 0x118bf, 32, 1},
    {0x16e40, 0x16e5f, 32, 1}, {0x1e900, 0x1e921, 34, 1},
};
/* characters that become several: upper("\u00df") is "SS" */
static const br_case_special br_upper_special[] = {
    {0xdf, "\x53\x53"},
    {0x149, "\xca\xbc\x4e"},
    {0x1f0, "\x4a\xcc\x8c"},
    {0x390, "\xce\x99\xcc\x88\xcc\x81"},
    {0x3b0, "\xce\xa5\xcc\x88\xcc\x81"},
    {0x587, "\xd4\xb5\xd5\x92"},
    {0x1e96, "\x48\xcc\xb1"},
    {0x1e97, "\x54\xcc\x88"},
    {0x1e98, "\x57\xcc\x8a"},
    {0x1e99, "\x59\xcc\x8a"},
    {0x1e9a, "\x41\xca\xbe"},
    {0x1f50, "\xce\xa5\xcc\x93"},
    {0x1f52, "\xce\xa5\xcc\x93\xcc\x80"},
    {0x1f54, "\xce\xa5\xcc\x93\xcc\x81"},
    {0x1f56, "\xce\xa5\xcc\x93\xcd\x82"},
    {0x1f80, "\xe1\xbc\x88\xce\x99"},
    {0x1f81, "\xe1\xbc\x89\xce\x99"},
    {0x1f82, "\xe1\xbc\x8a\xce\x99"},
    {0x1f83, "\xe1\xbc\x8b\xce\x99"},
    {0x1f84, "\xe1\xbc\x8c\xce\x99"},
    {0x1f85, "\xe1\xbc\x8d\xce\x99"},
    {0x1f86, "\xe1\xbc\x8e\xce\x99"},
    {0x1f87, "\xe1\xbc\x8f\xce\x99"},
    {0x1f88, "\xe1\xbc\x88\xce\x99"},
    {0x1f89, "\xe1\xbc\x89\xce\x99"},
    {0x1f8a, "\xe1\xbc\x8a\xce\x99"},
    {0x1f8b, "\xe1\xbc\x8b\xce\x99"},
    {0x1f8c, "\xe1\xbc\x8c\xce\x99"},
    {0x1f8d, "\xe1\xbc\x8d\xce\x99"},
    {0x1f8e, "\xe1\xbc\x8e\xce\x99"},
    {0x1f8f, "\xe1\xbc\x8f\xce\x99"},
    {0x1f90, "\xe1\xbc\xa8\xce\x99"},
    {0x1f91, "\xe1\xbc\xa9\xce\x99"},
    {0x1f92, "\xe1\xbc\xaa\xce\x99"},
    {0x1f93, "\xe1\xbc\xab\xce\x99"},
    {0x1f94, "\xe1\xbc\xac\xce\x99"},
    {0x1f95, "\xe1\xbc\xad\xce\x99"},
    {0x1f96, "\xe1\xbc\xae\xce\x99"},
    {0x1f97, "\xe1\xbc\xaf\xce\x99"},
    {0x1f98, "\xe1\xbc\xa8\xce\x99"},
    {0x1f99, "\xe1\xbc\xa9\xce\x99"},
    {0x1f9a, "\xe1\xbc\xaa\xce\x99"},
    {0x1f9b, "\xe1\xbc\xab\xce\x99"},
    {0x1f9c, "\xe1\xbc\xac\xce\x99"},
    {0x1f9d, "\xe1\xbc\xad\xce\x99"},
    {0x1f9e, "\xe1\xbc\xae\xce\x99"},
    {0x1f9f, "\xe1\xbc\xaf\xce\x99"},
    {0x1fa0, "\xe1\xbd\xa8\xce\x99"},
    {0x1fa1, "\xe1\xbd\xa9\xce\x99"},
    {0x1fa2, "\xe1\xbd\xaa\xce\x99"},
    {0x1fa3, "\xe1\xbd\xab\xce\x99"},
    {0x1fa4, "\xe1\xbd\xac\xce\x99"},
    {0x1fa5, "\xe1\xbd\xad\xce\x99"},
    {0x1fa6, "\xe1\xbd\xae\xce\x99"},
    {0x1fa7, "\xe1\xbd\xaf\xce\x99"},
    {0x1fa8, "\xe1\xbd\xa8\xce\x99"},
    {0x1fa9, "\xe1\xbd\xa9\xce\x99"},
    {0x1faa, "\xe1\xbd\xaa\xce\x99"},
    {0x1fab, "\xe1\xbd\xab\xce\x99"},
    {0x1fac, "\xe1\xbd\xac\xce\x99"},
    {0x1fad, "\xe1\xbd\xad\xce\x99"},
    {0x1fae, "\xe1\xbd\xae\xce\x99"},
    {0x1faf, "\xe1\xbd\xaf\xce\x99"},
    {0x1fb2, "\xe1\xbe\xba\xce\x99"},
    {0x1fb3, "\xce\x91\xce\x99"},
    {0x1fb4, "\xce\x86\xce\x99"},
    {0x1fb6, "\xce\x91\xcd\x82"},
    {0x1fb7, "\xce\x91\xcd\x82\xce\x99"},
    {0x1fbc, "\xce\x91\xce\x99"},
    {0x1fc2, "\xe1\xbf\x8a\xce\x99"},
    {0x1fc3, "\xce\x97\xce\x99"},
    {0x1fc4, "\xce\x89\xce\x99"},
    {0x1fc6, "\xce\x97\xcd\x82"},
    {0x1fc7, "\xce\x97\xcd\x82\xce\x99"},
    {0x1fcc, "\xce\x97\xce\x99"},
    {0x1fd2, "\xce\x99\xcc\x88\xcc\x80"},
    {0x1fd3, "\xce\x99\xcc\x88\xcc\x81"},
    {0x1fd6, "\xce\x99\xcd\x82"},
    {0x1fd7, "\xce\x99\xcc\x88\xcd\x82"},
    {0x1fe2, "\xce\xa5\xcc\x88\xcc\x80"},
    {0x1fe3, "\xce\xa5\xcc\x88\xcc\x81"},
    {0x1fe4, "\xce\xa1\xcc\x93"},
    {0x1fe6, "\xce\xa5\xcd\x82"},
    {0x1fe7, "\xce\xa5\xcc\x88\xcd\x82"},
    {0x1ff2, "\xe1\xbf\xba\xce\x99"},
    {0x1ff3, "\xce\xa9\xce\x99"},
    {0x1ff4, "\xce\x8f\xce\x99"},
    {0x1ff6, "\xce\xa9\xcd\x82"},
    {0x1ff7, "\xce\xa9\xcd\x82\xce\x99"},
    {0x1ffc, "\xce\xa9\xce\x99"},
    {0xfb00, "\x46\x46"},
    {0xfb01, "\x46\x49"},
    {0xfb02, "\x46\x4c"},
    {0xfb03, "\x46\x46\x49"},
    {0xfb04, "\x46\x46\x4c"},
    {0xfb05, "\x53\x54"},
    {0xfb06, "\x53\x54"},
    {0xfb13, "\xd5\x84\xd5\x86"},
    {0xfb14, "\xd5\x84\xd4\xb5"},
    {0xfb15, "\xd5\x84\xd4\xbb"},
    {0xfb16, "\xd5\x8e\xd5\x86"},
    {0xfb17, "\xd5\x84\xd4\xbd"},
};
static const br_case_special br_lower_special[] = {
    {0x130, "\x69\xcc\x87"},
};
static const uint32_t br_alpha_ranges[][2] = {
    {0x41, 0x5a}, {0x61, 0x7a}, {0xaa, 0xaa}, {0xb5, 0xb5}, {0xba, 0xba}, {0xc0, 0xd6},
    {0xd8, 0xf6}, {0xf8, 0x2c1}, {0x2c6, 0x2d1}, {0x2e0, 0x2e4}, {0x2ec, 0x2ec}, {0x2ee, 0x2ee},
    {0x370, 0x374}, {0x376, 0x377}, {0x37a, 0x37d}, {0x37f, 0x37f}, {0x386, 0x386}, {0x388, 0x38a},
    {0x38c, 0x38c}, {0x38e, 0x3a1}, {0x3a3, 0x3f5}, {0x3f7, 0x481}, {0x48a, 0x52f}, {0x531, 0x556},
    {0x559, 0x559}, {0x560, 0x588}, {0x5d0, 0x5ea}, {0x5ef, 0x5f2}, {0x620, 0x64a}, {0x66e, 0x66f},
    {0x671, 0x6d3}, {0x6d5, 0x6d5}, {0x6e5, 0x6e6}, {0x6ee, 0x6ef}, {0x6fa, 0x6fc}, {0x6ff, 0x6ff},
    {0x710, 0x710}, {0x712, 0x72f}, {0x74d, 0x7a5}, {0x7b1, 0x7b1}, {0x7ca, 0x7ea}, {0x7f4, 0x7f5},
    {0x7fa, 0x7fa}, {0x800, 0x815}, {0x81a, 0x81a}, {0x824, 0x824}, {0x828, 0x828}, {0x840, 0x858},
    {0x860, 0x86a}, {0x870, 0x887}, {0x889, 0x88e}, {0x8a0, 0x8c9}, {0x904, 0x939}, {0x93d, 0x93d},
    {0x950, 0x950}, {0x958, 0x961}, {0x971, 0x980}, {0x985, 0x98c}, {0x98f, 0x990}, {0x993, 0x9a8},
    {0x9aa, 0x9b0}, {0x9b2, 0x9b2}, {0x9b6, 0x9b9}, {0x9bd, 0x9bd}, {0x9ce, 0x9ce}, {0x9dc, 0x9dd},
    {0x9df, 0x9e1}, {0x9f0, 0x9f1}, {0x9fc, 0x9fc}, {0xa05, 0xa0a}, {0xa0f, 0xa10}, {0xa13, 0xa28},
    {0xa2a, 0xa30}, {0xa32, 0xa33}, {0xa35, 0xa36}, {0xa38, 0xa39}, {0xa59, 0xa5c}, {0xa5e, 0xa5e},
    {0xa72, 0xa74}, {0xa85, 0xa8d}, {0xa8f, 0xa91}, {0xa93, 0xaa8}, {0xaaa, 0xab0}, {0xab2, 0xab3},
    {0xab5, 0xab9}, {0xabd, 0xabd}, {0xad0, 0xad0}, {0xae0, 0xae1}, {0xaf9, 0xaf9}, {0xb05, 0xb0c},
    {0xb0f, 0xb10}, {0xb13, 0xb28}, {0xb2a, 0xb30}, {0xb32, 0xb33}, {0xb35, 0xb39}, {0xb3d, 0xb3d},
    {0xb5c, 0xb5d}, {0xb5f, 0xb61}, {0xb71, 0xb71}, {0xb83, 0xb83}, {0xb85, 0xb8a}, {0xb8e, 0xb90},
    {0xb92, 0xb95}, {0xb99, 0xb9a}, {0xb9c, 0xb9c}, {0xb9e, 0xb9f}, {0xba3, 0xba4}, {0xba8, 0xbaa},
    {0xbae, 0xbb9}, {0xbd0, 0xbd0}, {0xc05, 0xc0c}, {0xc0e, 0xc10}, {0xc12, 0xc28}, {0xc2a, 0xc39},
    {0xc3d, 0xc3d}, {0xc58, 0xc5a}, {0xc5d, 0xc5d}, {0xc60, 0xc61}, {0xc80, 0xc80}, {0xc85, 0xc8c},
    {0xc8e, 0xc90}, {0xc92, 0xca8}, {0xcaa, 0xcb3}, {0xcb5, 0xcb9}, {0xcbd, 0xcbd}, {0xcdd, 0xcde},
    {0xce0, 0xce1}, {0xcf1, 0xcf2}, {0xd04, 0xd0c}, {0xd0e, 0xd10}, {0xd12, 0xd3a}, {0xd3d, 0xd3d},
    {0xd4e, 0xd4e}, {0xd54, 0xd56}, {0xd5f, 0xd61}, {0xd7a, 0xd7f}, {0xd85, 0xd96}, {0xd9a, 0xdb1},
    {0xdb3, 0xdbb}, {0xdbd, 0xdbd}, {0xdc0, 0xdc6}, {0xe01, 0xe30}, {0xe32, 0xe33}, {0xe40, 0xe46},
    {0xe81, 0xe82}, {0xe84, 0xe84}, {0xe86, 0xe8a}, {0xe8c, 0xea3}, {0xea5, 0xea5}, {0xea7, 0xeb0},
    {0xeb2, 0xeb3}, {0xebd, 0xebd}, {0xec0, 0xec4}, {0xec6, 0xec6}, {0xedc, 0xedf}, {0xf00, 0xf00},
    {0xf40, 0xf47}, {0xf49, 0xf6c}, {0xf88, 0xf8c}, {0x1000, 0x102a}, {0x103f, 0x103f}, {0x1050, 0x1055},
    {0x105a, 0x105d}, {0x1061, 0x1061}, {0x1065, 0x1066}, {0x106e, 0x1070}, {0x1075, 0x1081}, {0x108e, 0x108e},
    {0x10a0, 0x10c5}, {0x10c7, 0x10c7}, {0x10cd, 0x10cd}, {0x10d0, 0x10fa}, {0x10fc, 0x1248}, {0x124a, 0x124d},
    {0x1250, 0x1256}, {0x1258, 0x1258}, {0x125a, 0x125d}, {0x1260, 0x1288}, {0x128a, 0x128d}, {0x1290, 0x12b0},
    {0x12b2, 0x12b5}, {0x12b8, 0x12be}, {0x12c0, 0x12c0}, {0x12c2, 0x12c5}, {0x12c8, 0x12d6}, {0x12d8, 0x1310},
    {0x1312, 0x1315}, {0x1318, 0x135a}, {0x1380, 0x138f}, {0x13a0, 0x13f5}, {0x13f8, 0x13fd}, {0x1401, 0x166c},
    {0x166f, 0x167f}, {0x1681, 0x169a}, {0x16a0, 0x16ea}, {0x16f1, 0x16f8}, {0x1700, 0x1711}, {0x171f, 0x1731},
    {0x1740, 0x1751}, {0x1760, 0x176c}, {0x176e, 0x1770}, {0x1780, 0x17b3}, {0x17d7, 0x17d7}, {0x17dc, 0x17dc},
    {0x1820, 0x1878}, {0x1880, 0x1884}, {0x1887, 0x18a8}, {0x18aa, 0x18aa}, {0x18b0, 0x18f5}, {0x1900, 0x191e},
    {0x1950, 0x196d}, {0x1970, 0x1974}, {0x1980, 0x19ab}, {0x19b0, 0x19c9}, {0x1a00, 0x1a16}, {0x1a20, 0x1a54},
    {0x1aa7, 0x1aa7}, {0x1b05, 0x1b33}, {0x1b45, 0x1b4c}, {0x1b83, 0x1ba0}, {0x1bae, 0x1baf}, {0x1bba, 0x1be5},
    {0x1c00, 0x1c23}, {0x1c4d, 0x1c4f}, {0x1c5a, 0x1c7d}, {0x1c80, 0x1c8a}, {0x1c90, 0x1cba}, {0x1cbd, 0x1cbf},
    {0x1ce9, 0x1cec}, {0x1cee, 0x1cf3}, {0x1cf5, 0x1cf6}, {0x1cfa, 0x1cfa}, {0x1d00, 0x1dbf}, {0x1e00, 0x1f15},
    {0x1f18, 0x1f1d}, {0x1f20, 0x1f45}, {0x1f48, 0x1f4d}, {0x1f50, 0x1f57}, {0x1f59, 0x1f59}, {0x1f5b, 0x1f5b},
    {0x1f5d, 0x1f5d}, {0x1f5f, 0x1f7d}, {0x1f80, 0x1fb4}, {0x1fb6, 0x1fbc}, {0x1fbe, 0x1fbe}, {0x1fc2, 0x1fc4},
    {0x1fc6, 0x1fcc}, {0x1fd0, 0x1fd3}, {0x1fd6, 0x1fdb}, {0x1fe0, 0x1fec}, {0x1ff2, 0x1ff4}, {0x1ff6, 0x1ffc},
    {0x2071, 0x2071}, {0x207f, 0x207f}, {0x2090, 0x209c}, {0x2102, 0x2102}, {0x2107, 0x2107}, {0x210a, 0x2113},
    {0x2115, 0x2115}, {0x2119, 0x211d}, {0x2124, 0x2124}, {0x2126, 0x2126}, {0x2128, 0x2128}, {0x212a, 0x212d},
    {0x212f, 0x2139}, {0x213c, 0x213f}, {0x2145, 0x2149}, {0x214e, 0x214e}, {0x2183, 0x2184}, {0x2c00, 0x2ce4},
    {0x2ceb, 0x2cee}, {0x2cf2, 0x2cf3}, {0x2d00, 0x2d25}, {0x2d27, 0x2d27}, {0x2d2d, 0x2d2d}, {0x2d30, 0x2d67},
    {0x2d6f, 0x2d6f}, {0x2d80, 0x2d96}, {0x2da0, 0x2da6}, {0x2da8, 0x2dae}, {0x2db0, 0x2db6}, {0x2db8, 0x2dbe},
    {0x2dc0, 0x2dc6}, {0x2dc8, 0x2dce}, {0x2dd0, 0x2dd6}, {0x2dd8, 0x2dde}, {0x2e2f, 0x2e2f}, {0x3005, 0x3006},
    {0x3031, 0x3035}, {0x303b, 0x303c}, {0x3041, 0x3096}, {0x309d, 0x309f}, {0x30a1, 0x30fa}, {0x30fc, 0x30ff},
    {0x3105, 0x312f}, {0x3131, 0x318e}, {0x31a0, 0x31bf}, {0x31f0, 0x31ff}, {0x3400, 0x4dbf}, {0x4e00, 0xa48c},
    {0xa4d0, 0xa4fd}, {0xa500, 0xa60c}, {0xa610, 0xa61f}, {0xa62a, 0xa62b}, {0xa640, 0xa66e}, {0xa67f, 0xa69d},
    {0xa6a0, 0xa6e5}, {0xa717, 0xa71f}, {0xa722, 0xa788}, {0xa78b, 0xa7cd}, {0xa7d0, 0xa7d1}, {0xa7d3, 0xa7d3},
    {0xa7d5, 0xa7dc}, {0xa7f2, 0xa801}, {0xa803, 0xa805}, {0xa807, 0xa80a}, {0xa80c, 0xa822}, {0xa840, 0xa873},
    {0xa882, 0xa8b3}, {0xa8f2, 0xa8f7}, {0xa8fb, 0xa8fb}, {0xa8fd, 0xa8fe}, {0xa90a, 0xa925}, {0xa930, 0xa946},
    {0xa960, 0xa97c}, {0xa984, 0xa9b2}, {0xa9cf, 0xa9cf}, {0xa9e0, 0xa9e4}, {0xa9e6, 0xa9ef}, {0xa9fa, 0xa9fe},
    {0xaa00, 0xaa28}, {0xaa40, 0xaa42}, {0xaa44, 0xaa4b}, {0xaa60, 0xaa76}, {0xaa7a, 0xaa7a}, {0xaa7e, 0xaaaf},
    {0xaab1, 0xaab1}, {0xaab5, 0xaab6}, {0xaab9, 0xaabd}, {0xaac0, 0xaac0}, {0xaac2, 0xaac2}, {0xaadb, 0xaadd},
    {0xaae0, 0xaaea}, {0xaaf2, 0xaaf4}, {0xab01, 0xab06}, {0xab09, 0xab0e}, {0xab11, 0xab16}, {0xab20, 0xab26},
    {0xab28, 0xab2e}, {0xab30, 0xab5a}, {0xab5c, 0xab69}, {0xab70, 0xabe2}, {0xac00, 0xd7a3}, {0xd7b0, 0xd7c6},
    {0xd7cb, 0xd7fb}, {0xf900, 0xfa6d}, {0xfa70, 0xfad9}, {0xfb00, 0xfb06}, {0xfb13, 0xfb17}, {0xfb1d, 0xfb1d},
    {0xfb1f, 0xfb28}, {0xfb2a, 0xfb36}, {0xfb38, 0xfb3c}, {0xfb3e, 0xfb3e}, {0xfb40, 0xfb41}, {0xfb43, 0xfb44},
    {0xfb46, 0xfbb1}, {0xfbd3, 0xfd3d}, {0xfd50, 0xfd8f}, {0xfd92, 0xfdc7}, {0xfdf0, 0xfdfb}, {0xfe70, 0xfe74},
    {0xfe76, 0xfefc}, {0xff21, 0xff3a}, {0xff41, 0xff5a}, {0xff66, 0xffbe}, {0xffc2, 0xffc7}, {0xffca, 0xffcf},
    {0xffd2, 0xffd7}, {0xffda, 0xffdc}, {0x10000, 0x1000b}, {0x1000d, 0x10026}, {0x10028, 0x1003a}, {0x1003c, 0x1003d},
    {0x1003f, 0x1004d}, {0x10050, 0x1005d}, {0x10080, 0x100fa}, {0x10280, 0x1029c}, {0x102a0, 0x102d0}, {0x10300, 0x1031f},
    {0x1032d, 0x10340}, {0x10342, 0x10349}, {0x10350, 0x10375}, {0x10380, 0x1039d}, {0x103a0, 0x103c3}, {0x103c8, 0x103cf},
    {0x10400, 0x1049d}, {0x104b0, 0x104d3}, {0x104d8, 0x104fb}, {0x10500, 0x10527}, {0x10530, 0x10563}, {0x10570, 0x1057a},
    {0x1057c, 0x1058a}, {0x1058c, 0x10592}, {0x10594, 0x10595}, {0x10597, 0x105a1}, {0x105a3, 0x105b1}, {0x105b3, 0x105b9},
    {0x105bb, 0x105bc}, {0x105c0, 0x105f3}, {0x10600, 0x10736}, {0x10740, 0x10755}, {0x10760, 0x10767}, {0x10780, 0x10785},
    {0x10787, 0x107b0}, {0x107b2, 0x107ba}, {0x10800, 0x10805}, {0x10808, 0x10808}, {0x1080a, 0x10835}, {0x10837, 0x10838},
    {0x1083c, 0x1083c}, {0x1083f, 0x10855}, {0x10860, 0x10876}, {0x10880, 0x1089e}, {0x108e0, 0x108f2}, {0x108f4, 0x108f5},
    {0x10900, 0x10915}, {0x10920, 0x10939}, {0x10980, 0x109b7}, {0x109be, 0x109bf}, {0x10a00, 0x10a00}, {0x10a10, 0x10a13},
    {0x10a15, 0x10a17}, {0x10a19, 0x10a35}, {0x10a60, 0x10a7c}, {0x10a80, 0x10a9c}, {0x10ac0, 0x10ac7}, {0x10ac9, 0x10ae4},
    {0x10b00, 0x10b35}, {0x10b40, 0x10b55}, {0x10b60, 0x10b72}, {0x10b80, 0x10b91}, {0x10c00, 0x10c48}, {0x10c80, 0x10cb2},
    {0x10cc0, 0x10cf2}, {0x10d00, 0x10d23}, {0x10d4a, 0x10d65}, {0x10d6f, 0x10d85}, {0x10e80, 0x10ea9}, {0x10eb0, 0x10eb1},
    {0x10ec2, 0x10ec4}, {0x10f00, 0x10f1c}, {0x10f27, 0x10f27}, {0x10f30, 0x10f45}, {0x10f70, 0x10f81}, {0x10fb0, 0x10fc4},
    {0x10fe0, 0x10ff6}, {0x11003, 0x11037}, {0x11071, 0x11072}, {0x11075, 0x11075}, {0x11083, 0x110af}, {0x110d0, 0x110e8},
    {0x11103, 0x11126}, {0x11144, 0x11144}, {0x11147, 0x11147}, {0x11150, 0x11172}, {0x11176, 0x11176}, {0x11183, 0x111b2},
    {0x111c1, 0x111c4}, {0x111da, 0x111da}, {0x111dc, 0x111dc}, {0x11200, 0x11211}, {0x11213, 0x1122b}, {0x1123f, 0x11240},
    {0x11280, 0x11286}, {0x11288, 0x11288}, {0x1128a, 0x1128d}, {0x1128f, 0x1129d}, {0x1129f, 0x112a8}, {0x112b0, 0x112de},
    {0x11305, 0x1130c}, {0x1130f, 0x11310}, {0x11313, 0x11328}, {0x1132a, 0x11330}, {0x11332, 0x11333}, {0x11335, 0x11339},
    {0x1133d, 0x1133d}, {0x11350, 0x11350}, {0x1135d, 0x11361}, {0x11380, 0x11389}, {0x1138b, 0x1138b}, {0x1138e, 0x1138e},
    {0x11390, 0x113b5}, {0x113b7, 0x113b7}, {0x113d1, 0x113d1}, {0x113d3, 0x113d3}, {0x11400, 0x11434}, {0x11447, 0x1144a},
    {0x1145f, 0x11461}, {0x11480, 0x114af}, {0x114c4, 0x114c5}, {0x114c7, 0x114c7}, {0x11580, 0x115ae}, {0x115d8, 0x115db},
    {0x11600, 0x1162f}, {0x11644, 0x11644}, {0x11680, 0x116aa}, {0x116b8, 0x116b8}, {0x11700, 0x1171a}, {0x11740, 0x11746},
    {0x11800, 0x1182b}, {0x118a0, 0x118df}, {0x118ff, 0x11906}, {0x11909, 0x11909}, {0x1190c, 0x11913}, {0x11915, 0x11916},
    {0x11918, 0x1192f}, {0x1193f, 0x1193f}, {0x11941, 0x11941}, {0x119a0, 0x119a7}, {0x119aa, 0x119d0}, {0x119e1, 0x119e1},
    {0x119e3, 0x119e3}, {0x11a00, 0x11a00}, {0x11a0b, 0x11a32}, {0x11a3a, 0x11a3a}, {0x11a50, 0x11a50}, {0x11a5c, 0x11a89},
    {0x11a9d, 0x11a9d}, {0x11ab0, 0x11af8}, {0x11bc0, 0x11be0}, {0x11c00, 0x11c08}, {0x11c0a, 0x11c2e}, {0x11c40, 0x11c40},
    {0x11c72, 0x11c8f}, {0x11d00, 0x11d06}, {0x11d08, 0x11d09}, {0x11d0b, 0x11d30}, {0x11d46, 0x11d46}, {0x11d60, 0x11d65},
    {0x11d67, 0x11d68}, {0x11d6a, 0x11d89}, {0x11d98, 0x11d98}, {0x11ee0, 0x11ef2}, {0x11f02, 0x11f02}, {0x11f04, 0x11f10},
    {0x11f12, 0x11f33}, {0x11fb0, 0x11fb0}, {0x12000, 0x12399}, {0x12480, 0x12543}, {0x12f90, 0x12ff0}, {0x13000, 0x1342f},
    {0x13441, 0x13446}, {0x13460, 0x143fa}, {0x14400, 0x14646}, {0x16100, 0x1611d}, {0x16800, 0x16a38}, {0x16a40, 0x16a5e},
    {0x16a70, 0x16abe}, {0x16ad0, 0x16aed}, {0x16b00, 0x16b2f}, {0x16b40, 0x16b43}, {0x16b63, 0x16b77}, {0x16b7d, 0x16b8f},
    {0x16d40, 0x16d6c}, {0x16e40, 0x16e7f}, {0x16f00, 0x16f4a}, {0x16f50, 0x16f50}, {0x16f93, 0x16f9f}, {0x16fe0, 0x16fe1},
    {0x16fe3, 0x16fe3}, {0x17000, 0x187f7}, {0x18800, 0x18cd5}, {0x18cff, 0x18d08}, {0x1aff0, 0x1aff3}, {0x1aff5, 0x1affb},
    {0x1affd, 0x1affe}, {0x1b000, 0x1b122}, {0x1b132, 0x1b132}, {0x1b150, 0x1b152}, {0x1b155, 0x1b155}, {0x1b164, 0x1b167},
    {0x1b170, 0x1b2fb}, {0x1bc00, 0x1bc6a}, {0x1bc70, 0x1bc7c}, {0x1bc80, 0x1bc88}, {0x1bc90, 0x1bc99}, {0x1d400, 0x1d454},
    {0x1d456, 0x1d49c}, {0x1d49e, 0x1d49f}, {0x1d4a2, 0x1d4a2}, {0x1d4a5, 0x1d4a6}, {0x1d4a9, 0x1d4ac}, {0x1d4ae, 0x1d4b9},
    {0x1d4bb, 0x1d4bb}, {0x1d4bd, 0x1d4c3}, {0x1d4c5, 0x1d505}, {0x1d507, 0x1d50a}, {0x1d50d, 0x1d514}, {0x1d516, 0x1d51c},
    {0x1d51e, 0x1d539}, {0x1d53b, 0x1d53e}, {0x1d540, 0x1d544}, {0x1d546, 0x1d546}, {0x1d54a, 0x1d550}, {0x1d552, 0x1d6a5},
    {0x1d6a8, 0x1d6c0}, {0x1d6c2, 0x1d6da}, {0x1d6dc, 0x1d6fa}, {0x1d6fc, 0x1d714}, {0x1d716, 0x1d734}, {0x1d736, 0x1d74e},
    {0x1d750, 0x1d76e}, {0x1d770, 0x1d788}, {0x1d78a, 0x1d7a8}, {0x1d7aa, 0x1d7c2}, {0x1d7c4, 0x1d7cb}, {0x1df00, 0x1df1e},
    {0x1df25, 0x1df2a}, {0x1e030, 0x1e06d}, {0x1e100, 0x1e12c}, {0x1e137, 0x1e13d}, {0x1e14e, 0x1e14e}, {0x1e290, 0x1e2ad},
    {0x1e2c0, 0x1e2eb}, {0x1e4d0, 0x1e4eb}, {0x1e5d0, 0x1e5ed}, {0x1e5f0, 0x1e5f0}, {0x1e7e0, 0x1e7e6}, {0x1e7e8, 0x1e7eb},
    {0x1e7ed, 0x1e7ee}, {0x1e7f0, 0x1e7fe}, {0x1e800, 0x1e8c4}, {0x1e900, 0x1e943}, {0x1e94b, 0x1e94b}, {0x1ee00, 0x1ee03},
    {0x1ee05, 0x1ee1f}, {0x1ee21, 0x1ee22}, {0x1ee24, 0x1ee24}, {0x1ee27, 0x1ee27}, {0x1ee29, 0x1ee32}, {0x1ee34, 0x1ee37},
    {0x1ee39, 0x1ee39}, {0x1ee3b, 0x1ee3b}, {0x1ee42, 0x1ee42}, {0x1ee47, 0x1ee47}, {0x1ee49, 0x1ee49}, {0x1ee4b, 0x1ee4b},
    {0x1ee4d, 0x1ee4f}, {0x1ee51, 0x1ee52}, {0x1ee54, 0x1ee54}, {0x1ee57, 0x1ee57}, {0x1ee59, 0x1ee59}, {0x1ee5b, 0x1ee5b},
    {0x1ee5d, 0x1ee5d}, {0x1ee5f, 0x1ee5f}, {0x1ee61, 0x1ee62}, {0x1ee64, 0x1ee64}, {0x1ee67, 0x1ee6a}, {0x1ee6c, 0x1ee72},
    {0x1ee74, 0x1ee77}, {0x1ee79, 0x1ee7c}, {0x1ee7e, 0x1ee7e}, {0x1ee80, 0x1ee89}, {0x1ee8b, 0x1ee9b}, {0x1eea1, 0x1eea3},
    {0x1eea5, 0x1eea9}, {0x1eeab, 0x1eebb}, {0x20000, 0x2a6df}, {0x2a700, 0x2b739}, {0x2b740, 0x2b81d}, {0x2b820, 0x2cea1},
    {0x2ceb0, 0x2ebe0}, {0x2ebf0, 0x2ee5d}, {0x2f800, 0x2fa1d}, {0x30000, 0x3134a}, {0x31350, 0x323af},
};
static const uint32_t br_space_ranges[][2] = {
    {0x9, 0xd}, {0x1c, 0x20}, {0x85, 0x85}, {0xa0, 0xa0}, {0x1680, 0x1680}, {0x2000, 0x200a},
    {0x2028, 0x2029}, {0x202f, 0x202f}, {0x205f, 0x205f}, {0x3000, 0x3000},
};
#define BR_COUNT(t) (sizeof(t) / sizeof((t)[0]))
/* stands for a byte that isn't valid UTF-8: no case, no class */
#define BR_NOT_CP 0xffffffffu

/* the code point of the n-byte character at p (n from br_utf8_len) */
static uint32_t br_utf8_cp(const unsigned char *p, int64_t n) {
    if (n == 1) return p[0] < 0x80 ? p[0] : BR_NOT_CP;
    uint32_t c = p[0] & (0x7f >> n);
    for (int64_t i = 1; i < n; i++) c = (c << 6) | (p[i] & 0x3f);
    return c;
}
static void br_sb_cp(brine_sb *sb, uint32_t c) {
    char b[4];
    int n;
    if (c < 0x80) {
        b[0] = (char)c;
        n = 1;
    } else if (c < 0x800) {
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
    brine_sb_cat(sb, b, n);
}
/* c mapped by the run that covers it (c itself if none does) */
static uint32_t br_case_map(const br_case_run *t, size_t n, uint32_t c) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t m = (lo + hi) / 2;
        if (t[m].lo <= c) lo = m + 1;
        else hi = m;
    }
    if (lo == 0) return c;
    const br_case_run *r = &t[lo - 1];
    if (c > r->hi || (c - r->lo) % r->step) return c;
    return (uint32_t)((int32_t)c + r->delta);
}
static const char *br_case_many(const br_case_special *t, size_t n, uint32_t c) {
    for (size_t i = 0; i < n; i++)
        if (t[i].cp == c) return t[i].to;
    return NULL;
}
static bool br_in_ranges(const uint32_t (*t)[2], size_t n, uint32_t c) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t m = (lo + hi) / 2;
        if (t[m][0] <= c) lo = m + 1;
        else hi = m;
    }
    return lo > 0 && c <= t[lo - 1][1];
}
static bool br_alpha(uint32_t c) {
    if (c < 0x80) return (c | 32) >= 'a' && (c | 32) <= 'z';
    return c != BR_NOT_CP && br_in_ranges(br_alpha_ranges, BR_COUNT(br_alpha_ranges), c);
}
static bool br_space(uint32_t c) {
    if (c < 0x80) return c == ' ' || (c >= '\t' && c <= '\r') || (c >= 0x1c && c <= 0x1f);
    return c != BR_NOT_CP && br_in_ranges(br_space_ranges, BR_COUNT(br_space_ranges), c);
}
/* the character at byte i of s: its length, and its code point in *c */
static int64_t br_char_at(const brine_str *s, int64_t i, uint32_t *c) {
    const unsigned char *p = (const unsigned char *)s->data + i;
    int64_t n = br_utf8_len(p, s->len - i);
    *c = br_utf8_cp(p, n);
    return n;
}
/* the length of the whitespace character ending at byte hi of s, or 0 */
static int64_t br_space_before(const brine_str *s, int64_t hi) {
    int64_t j = hi - 1;
    while (j > 0 && hi - j < 4 && ((unsigned char)s->data[j] & 0xc0) == 0x80) j--;
    uint32_t c;
    int64_t n = br_char_at(s, j, &c);
    if (j + n != hi) n = br_char_at(s, hi - 1, &c);
    return br_space(c) ? n : 0;
}
static brine_str *br_case(brine_str *s, bool up) {
    const br_case_run *runs = up ? br_upper_runs : br_lower_runs;
    size_t nruns = up ? BR_COUNT(br_upper_runs) : BR_COUNT(br_lower_runs);
    brine_sb sb;
    brine_sb_init(&sb);
    bool prev_alpha = false;
    for (int64_t i = 0; i < s->len;) {
        uint32_t c;
        int64_t n = br_char_at(s, i, &c);
        const char *many = up ? br_case_many(br_upper_special, BR_COUNT(br_upper_special), c)
                              : br_case_many(br_lower_special, BR_COUNT(br_lower_special), c);
        if (many) {
            brine_sb_cat(&sb, many, (int64_t)strlen(many));
        } else {
            uint32_t m = c == BR_NOT_CP ? c : br_case_map(runs, nruns, c);
            /* a capital sigma that ends a word becomes the final form, as in Python */
            if (!up && c == 0x3a3 && prev_alpha) {
                uint32_t next = BR_NOT_CP;
                if (i + n < s->len) br_char_at(s, i + n, &next);
                if (!br_alpha(next)) m = 0x3c2;
            }
            if (m == c) brine_sb_cat(&sb, s->data + i, n);
            else br_sb_cp(&sb, m);
        }
        prev_alpha = br_alpha(c);
        i += n;
    }
    return brine_sb_finish(&sb);
}
brine_arr *brine_s_split_ws(brine_str *s, const char *loc) {
    (void)loc;
    brine_arr *out = br_strs();
    int64_t i = 0;
    uint32_t c;
    while (i < s->len) {
        int64_t n = br_char_at(s, i, &c);
        if (br_space(c)) {
            i += n;
            continue;
        }
        int64_t j = i;
        while (j < s->len && (n = br_char_at(s, j, &c), !br_space(c))) j += n;
        br_push_str(&out, br_sub(s, i, j));
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
    int64_t lo = 0, hi = s->len, n;
    uint32_t c;
    while (left && lo < hi && (n = br_char_at(s, lo, &c), br_space(c))) lo += n;
    while (right && hi > lo && (n = br_space_before(s, hi)) > 0) hi -= n;
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
/* true for a non-empty string whose every character passes `test` (on c);
   digits are 0-9, the ones int() reads; letters and spaces are Unicode */
#define BR_CLASS(name, test)                                   \
    bool brine_s_##name(brine_str *s, const char *loc) {       \
        (void)loc;                                             \
        if (s->len == 0) return false;                         \
        for (int64_t i = 0; i < s->len;) {                     \
            uint32_t c;                                        \
            i += br_char_at(s, i, &c);                         \
            if (!(test)) return false;                         \
        }                                                      \
        return true;                                           \
    }
BR_CLASS(is_digit, c >= '0' && c <= '9')
BR_CLASS(is_alpha, br_alpha(c))
BR_CLASS(is_space, br_space(c))
BR_CLASS(is_alnum, br_alpha(c) || (c >= '0' && c <= '9'))
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
/* write a finished builder (and free it); `print` adds its own `end` */
void brine_out_sb(brine_sb *sb) {
    br_lock(&br_io_mu);
    fwrite(sb->p, 1, (size_t)sb->len, stdout);
    br_unlock(&br_io_mu);
    free(sb->p);
    sb->p = NULL;
}
void brine_err_sb(brine_sb *sb) {
    br_lock(&br_io_mu);
    fflush(stdout);
    fwrite(sb->p, 1, (size_t)sb->len, stderr);
    br_unlock(&br_io_mu);
    free(sb->p);
    sb->p = NULL;
}
void brine_print_sb(brine_sb *sb) {
    brine_sb_cat(sb, "\n", 1);
    brine_out_sb(sb);
}
void brine_eprint_sb(brine_sb *sb) {
    brine_sb_cat(sb, "\n", 1);
    brine_err_sb(sb);
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
    uint64_t id;
} br_job;

/* The running job is published in `cur` (and its number in `ids`); a worker
   counts itself in `active` before it reads `cur`, and the thread that posted
   the job clears `cur` and waits for `active` to drain before the job (on its
   stack) goes away. Joining and leaving take no lock; `mu` is only for
   sleeping. */
static struct {
    br_mutex mu;
    br_cond cv_work, cv_done;
    br_mutex job_lock;
    int nthreads;
    int started;
    br_job *cur;
    uint64_t ids;
    int active;
    int main_waits;
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

/* Waking a sleeping thread takes tens of microseconds, longer than many
   parallel regions run, so threads busy-wait a little before they sleep. */
#if defined(__x86_64__) || defined(__i386__)
#define BR_RELAX() __builtin_ia32_pause()
#elif defined(__aarch64__)
#define BR_RELAX() __asm__ __volatile__("yield")
#else
#define BR_RELAX() ((void)0)
#endif
#define BR_SPINS 20000
static void br_spin_while_eq(uint64_t *p, uint64_t v) {
    for (int i = 0; i < BR_SPINS && __atomic_load_n(p, __ATOMIC_ACQUIRE) == v; i++) BR_RELAX();
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
    uint64_t seen = 0;
    for (;;) {
        br_spin_while_eq(&br_pool.ids, seen);
        if (__atomic_load_n(&br_pool.ids, __ATOMIC_SEQ_CST) == seen) {
            br_lock(&br_pool.mu);
            while (__atomic_load_n(&br_pool.ids, __ATOMIC_SEQ_CST) == seen) br_cond_wait(&br_pool.cv_work, &br_pool.mu);
            br_unlock(&br_pool.mu);
        }
        seen = __atomic_load_n(&br_pool.ids, __ATOMIC_SEQ_CST);
        __atomic_add_fetch(&br_pool.active, 1, __ATOMIC_SEQ_CST);
        br_job *j = __atomic_load_n(&br_pool.cur, __ATOMIC_SEQ_CST);
        if (j) br_run_chunks(j);
        if (__atomic_sub_fetch(&br_pool.active, 1, __ATOMIC_SEQ_CST) == 0 &&
            __atomic_load_n(&br_pool.main_waits, __ATOMIC_SEQ_CST)) {
            br_lock(&br_pool.mu);
            br_cond_broadcast(&br_pool.cv_done);
            br_unlock(&br_pool.mu);
        }
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
    job.id = br_pool.ids + 1;
    __atomic_store_n(&br_pool.cur, &job, __ATOMIC_SEQ_CST);
    __atomic_store_n(&br_pool.ids, job.id, __ATOMIC_SEQ_CST);
    br_cond_broadcast(&br_pool.cv_work);
    br_unlock(&br_pool.mu);
    br_in_par = 1;
    br_run_chunks(&job);
    br_in_par = 0;
    __atomic_store_n(&br_pool.cur, (br_job *)NULL, __ATOMIC_SEQ_CST);
    for (int i = 0; i < BR_SPINS && __atomic_load_n(&br_pool.active, __ATOMIC_SEQ_CST) > 0; i++) BR_RELAX();
    if (__atomic_load_n(&br_pool.active, __ATOMIC_SEQ_CST) > 0) {
        br_lock(&br_pool.mu);
        __atomic_store_n(&br_pool.main_waits, 1, __ATOMIC_SEQ_CST);
        while (__atomic_load_n(&br_pool.active, __ATOMIC_SEQ_CST) > 0) br_cond_wait(&br_pool.cv_done, &br_pool.mu);
        __atomic_store_n(&br_pool.main_waits, 0, __ATOMIC_SEQ_CST);
        br_unlock(&br_pool.mu);
    }
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

/* ============================================================ reductions */
/* `par sum(...)` and the other reductions combine in one fixed order that
 * depends only on the element count (see brine_rt.h). A fold holds the nodes
 * of complete aligned subtrees, highest first, like the digits of a binary
 * counter: a new node merges with the top while their heights match. Pushing
 * aligned subtrees in order and then combining what's left from the top down
 * gives exactly the level-by-level pairing tree. */
typedef void (*br_comb_fn)(void *a, const void *b, const char *loc);

#define BR_COMB_INT(T, S)                                                         \
    static void br_sum_##S(void *a, const void *b, const char *loc) {             \
        *(T *)a = brine_add_##S(*(T *)a, *(const T *)b, loc);                     \
    }                                                                             \
    static void br_prod_##S(void *a, const void *b, const char *loc) {            \
        *(T *)a = brine_mul_##S(*(T *)a, *(const T *)b, loc);                     \
    }
#define BR_COMB_FLOAT(T, S)                                                       \
    static void br_sum_##S(void *a, const void *b, const char *loc) {             \
        (void)loc;                                                                \
        *(T *)a = *(T *)a + *(const T *)b;                                        \
    }                                                                             \
    static void br_prod_##S(void *a, const void *b, const char *loc) {            \
        (void)loc;                                                                \
        *(T *)a = *(T *)a * *(const T *)b;                                        \
    }
/* like the sequential `min`/`max`: the right value replaces the left one only
 * if it's strictly smaller (larger), so NaNs and ties keep the left one */
#define BR_COMB_ORD(T, S)                                                         \
    static void br_min_##S(void *a, const void *b, const char *loc) {             \
        (void)loc;                                                                \
        if (*(const T *)b < *(T *)a) *(T *)a = *(const T *)b;                     \
    }                                                                             \
    static void br_max_##S(void *a, const void *b, const char *loc) {             \
        (void)loc;                                                                \
        if (*(const T *)b > *(T *)a) *(T *)a = *(const T *)b;                     \
    }
#define BR_COMB_ALL(T, S) BR_COMB_INT(T, S) BR_COMB_ORD(T, S)
BR_COMB_ALL(int8_t, i8)
BR_COMB_ALL(int16_t, i16)
BR_COMB_ALL(int32_t, i32)
BR_COMB_ALL(int64_t, i64)
BR_COMB_ALL(brine_i128, i128)
BR_COMB_ALL(uint8_t, u8)
BR_COMB_ALL(uint16_t, u16)
BR_COMB_ALL(uint32_t, u32)
BR_COMB_ALL(uint64_t, u64)
BR_COMB_ALL(brine_u128, u128)
BR_COMB_FLOAT(float, f32)
BR_COMB_FLOAT(double, f64)
BR_COMB_ORD(float, f32)
BR_COMB_ORD(double, f64)
static void br_any(void *a, const void *b, const char *loc) {
    (void)loc;
    *(bool *)a = *(bool *)a || *(const bool *)b;
}
static void br_all(void *a, const void *b, const char *loc) {
    (void)loc;
    *(bool *)a = *(bool *)a && *(const bool *)b;
}

static const int br_vk_size[] = {1, 2, 4, 8, 16, 1, 2, 4, 8, 16, 4, 8, 8, 1};

static br_comb_fn br_comb_pick(int64_t op, int64_t vk) {
#define BR_PICK(S)                                                                \
    switch (op) {                                                                 \
    case BRINE_RED_SUM: return br_sum_##S;                                        \
    case BRINE_RED_PRODUCT: return br_prod_##S;                                   \
    case BRINE_RED_MIN: return br_min_##S;                                        \
    default: return br_max_##S;                                                   \
    }
    if (op == BRINE_RED_ANY) return br_any;
    if (op == BRINE_RED_ALL) return br_all;
    switch (vk) {
    case BRINE_VK_I8: BR_PICK(i8)
    case BRINE_VK_I16: BR_PICK(i16)
    case BRINE_VK_I32: BR_PICK(i32)
    case BRINE_VK_I64: BR_PICK(i64)
    case BRINE_VK_I128: BR_PICK(i128)
    case BRINE_VK_U8: BR_PICK(u8)
    case BRINE_VK_U16: BR_PICK(u16)
    case BRINE_VK_U32: BR_PICK(u32)
    case BRINE_VK_U64: BR_PICK(u64)
    case BRINE_VK_U128: BR_PICK(u128)
    case BRINE_VK_F32: BR_PICK(f32)
    default: BR_PICK(f64)
    }
#undef BR_PICK
}

typedef struct {
    br_comb_fn comb;
    size_t size;
    const char *loc;
    int top;
    int height[72];
    bool has[72];
    _Alignas(16) char val[72][16];
} br_fold;

static void br_fold_init(br_fold *f, br_comb_fn comb, int64_t size, const char *loc) {
    f->comb = comb;
    f->size = (size_t)size;
    f->loc = loc;
    f->top = 0;
}

/* a <- a (+) b, where a node without a value (all filtered out) is the identity */
static void br_node_comb(br_fold *f, void *a, bool *ha, const void *b, bool hb) {
    if (!hb) return;
    if (!*ha) {
        memcpy(a, b, f->size);
        *ha = true;
        return;
    }
    f->comb(a, b, f->loc);
}

/* Push the node of an aligned subtree of 2^h leaves that starts where the
 * nodes pushed so far end. */
static void br_fold_push(br_fold *f, int h, const void *v, bool has) {
    while (f->top > 0 && f->height[f->top - 1] == h) {
        int t = f->top - 1;
        br_node_comb(f, f->val[t], &f->has[t], v, has);
        f->top = t;
        v = f->val[t];
        has = f->has[t];
        h++;
    }
    int t = f->top++;
    f->height[t] = h;
    f->has[t] = has;
    if (v != f->val[t]) memcpy(f->val[t], v, f->size);
}

/* The root: the nodes left, combined from the top down. False if no element
 * had a value. */
static bool br_fold_finish(br_fold *f, void *out) {
    if (f->top == 0) return false;
    int t = f->top - 1;
    while (t > 0) {
        br_node_comb(f, f->val[t - 1], &f->has[t - 1], f->val[t], f->has[t]);
        t--;
    }
    if (f->has[0]) memcpy(out, f->val[0], f->size);
    return f->has[0];
}

typedef struct {
    brine_leaf_fn fn;
    void *ctx;
    int64_t n, nleaves, task_leaves;
    br_comb_fn comb;
    int64_t size;
    const char *loc;
    char *vals; /* per task: the node of its subtree */
    bool *has;
} br_reduce_job;

static void br_reduce_task(void *ctx, int64_t chunk, int64_t lo, int64_t hi) {
    (void)chunk;
    br_reduce_job *j = (br_reduce_job *)ctx;
    _Alignas(16) char v[16];
    for (int64_t t = lo; t < hi; t++) {
        br_fold f;
        br_fold_init(&f, j->comb, j->size, j->loc);
        int64_t l0 = t * j->task_leaves;
        int64_t l1 = l0 + j->task_leaves < j->nleaves ? l0 + j->task_leaves : j->nleaves;
        for (int64_t l = l0; l < l1; l++) {
            int64_t a = l * BRINE_LEAF;
            int64_t b = a + BRINE_LEAF < j->n ? a + BRINE_LEAF : j->n;
            bool h = false;
            j->fn(j->ctx, l, a, b, v, &h);
            br_fold_push(&f, 0, v, h);
        }
        j->has[t] = br_fold_finish(&f, j->vals + t * 16);
    }
}

static void br_reduce_cpu(int64_t n, brine_leaf_fn fn, void *ctx, int64_t op, int64_t vk, void *val, bool *has, const char *loc) {
    if (n <= 0) return;
    br_reduce_job j;
    j.fn = fn;
    j.ctx = ctx;
    j.n = n;
    j.nleaves = (n + BRINE_LEAF - 1) / BRINE_LEAF;
    j.comb = br_comb_pick(op, vk);
    j.size = br_vk_size[vk];
    j.loc = loc;
    /* tasks of 2^h leaves, several per thread for balance; the result doesn't
     * depend on the split, since every task is a whole subtree */
    int64_t want = (int64_t)br_threads() * 8;
    int h = 0;
    while (h < 62 && (((int64_t)1 << h) * want) < j.nleaves) h++;
    j.task_leaves = (int64_t)1 << h;
    int64_t ntasks = (j.nleaves + j.task_leaves - 1) / j.task_leaves;
    j.vals = (char *)brine_alloc(ntasks * 16);
    j.has = (bool *)brine_alloc(ntasks);
    brine_par_run(0, ntasks, ntasks, br_reduce_task, &j);
    br_fold f;
    br_fold_init(&f, j.comb, j.size, loc);
    for (int64_t t = 0; t < ntasks; t++) br_fold_push(&f, h, j.vals + t * 16, j.has[t]);
    if (br_fold_finish(&f, val)) *has = true;
    free(j.vals);
    free(j.has);
}

/* ============================================================ GPU */
/* `par` constructs whose bodies only compute (ptx.br decides, at compile
 * time) can run on a GPU: NVIDIA's through CUDA, Apple's through Metal. The
 * driver or framework is loaded when first needed: there's no toolkit or
 * link-time dependency, and without a GPU everything runs on the thread pool.
 *
 * Results are the same either way: kernels compute exactly what the CPU code
 * does (ptx.br, msl.br), reductions combine in the same tree, and a failing check on
 * the device (overflow, an index, ...) discards the device's work and runs
 * the construct on the CPU, which reports the error as usual.
 *
 * BRINE_GPU=off|on|auto (default auto): auto uses the GPU when the estimate
 * says it's faster (the CPU's speed measured on the first elements, the
 * GPU's after its first run, the copies at the measured bandwidth); on uses
 * it for every construct it can run. BRINE_GPU_LOG=1 prints where each
 * construct ran, and how long it took, to stderr. */

typedef int br_gpu_res;
typedef unsigned long long br_gpu_ptr;

enum { BR_GPU_OOM = 2 };
enum { BR_GPU_OFF, BR_GPU_AUTO, BR_GPU_ON };
enum { BR_GS_NONE, BR_GS_INIT, BR_GS_READY, BR_GS_FAILED };

/* threads per block: the combine kernels' fan-in (BLOCK in ptx.br) */
#define BR_GPU_BLOCK 256
/* smallest construct auto considers (elements) */
#define BR_GPU_MIN_N 4096
/* a launch should take about this long (seconds): Windows resets a GPU
 * whose kernel runs for 2 s */
#define BR_GPU_SLICE 0.03
/* how much one hardware thread adds to the pool's speed: most CPUs run two
 * threads per core */
#define BR_CPU_EFF 0.5

static struct {
    br_mutex mu; /* the fields below */
    br_cond cv;
    br_mutex run; /* held while the device is in use */
    int mode, log, state;
    char why[256];
    const char *ptx;
    br_gpu_ptr err; /* the error flag kernels set */
    br_gpu_ptr buf;
    size_t cap;
    char *stage;
    size_t stage_cap;
    int stage_pinned;
    double bw;     /* bytes per second, host <-> device */
    double init_s; /* how long getting the device ready takes */
    char name[128];
    int sms;
    double speed;     /* how much faster than one CPU thread at f32 and integer code */
    double f64_ratio; /* the same at f64 code */
} br_gpu;

/* what the runtime learned about one construct */
typedef struct {
    void *fn, *comb;
    double seq_ns; /* one CPU thread, per element */
    double cpu_ns; /* the thread pool, per element */
    double gpu_ns; /* the device, per element (0: not measured) */
    int cpu_seen;  /* cpu_ns was measured, not estimated */
} br_kst;

static void br_gpu_setup(void) {
    br_mutex_init(&br_gpu.mu);
    br_mutex_init(&br_gpu.run);
    br_cond_init(&br_gpu.cv);
    const char *m = getenv("BRINE_GPU");
    br_gpu.mode = BR_GPU_AUTO;
    if (m && (!strcmp(m, "off") || !strcmp(m, "0"))) br_gpu.mode = BR_GPU_OFF;
    if (m && (!strcmp(m, "on") || !strcmp(m, "1"))) br_gpu.mode = BR_GPU_ON;
    const char *l = getenv("BRINE_GPU_LOG");
    br_gpu.log = l && *l && strcmp(l, "0") != 0;
    br_gpu.bw = 8e9;
    br_gpu.init_s = 0.3;
    br_gpu.speed = 150;
    br_gpu.f64_ratio = 40;
}

static void br_gpu_logf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    br_lock(&br_io_mu);
    fputs("brine gpu: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
    br_unlock(&br_io_mu);
    va_end(ap);
}

/* ------------------------------------------------------------ devices */
/* What the rest needs from a GPU; CUDA and Metal below. Calls return 0 or an
 * error br_dev_err describes. Device pointers are addresses the kernels use
 * as they are. */
static int br_dev_open(const char *code, char *why, size_t nwhy);
static const char *br_dev_err(br_gpu_res r);
/* makes the device current on this thread */
static br_gpu_res br_dev_bind(void);
static br_gpu_res br_dev_alloc(br_gpu_ptr *p, size_t n);
static void br_dev_free(br_gpu_ptr p);
/* host memory the device copies from quickly, or NULL */
static void *br_dev_host_alloc(size_t n);
static void br_dev_host_free(void *p);
static br_gpu_res br_dev_htod(br_gpu_ptr d, const void *h, size_t n);
static br_gpu_res br_dev_dtoh(void *h, br_gpu_ptr d, size_t n);
static br_gpu_res br_dev_zero(br_gpu_ptr d, size_t n);
static br_gpu_res br_dev_func(void **fn, const char *name);
/* `count` threads of a construct's kernel, and waits for them. args: the
 * context, n, the first index, count, and for reductions where the leaves'
 * values and flags go. */
static br_gpu_res br_dev_run(void *fn, int64_t count, const uint64_t *args, int nargs);
/* `blocks` blocks of BR_GPU_BLOCK threads of a combine kernel. args: values
 * and flags in, their count, values and flags out. */
static br_gpu_res br_dev_comb(void *fn, int64_t blocks, const uint64_t *args, int nargs);

#if defined(__APPLE__)
/* Metal, through the Objective-C runtime: the frameworks are loaded when
 * first needed, so there's nothing to link. The program's module is MSL
 * source (msl.br); "// stack N" on its second line is the device stack each
 * thread needs. Memory is shared with the CPU, so copies are memcpy. */
typedef void *br_id;
typedef void *br_sel;
typedef struct {
    unsigned long w, h, d;
} br_mtl_size;

typedef struct {
    char *name;
    br_id ps;
} br_mtl_fn;

static struct {
    br_id (*cls)(const char *);
    br_sel (*sel)(const char *);
    void (*msg)(void); /* objc_msgSend: cast to each call's type */
    void *(*push)(void);
    void (*pop)(void *);
    br_id dev, queue, lib;
    br_id buf, flag, stk; /* the main buffer, the error flag's, the stack's */
    br_gpu_ptr buf_at, flag_at, stk_at;
    char *buf_mem, *flag_mem;
    size_t buf_len, stk_len;
    uint64_t stack;
    br_mtl_fn *fns;
    int nfns;
    char err[256];
} br_mtl;

enum { BR_MTL_FAIL = 1, BR_MTL_STATUS_ERROR = 5, BR_MTL_KERNEL = 3 };

#define BR_OBJC(ret, ...) ((ret(*)(br_id, br_sel, ##__VA_ARGS__))br_mtl.msg)
static br_id br_oc(br_id o, const char *s) { return BR_OBJC(br_id)(o, br_mtl.sel(s)); }
static br_id br_oc_o(br_id o, const char *s, br_id a) { return BR_OBJC(br_id, br_id)(o, br_mtl.sel(s), a); }
static unsigned long br_oc_u(br_id o, const char *s) { return BR_OBJC(unsigned long)(o, br_mtl.sel(s)); }
static int br_oc_has(br_id o, const char *s) { return BR_OBJC(unsigned char, br_sel)(o, br_mtl.sel("respondsToSelector:"), br_mtl.sel(s)) != 0; }
static br_id br_oc_str(const char *s) {
    return BR_OBJC(br_id, const char *)(br_mtl.cls("NSString"), br_mtl.sel("stringWithUTF8String:"), s);
}
static const char *br_oc_cstr(br_id nsstr) {
    const char *s = nsstr ? BR_OBJC(const char *)(nsstr, br_mtl.sel("UTF8String")) : NULL;
    return s ? s : "unknown error";
}

static br_gpu_res br_mtl_fail(const char *what, br_id nserr) {
    snprintf(br_mtl.err, sizeof br_mtl.err, "%s%s%s", what, nserr ? ": " : "", nserr ? br_oc_cstr(br_oc(nserr, "localizedDescription")) : "");
    return BR_MTL_FAIL;
}

static const char *br_dev_err(br_gpu_res r) { return r == BR_GPU_OOM ? "out of memory" : br_mtl.err; }

/* a shared buffer of n bytes, and its GPU address */
static br_id br_mtl_buffer(size_t n, br_gpu_ptr *at) {
    br_id b = BR_OBJC(br_id, unsigned long, unsigned long)(br_mtl.dev, br_mtl.sel("newBufferWithLength:options:"), (unsigned long)n, 0);
    if (b) *at = (br_gpu_ptr)br_oc_u(b, "gpuAddress");
    return b;
}

static int br_dev_open(const char *code, char *why, size_t nwhy) {
    if (strncmp(code, "// brine metal\n// stack ", 24) != 0) {
        snprintf(why, nwhy, "the program wasn't built for Metal");
        return 0;
    }
    br_mtl.stack = strtoull(code + 24, NULL, 10);
    void *objc = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW | RTLD_LOCAL);
    void *fnd = dlopen("/System/Library/Frameworks/Foundation.framework/Foundation", RTLD_NOW | RTLD_LOCAL);
    void *mtl = dlopen("/System/Library/Frameworks/Metal.framework/Metal", RTLD_NOW | RTLD_LOCAL);
    if (!objc || !fnd || !mtl) {
        snprintf(why, nwhy, "no Metal framework");
        return 0;
    }
    *(void **)&br_mtl.cls = dlsym(objc, "objc_getClass");
    *(void **)&br_mtl.sel = dlsym(objc, "sel_registerName");
    *(void **)&br_mtl.msg = dlsym(objc, "objc_msgSend");
    *(void **)&br_mtl.push = dlsym(objc, "objc_autoreleasePoolPush");
    *(void **)&br_mtl.pop = dlsym(objc, "objc_autoreleasePoolPop");
    br_id (*create)(void) = NULL;
    *(void **)&create = dlsym(mtl, "MTLCreateSystemDefaultDevice");
    if (!br_mtl.cls || !br_mtl.sel || !br_mtl.msg || !br_mtl.push || !br_mtl.pop || !create) {
        snprintf(why, nwhy, "the Objective-C runtime or Metal is missing a function");
        return 0;
    }
    void *pool = br_mtl.push();
    int ok = 0;
    br_id dev = create();
    if (!dev) {
        snprintf(why, nwhy, "no Metal device");
        goto out;
    }
    br_mtl.dev = dev;
    snprintf(br_gpu.name, sizeof br_gpu.name, "%s", br_oc_cstr(br_oc(dev, "name")));
    br_mtl.flag = br_mtl_buffer(256, &br_mtl.flag_at);
    if (!br_mtl.flag || !br_oc_has(br_mtl.flag, "gpuAddress")) {
        snprintf(why, nwhy, "%s has no Metal 3 (GPU addresses need macOS 13)", br_gpu.name);
        goto out;
    }
    br_mtl.flag_mem = (char *)br_oc(br_mtl.flag, "contents");
    /* exact arithmetic, as on the CPU */
    br_id opts = br_oc(br_oc(br_mtl.cls("MTLCompileOptions"), "alloc"), "init");
    if (br_oc_has(opts, "setMathMode:"))
        BR_OBJC(void, long)(opts, br_mtl.sel("setMathMode:"), 0);
    else
        BR_OBJC(void, _Bool)(opts, br_mtl.sel("setFastMathEnabled:"), 0);
    br_id err = NULL;
    br_mtl.lib = BR_OBJC(br_id, br_id, br_id, br_id *)(dev, br_mtl.sel("newLibraryWithSource:options:error:"), br_oc_str(code), opts, &err);
    BR_OBJC(void)(opts, br_mtl.sel("release"));
    if (!br_mtl.lib) {
        snprintf(why, nwhy, "the Metal source doesn't compile: %.180s", err ? br_oc_cstr(br_oc(err, "localizedDescription")) : "");
        goto out;
    }
    /* every kernel's pipeline now, while the program runs on the CPU */
    br_id names = br_oc(br_mtl.lib, "functionNames");
    unsigned long nn = br_oc_u(names, "count");
    br_mtl.fns = (br_mtl_fn *)calloc(nn ? nn : 1, sizeof(br_mtl_fn));
    if (!br_mtl.fns) {
        snprintf(why, nwhy, "out of memory");
        goto out;
    }
    for (unsigned long i = 0; i < nn; i++) {
        br_id nm = BR_OBJC(br_id, unsigned long)(names, br_mtl.sel("objectAtIndex:"), i);
        const char *cn = br_oc_cstr(nm);
        br_id f = br_oc_o(br_mtl.lib, "newFunctionWithName:", nm);
        /* kernels only */
        if (f && br_oc_u(f, "functionType") != BR_MTL_KERNEL) {
            BR_OBJC(void)(f, br_mtl.sel("release"));
            continue;
        }
        err = NULL;
        br_id ps = f ? BR_OBJC(br_id, br_id, br_id *)(dev, br_mtl.sel("newComputePipelineStateWithFunction:error:"), f, &err) : NULL;
        if (f) BR_OBJC(void)(f, br_mtl.sel("release"));
        if (!ps) {
            snprintf(why, nwhy, "no pipeline for %s: %.160s", cn, err ? br_oc_cstr(br_oc(err, "localizedDescription")) : "");
            goto out;
        }
        br_mtl.fns[br_mtl.nfns].name = strdup(cn);
        br_mtl.fns[br_mtl.nfns++].ps = ps;
    }
    br_mtl.queue = br_oc(dev, "newCommandQueue");
    if (!br_mtl.queue) {
        snprintf(why, nwhy, "no Metal command queue");
        goto out;
    }
    br_gpu.err = br_mtl.flag_at;
    /* memory is shared: copies run at memcpy speed */
    br_gpu.bw = 30e9;
    br_gpu.speed = 60;
    ok = 1;
out:
    br_mtl.pop(pool);
    return ok;
}

static br_gpu_res br_dev_bind(void) { return 0; }

/* one main buffer at a time: br_gpu_reserve frees the old one first */
static br_gpu_res br_dev_alloc(br_gpu_ptr *p, size_t n) {
    br_id b = br_mtl_buffer(n, p);
    if (!b) return BR_GPU_OOM;
    br_mtl.buf = b;
    br_mtl.buf_at = *p;
    br_mtl.buf_mem = (char *)br_oc(b, "contents");
    br_mtl.buf_len = n;
    return 0;
}

static void br_dev_free(br_gpu_ptr p) {
    if (!br_mtl.buf || p != br_mtl.buf_at) return;
    BR_OBJC(void)(br_mtl.buf, br_mtl.sel("release"));
    br_mtl.buf = NULL;
    br_mtl.buf_len = 0;
}

static void *br_dev_host_alloc(size_t n) {
    (void)n;
    return NULL;
}
static void br_dev_host_free(void *p) { (void)p; }

/* where device memory is in the CPU's view of it */
static char *br_mtl_host(br_gpu_ptr d, size_t n) {
    if (br_mtl.buf && d >= br_mtl.buf_at && d + n <= br_mtl.buf_at + br_mtl.buf_len) return br_mtl.buf_mem + (d - br_mtl.buf_at);
    if (d >= br_mtl.flag_at && d + n <= br_mtl.flag_at + 256) return br_mtl.flag_mem + (d - br_mtl.flag_at);
    snprintf(br_mtl.err, sizeof br_mtl.err, "an address outside the GPU's buffers");
    return NULL;
}

static br_gpu_res br_dev_htod(br_gpu_ptr d, const void *h, size_t n) {
    char *m = br_mtl_host(d, n);
    if (!m) return BR_MTL_FAIL;
    memcpy(m, h, n);
    return 0;
}

static br_gpu_res br_dev_dtoh(void *h, br_gpu_ptr d, size_t n) {
    char *m = br_mtl_host(d, n);
    if (!m) return BR_MTL_FAIL;
    memcpy(h, m, n);
    return 0;
}

static br_gpu_res br_dev_zero(br_gpu_ptr d, size_t n) {
    char *m = br_mtl_host(d, n);
    if (!m) return BR_MTL_FAIL;
    memset(m, 0, n);
    return 0;
}

static br_gpu_res br_dev_func(void **fn, const char *name) {
    for (int i = 0; i < br_mtl.nfns; i++)
        if (!strcmp(br_mtl.fns[i].name, name)) {
            *fn = br_mtl.fns[i].ps;
            return 0;
        }
    snprintf(br_mtl.err, sizeof br_mtl.err, "no kernel %s", name);
    return BR_MTL_FAIL;
}

/* Runs one dispatch and waits for it: `groups` threadgroups of `per`
 * threads, or (groups 0) `threads` threads. */
static br_gpu_res br_mtl_dispatch(br_id ps, const uint64_t *a, size_t na, unsigned long threads, unsigned long groups, unsigned long per) {
    void *pool = br_mtl.push();
    br_gpu_res r = 0;
    br_id cb = br_oc(br_mtl.queue, "commandBuffer");
    br_id enc = cb ? br_oc(cb, "computeCommandEncoder") : NULL;
    if (!enc) {
        r = br_mtl_fail("no Metal command buffer", NULL);
        goto out;
    }
    BR_OBJC(void, br_id)(enc, br_mtl.sel("setComputePipelineState:"), ps);
    br_sel set = br_mtl.sel("setBuffer:offset:atIndex:");
    BR_OBJC(void, br_id, unsigned long, unsigned long)(enc, set, br_mtl.buf, 0, 0);
    BR_OBJC(void, const void *, unsigned long, unsigned long)(enc, br_mtl.sel("setBytes:length:atIndex:"), a, (unsigned long)(na * 8), 1);
    BR_OBJC(void, br_id, unsigned long, unsigned long)(enc, set, br_mtl.flag, 0, 2);
    /* the kernels reach memory by address, not through their arguments (which
     * the Metal compiler drops when unused): every buffer has to be made
     * resident (read and write) */
    br_sel use = br_mtl.sel("useResource:usage:");
    BR_OBJC(void, br_id, unsigned long)(enc, use, br_mtl.buf, 3);
    BR_OBJC(void, br_id, unsigned long)(enc, use, br_mtl.flag, 3);
    if (br_mtl.stk) BR_OBJC(void, br_id, unsigned long)(enc, use, br_mtl.stk, 3);
    br_mtl_size tg = {per, 1, 1};
    if (groups) {
        br_mtl_size g = {groups, 1, 1};
        BR_OBJC(void, br_mtl_size, br_mtl_size)(enc, br_mtl.sel("dispatchThreadgroups:threadsPerThreadgroup:"), g, tg);
    } else {
        br_mtl_size g = {threads, 1, 1};
        BR_OBJC(void, br_mtl_size, br_mtl_size)(enc, br_mtl.sel("dispatchThreads:threadsPerThreadgroup:"), g, tg);
    }
    BR_OBJC(void)(enc, br_mtl.sel("endEncoding"));
    BR_OBJC(void)(cb, br_mtl.sel("commit"));
    BR_OBJC(void)(cb, br_mtl.sel("waitUntilCompleted"));
    if (br_oc_u(cb, "status") == BR_MTL_STATUS_ERROR) r = br_mtl_fail("a Metal command failed", br_oc(cb, "error"));
out:
    br_mtl.pop(pool);
    return r;
}

/* the device stack for `threads` threads at a time (bytes) */
static br_gpu_res br_mtl_stack(uint64_t bytes) {
    if (br_mtl.stk_len >= bytes) return 0;
    if (br_mtl.stk) BR_OBJC(void)(br_mtl.stk, br_mtl.sel("release"));
    br_mtl.stk = br_mtl_buffer((size_t)bytes, &br_mtl.stk_at);
    br_mtl.stk_len = br_mtl.stk ? (size_t)bytes : 0;
    return br_mtl.stk ? 0 : BR_GPU_OOM;
}

static br_gpu_res br_dev_run(void *fn, int64_t count, const uint64_t *args, int nargs) {
    /* msl.br's kernel arguments: context, n, first index, count, error flag,
     * stack, stack per thread, first thread, values, flags */
    uint64_t a[10] = {0};
    for (int i = 0; i < nargs && i < 6; i++) a[i < 4 ? i : i + 4] = args[i];
    a[4] = br_mtl.flag_at;
    a[6] = br_mtl.stack;
    unsigned long max = br_oc_u((br_id)fn, "maxTotalThreadsPerThreadgroup");
    unsigned long per = max < BR_GPU_BLOCK ? max : BR_GPU_BLOCK;
    /* threads at a time: as many as 256 MB of stack holds */
    int64_t step = count;
    if (br_mtl.stack > 0) {
        int64_t most = (int64_t)(((uint64_t)256 << 20) / br_mtl.stack);
        if (most < 1) most = 1;
        if (step > most) step = most;
        br_gpu_res r = br_mtl_stack((uint64_t)step * br_mtl.stack);
        if (r) return r;
        a[5] = br_mtl.stk_at;
    }
    for (int64_t t0 = 0; t0 < count; t0 += step) {
        int64_t m = count - t0 < step ? count - t0 : step;
        a[7] = (uint64_t)t0;
        br_gpu_res r = br_mtl_dispatch((br_id)fn, a, 10, (unsigned long)m, 0, per);
        if (r) return r;
    }
    return 0;
}

static br_gpu_res br_dev_comb(void *fn, int64_t blocks, const uint64_t *args, int nargs) {
    uint64_t a[6] = {0};
    for (int i = 0; i < nargs && i < 5; i++) a[i] = args[i];
    a[5] = br_mtl.flag_at;
    if (br_oc_u((br_id)fn, "maxTotalThreadsPerThreadgroup") < BR_GPU_BLOCK) {
        snprintf(br_mtl.err, sizeof br_mtl.err, "a combine kernel can't run %d threads per threadgroup", BR_GPU_BLOCK);
        return BR_MTL_FAIL;
    }
    return br_mtl_dispatch((br_id)fn, a, 6, 0, (unsigned long)blocks, BR_GPU_BLOCK);
}

#else
/* CUDA: the driver is loaded when first needed (nvcuda.dll, libcuda.so.1),
 * and JIT-compiles the program's PTX (ptx.br). */
typedef struct {
    br_gpu_res (*Init)(unsigned);
    br_gpu_res (*DeviceGetCount)(int *);
    br_gpu_res (*DeviceGet)(int *, int);
    br_gpu_res (*DeviceGetAttribute)(int *, int, int);
    br_gpu_res (*DeviceGetName)(char *, int, int);
    br_gpu_res (*DevicePrimaryCtxRetain)(void **, int);
    br_gpu_res (*CtxSetCurrent)(void *);
    br_gpu_res (*CtxSynchronize)(void);
    br_gpu_res (*ModuleLoadDataEx)(void **, const void *, unsigned, int *, void **);
    br_gpu_res (*ModuleGetFunction)(void **, void *, const char *);
    br_gpu_res (*ModuleGetGlobal)(br_gpu_ptr *, size_t *, void *, const char *);
    br_gpu_res (*MemAlloc)(br_gpu_ptr *, size_t);
    br_gpu_res (*MemFree)(br_gpu_ptr);
    br_gpu_res (*MemAllocHost)(void **, size_t);
    br_gpu_res (*MemFreeHost)(void *);
    br_gpu_res (*MemcpyHtoD)(br_gpu_ptr, const void *, size_t);
    br_gpu_res (*MemcpyDtoH)(void *, br_gpu_ptr, size_t);
    br_gpu_res (*MemsetD8)(br_gpu_ptr, unsigned char, size_t);
    br_gpu_res (*LaunchKernel)(void *, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, void *, void **, void **);
    br_gpu_res (*GetErrorString)(br_gpu_res, const char **);
} br_cu_api;

enum { BR_CU_ATTR_SMS = 16, BR_CU_ATTR_CC_MAJOR = 75, BR_CU_ATTR_CC_MINOR = 76 };
enum { BR_CU_JIT_ERROR_LOG = 5, BR_CU_JIT_ERROR_LOG_SIZE = 6 };

static br_cu_api br_cu;
static void *br_cu_ctx, *br_cu_mod;

static const char *br_dev_err(br_gpu_res r) {
    const char *s = NULL;
    if (br_cu.GetErrorString) br_cu.GetErrorString(r, &s);
    return s ? s : "unknown CUDA error";
}

#ifdef _WIN32
static void *br_cu_open(void) { return (void *)LoadLibraryA("nvcuda.dll"); }
static void *br_cu_sym(void *lib, const char *name) { return (void *)GetProcAddress((HMODULE)lib, name); }
#else
static void *br_cu_open(void) {
    void *h = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
    return h ? h : dlopen("libcuda.so", RTLD_NOW | RTLD_LOCAL);
}
static void *br_cu_sym(void *lib, const char *name) { return dlsym(lib, name); }
#endif

/* Loads the driver, picks device 0 and JIT-compiles the program's PTX. On
 * failure, `why` says what's missing. Called without br_gpu.mu held. */
static int br_dev_open(const char *ptx, char *why, size_t nwhy) {
    void *lib = br_cu_open();
    if (!lib) {
        snprintf(why, nwhy, "no CUDA driver");
        return 0;
    }
    br_cu_api *c = &br_cu;
#define BR_CU(field, name)                                         \
    if (!(*(void **)&c->field = br_cu_sym(lib, name))) {           \
        snprintf(why, nwhy, "the CUDA driver has no %s", name);    \
        return 0;                                                  \
    }
    BR_CU(Init, "cuInit")
    BR_CU(DeviceGetCount, "cuDeviceGetCount")
    BR_CU(DeviceGet, "cuDeviceGet")
    BR_CU(DeviceGetAttribute, "cuDeviceGetAttribute")
    BR_CU(DeviceGetName, "cuDeviceGetName")
    BR_CU(DevicePrimaryCtxRetain, "cuDevicePrimaryCtxRetain")
    BR_CU(CtxSetCurrent, "cuCtxSetCurrent")
    BR_CU(CtxSynchronize, "cuCtxSynchronize")
    BR_CU(ModuleLoadDataEx, "cuModuleLoadDataEx")
    BR_CU(ModuleGetFunction, "cuModuleGetFunction")
    BR_CU(ModuleGetGlobal, "cuModuleGetGlobal_v2")
    BR_CU(MemAlloc, "cuMemAlloc_v2")
    BR_CU(MemFree, "cuMemFree_v2")
    BR_CU(MemAllocHost, "cuMemAllocHost_v2")
    BR_CU(MemFreeHost, "cuMemFreeHost")
    BR_CU(MemcpyHtoD, "cuMemcpyHtoD_v2")
    BR_CU(MemcpyDtoH, "cuMemcpyDtoH_v2")
    BR_CU(MemsetD8, "cuMemsetD8_v2")
    BR_CU(LaunchKernel, "cuLaunchKernel")
    BR_CU(GetErrorString, "cuGetErrorString")
#undef BR_CU
    if (!strncmp(ptx, "// brine metal", 14)) {
        snprintf(why, nwhy, "the program was built for Metal");
        return 0;
    }
    br_gpu_res r;
    int n = 0, dev = 0;
    if ((r = c->Init(0)) != 0 || (r = c->DeviceGetCount(&n)) != 0) {
        snprintf(why, nwhy, "cuInit: %s", br_dev_err(r));
        return 0;
    }
    if (n <= 0 || c->DeviceGet(&dev, 0) != 0) {
        snprintf(why, nwhy, "no CUDA device");
        return 0;
    }
    int major = 0, minor = 0;
    c->DeviceGetAttribute(&major, BR_CU_ATTR_CC_MAJOR, dev);
    c->DeviceGetAttribute(&minor, BR_CU_ATTR_CC_MINOR, dev);
    /* x.0 data-center parts (P100, V100, A100, H100, B200) run f64 at half
     * the f32 rate; the others at 1/32 or 1/64 */
    if (minor == 0 && major >= 6) br_gpu.f64_ratio = 75;
    c->DeviceGetAttribute(&br_gpu.sms, BR_CU_ATTR_SMS, dev);
    c->DeviceGetName(br_gpu.name, (int)sizeof br_gpu.name, dev);
    if (major < 5) {
        snprintf(why, nwhy, "%s is older than compute capability 5.0", br_gpu.name);
        return 0;
    }
    if ((r = c->DevicePrimaryCtxRetain(&br_cu_ctx, dev)) != 0 || (r = c->CtxSetCurrent(br_cu_ctx)) != 0) {
        snprintf(why, nwhy, "no CUDA context: %s", br_dev_err(r));
        return 0;
    }
    static char log[4096];
    int opts[2] = {BR_CU_JIT_ERROR_LOG, BR_CU_JIT_ERROR_LOG_SIZE};
    void *vals[2] = {log, (void *)(uintptr_t)sizeof log};
    if ((r = c->ModuleLoadDataEx(&br_cu_mod, ptx, 2, opts, vals)) != 0) {
        snprintf(why, nwhy, "the PTX doesn't compile (%s): %.180s", br_dev_err(r), log);
        return 0;
    }
    size_t sz = 0;
    if ((r = c->ModuleGetGlobal(&br_gpu.err, &sz, br_cu_mod, "brine_gpu_err")) != 0) {
        snprintf(why, nwhy, "cuModuleGetGlobal: %s", br_dev_err(r));
        return 0;
    }
    return 1;
}

static br_gpu_res br_dev_bind(void) { return br_cu.CtxSetCurrent(br_cu_ctx); }
static br_gpu_res br_dev_alloc(br_gpu_ptr *p, size_t n) { return br_cu.MemAlloc(p, n); }
static void br_dev_free(br_gpu_ptr p) { br_cu.MemFree(p); }
static void *br_dev_host_alloc(size_t n) {
    void *p = NULL;
    return br_cu.MemAllocHost(&p, n) == 0 ? p : NULL;
}
static void br_dev_host_free(void *p) { br_cu.MemFreeHost(p); }
static br_gpu_res br_dev_htod(br_gpu_ptr d, const void *h, size_t n) { return br_cu.MemcpyHtoD(d, h, n); }
static br_gpu_res br_dev_dtoh(void *h, br_gpu_ptr d, size_t n) { return br_cu.MemcpyDtoH(h, d, n); }
static br_gpu_res br_dev_zero(br_gpu_ptr d, size_t n) { return br_cu.MemsetD8(d, 0, n); }
static br_gpu_res br_dev_func(void **fn, const char *name) { return br_cu.ModuleGetFunction(fn, br_cu_mod, name); }

static br_gpu_res br_cu_launch(void *fn, int64_t blocks, const uint64_t *args, int nargs) {
    void *p[8];
    for (int i = 0; i < nargs && i < 8; i++) p[i] = (void *)&args[i];
    br_gpu_res r = br_cu.LaunchKernel(fn, (unsigned)blocks, 1, 1, BR_GPU_BLOCK, 1, 1, 0, NULL, p, NULL);
    return r ? r : br_cu.CtxSynchronize();
}

static br_gpu_res br_dev_run(void *fn, int64_t count, const uint64_t *args, int nargs) {
    return br_cu_launch(fn, (count + BR_GPU_BLOCK - 1) / BR_GPU_BLOCK, args, nargs);
}

static br_gpu_res br_dev_comb(void *fn, int64_t blocks, const uint64_t *args, int nargs) { return br_cu_launch(fn, blocks, args, nargs); }
#endif

static void br_gpu_init_main(void *arg) {
    char why[256] = "";
    double t0 = brine_clock();
    int ok = br_dev_open((const char *)arg, why, sizeof why);
    double dt = brine_clock() - t0;
    br_lock(&br_gpu.mu);
    br_gpu.state = ok ? BR_GS_READY : BR_GS_FAILED;
    if (ok) br_gpu.init_s = dt;
    memcpy(br_gpu.why, why, sizeof why);
    br_cond_broadcast(&br_gpu.cv);
    br_unlock(&br_gpu.mu);
    if (br_gpu.log) {
        if (ok)
            br_gpu_logf("%s ready in %.0f ms", br_gpu.name, dt * 1e3);
        else
            br_gpu_logf("not used: %s", why);
    }
}

/* Starts getting the device ready (in the background). Holds br_gpu.mu. */
static void br_gpu_start(const char *ptx) {
    if (br_gpu.state != BR_GS_NONE) return;
    br_gpu.state = BR_GS_INIT;
    br_gpu.ptx = ptx;
    br_thread th;
    if (!br_thread_start(&th, br_gpu_init_main, (void *)ptx)) {
        br_gpu.state = BR_GS_FAILED;
        snprintf(br_gpu.why, sizeof br_gpu.why, "no thread to start the GPU");
    }
}

/* Exiting while the CUDA driver starts up on its thread crashes in the
 * driver, so the way out waits for that to finish. */
static void br_gpu_settle(void) {
    if (__atomic_load_n(&br_gpu.state, __ATOMIC_ACQUIRE) != BR_GS_INIT) return;
    br_lock(&br_gpu.mu);
    while (br_gpu.state == BR_GS_INIT) br_cond_wait(&br_gpu.cv, &br_gpu.mu);
    br_unlock(&br_gpu.mu);
}

/* Waits until the device is ready or has failed. Holds br_gpu.mu. */
static int br_gpu_wait(const char *ptx) {
    br_gpu_start(ptx);
    while (br_gpu.state == BR_GS_INIT) br_cond_wait(&br_gpu.cv, &br_gpu.mu);
    return br_gpu.state == BR_GS_READY;
}

/* A device call failed: no more GPU for this run. */
static void br_gpu_fail(const char *what, br_gpu_res r) {
    br_lock(&br_gpu.mu);
    br_gpu.state = BR_GS_FAILED;
    snprintf(br_gpu.why, sizeof br_gpu.why, "%s: %s", what, br_dev_err(r));
    br_unlock(&br_gpu.mu);
    if (br_gpu.log) br_gpu_logf("disabled: %s: %s", what, br_dev_err(r));
}

static br_kst *br_kstate(brine_kernel *k) {
    if (!k->state) {
        br_kst *s = (br_kst *)calloc(1, sizeof(br_kst));
        if (!s) return NULL;
        k->state = s;
    }
    return (br_kst *)k->state;
}

/* ------------------------------------------------------------ copies */
/* One array the device needs: its header at `hoff` and its elements at
 * `doff` in the device buffer. Staged ones are copied with the context in one
 * piece (headers, small arrays, arrays of arrays, whose pointers are
 * rewritten); the others straight from the array. */
typedef struct {
    const brine_arr *host;
    int64_t node;
    uint64_t hoff, doff, bytes;
    int written, read, staged;
} br_garr;

typedef struct {
    const brine_kernel *k;
    const int64_t *nodes;
    br_garr *a;
    int64_t na, acap;
    int64_t *tab; /* open addressing, index + 1 */
    int64_t tcap;
    uint64_t stage, direct;
    int ok;
} br_gplan;

#define BR_ALIGN(x, a) (((x) + (a) - 1) & ~(uint64_t)((a) - 1))
#define BR_GPU_SMALL 65536

static int64_t br_gplan_find(br_gplan *p, const void *h, int64_t *slot) {
    uint64_t x = (uint64_t)(uintptr_t)h;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    int64_t i = (int64_t)(x & (uint64_t)(p->tcap - 1));
    while (p->tab[i]) {
        if (p->a[p->tab[i] - 1].host == h) return p->tab[i] - 1;
        i = (i + 1) & (p->tcap - 1);
    }
    *slot = i;
    return -1;
}

static void br_gplan_grow(br_gplan *p) {
    int64_t nc = p->tcap ? p->tcap * 2 : 64;
    free(p->tab);
    p->tab = (int64_t *)calloc((size_t)nc, sizeof(int64_t));
    p->tcap = nc;
    if (!p->tab) {
        p->ok = 0;
        return;
    }
    for (int64_t j = 0; j < p->na; j++) {
        int64_t slot = 0;
        br_gplan_find(p, p->a[j].host, &slot);
        p->tab[slot] = j + 1;
    }
}

static void br_gplan_add(br_gplan *p, const brine_arr *a, int64_t node, int written, int read, int depth) {
    if (!p->ok || !a) return;
    if (depth > 16) {
        p->ok = 0;
        return;
    }
    if ((p->na + 1) * 2 > p->tcap) {
        br_gplan_grow(p);
        if (!p->ok) return;
    }
    int64_t slot = 0;
    int64_t i = br_gplan_find(p, a, &slot);
    if (i >= 0) {
        p->a[i].written |= written;
        p->a[i].read |= read;
        return;
    }
    if (p->na == p->acap) {
        p->acap = p->acap ? p->acap * 2 : 16;
        br_garr *na = (br_garr *)realloc(p->a, (size_t)p->acap * sizeof(br_garr));
        if (!na) {
            p->ok = 0;
            return;
        }
        p->a = na;
    }
    const int64_t *nd = p->nodes + node;
    int64_t esz = nd[0], nsub = nd[1];
    if (a->esz != esz && a->len > 0) {
        p->ok = 0;
        return;
    }
    br_garr *g = &p->a[p->na];
    g->host = a;
    g->node = node;
    g->bytes = (uint64_t)a->len * (uint64_t)esz;
    g->written = written;
    g->read = read;
    g->hoff = p->stage;
    p->stage += 64;
    g->staged = nsub > 0 || g->bytes <= BR_GPU_SMALL;
    if (g->staged) {
        g->doff = p->stage;
        p->stage = BR_ALIGN(p->stage + g->bytes, 64);
    } else {
        g->doff = p->direct;
        p->direct = BR_ALIGN(p->direct + g->bytes, 256);
    }
    p->tab[slot] = ++p->na;
    for (int64_t e = 0; e < a->len && nsub > 0; e++)
        for (int64_t s = 0; s < nsub; s++) {
            const brine_arr *c = *(brine_arr *const *)(a->data + e * esz + nd[2 + 2 * s]);
            br_gplan_add(p, c, nd[3 + 2 * s], 0, 1, depth + 1);
        }
}

/* The arrays a construct's context reaches. */
static int br_gplan_make(br_gplan *p, const brine_kernel *k, const void *ctx) {
    memset(p, 0, sizeof *p);
    p->k = k;
    p->ok = 1;
    int64_t nf = k->ptrs[0];
    p->nodes = k->ptrs + 1 + 3 * nf;
    p->stage = BR_ALIGN((uint64_t)k->ctx_size, 64);
    for (int64_t f = 0; f < nf; f++) {
        const int64_t *fd = k->ptrs + 1 + 3 * f;
        const brine_arr *a = *(brine_arr *const *)((const char *)ctx + fd[0]);
        br_gplan_add(p, a, fd[2], (fd[1] & 1) != 0, (fd[1] & 2) == 0, 0);
    }
    for (int64_t i = 0; i < p->na; i++)
        if (!p->a[i].staged) p->a[i].doff += BR_ALIGN(p->stage, 256);
    return p->ok;
}

static void br_gplan_free(br_gplan *p) {
    free(p->a);
    free(p->tab);
}

static uint64_t br_gplan_size(const br_gplan *p) { return BR_ALIGN(p->stage, 256) + p->direct; }

static uint64_t br_gplan_bytes(const br_gplan *p) {
    uint64_t b = p->stage;
    for (int64_t i = 0; i < p->na; i++)
        if (!p->a[i].staged) b += (p->a[i].read ? p->a[i].bytes : 0) + (p->a[i].written ? p->a[i].bytes : 0);
    return b;
}

static int64_t br_gplan_index(br_gplan *p, const void *h) {
    int64_t slot = 0;
    return br_gplan_find(p, h, &slot);
}

/* Device memory for `n` bytes (one buffer, kept between launches). */
static int br_gpu_reserve(uint64_t n, br_gpu_res *r) {
    *r = 0;
    if (br_gpu.cap >= n) return 1;
    if (br_gpu.buf) br_dev_free(br_gpu.buf);
    br_gpu.buf = 0;
    br_gpu.cap = 0;
    uint64_t c = n + n / 4;
    if ((*r = br_dev_alloc(&br_gpu.buf, (size_t)c)) != 0) {
        br_gpu.buf = 0;
        return 0;
    }
    br_gpu.cap = (size_t)c;
    return 1;
}

static char *br_gpu_stage(uint64_t n) {
    if (br_gpu.stage_cap >= n) return br_gpu.stage;
    if (br_gpu.stage) {
        if (br_gpu.stage_pinned)
            br_dev_host_free(br_gpu.stage);
        else
            free(br_gpu.stage);
    }
    uint64_t c = n + n / 4 + 4096;
    void *p = br_dev_host_alloc((size_t)c);
    br_gpu.stage_pinned = p != NULL;
    if (!br_gpu.stage_pinned) p = malloc((size_t)c);
    br_gpu.stage = (char *)p;
    br_gpu.stage_cap = p ? (size_t)c : 0;
    return br_gpu.stage;
}

/* Copies the context and the arrays to the device: the device context is at
 * br_gpu.buf. Holds br_gpu.run. */
static br_gpu_res br_gpu_copy_in(br_gplan *p, const void *ctx) {
    char *st = br_gpu_stage(p->stage);
    if (!st) return BR_GPU_OOM;
    br_gpu_ptr base = br_gpu.buf;
    const brine_kernel *k = p->k;
    memcpy(st, ctx, (size_t)k->ctx_size);
    int64_t nf = k->ptrs[0];
    for (int64_t f = 0; f < nf; f++) {
        const int64_t *fd = k->ptrs + 1 + 3 * f;
        const void *h = *(void *const *)((const char *)ctx + fd[0]);
        int64_t i = br_gplan_index(p, h);
        uint64_t d = i >= 0 ? base + p->a[i].hoff : 0;
        memcpy(st + fd[0], &d, 8);
    }
    for (int64_t i = 0; i < p->na; i++) {
        br_garr *g = &p->a[i];
        memcpy(st + g->hoff, g->host, sizeof(brine_arr));
        uint64_t d = base + g->doff;
        memcpy(st + g->hoff + offsetof(brine_arr, data), &d, 8);
        if (!g->staged || g->bytes == 0) continue;
        const int64_t *nd = p->nodes + g->node;
        int64_t esz = nd[0], nsub = nd[1];
        if (g->read || nsub > 0) memcpy(st + g->doff, g->host->data, (size_t)g->bytes);
        for (int64_t e = 0; e < g->host->len && nsub > 0; e++)
            for (int64_t s = 0; s < nsub; s++) {
                char *at = st + g->doff + e * esz + nd[2 + 2 * s];
                void *ch = NULL;
                memcpy(&ch, at, 8);
                int64_t j = br_gplan_index(p, ch);
                uint64_t cd = j >= 0 ? base + p->a[j].hoff : 0;
                memcpy(at, &cd, 8);
            }
    }
    br_gpu_res r = br_dev_htod(base, st, (size_t)p->stage);
    for (int64_t i = 0; i < p->na && r == 0; i++) {
        br_garr *g = &p->a[i];
        if (!g->staged && g->read && g->bytes > 0) r = br_dev_htod(base + g->doff, g->host->data, (size_t)g->bytes);
    }
    if (r == 0) r = br_dev_zero(br_gpu.err, 4);
    return r;
}

/* Copies the arrays the body wrote back. */
static br_gpu_res br_gpu_copy_out(br_gplan *p) {
    br_gpu_res r = 0;
    for (int64_t i = 0; i < p->na && r == 0; i++) {
        br_garr *g = &p->a[i];
        if (g->written && g->bytes > 0) r = br_dev_dtoh(g->host->data, br_gpu.buf + g->doff, (size_t)g->bytes);
    }
    return r;
}

static br_gpu_res br_gpu_errflag(int *flag) {
    uint32_t v = 0;
    br_gpu_res r = br_dev_dtoh(&v, br_gpu.err, 4);
    *flag = v != 0;
    return r;
}

/* ------------------------------------------------------------ policy */
static const char *br_gpu_kind(const brine_kernel *k) {
    return k->kind == 2 ? "par reduction" : (k->kind == 1 ? "par map" : "par for");
}

/* Device time per element, before it has been measured: the CPU's, scaled
 * by how much faster the device runs this kind of code (f64 is slow on most
 * GeForce cards). */
static double br_gpu_guess_ns(const brine_kernel *k, double seq_ns) {
    double f = k->cost > 0 ? (double)k->cost64 / (double)k->cost : 0;
    if (f > 1) f = 1;
    return seq_ns * ((1 - f) / br_gpu.speed + f / br_gpu.f64_ratio);
}

typedef struct {
    int use;          /* run (the rest) on the GPU */
    double cpu_s, gpu_s;
    const char *why;  /* when not */
} br_gpu_choice;

/* Should elements [done, n) run on the device? Holds br_gpu.mu. */
static br_gpu_choice br_gpu_choose(brine_kernel *k, br_kst *st, int64_t rest, uint64_t bytes, int64_t launches) {
    br_gpu_choice c = {0, 0, 0, NULL};
    double gpu_ns = st->gpu_ns > 0 ? st->gpu_ns : br_gpu_guess_ns(k, st->seq_ns);
    c.cpu_s = st->cpu_ns * 1e-9 * (double)rest;
    c.gpu_s = gpu_ns * 1e-9 * (double)rest + (double)bytes / br_gpu.bw + 30e-6 * (double)launches;
    if (br_gpu.mode == BR_GPU_ON) {
        c.use = br_gpu_wait(k->ptx);
        if (!c.use) c.why = br_gpu.why;
        return c;
    }
    if (br_gpu.state == BR_GS_FAILED) {
        c.why = br_gpu.why;
        return c;
    }
    int ready = br_gpu.state == BR_GS_READY;
    double init = ready ? 0 : br_gpu.init_s;
    if (c.gpu_s + init < 0.75 * c.cpu_s) {
        c.use = br_gpu_wait(k->ptx);
        if (!c.use) c.why = br_gpu.why;
        return c;
    }
    /* not now, but maybe soon: get the device ready in the background */
    if (!ready && c.gpu_s < 0.75 * c.cpu_s && c.cpu_s > 2e-3) br_gpu_start(k->ptx);
    c.why = !ready && c.gpu_s < 0.75 * c.cpu_s ? "starting the GPU" : "faster on the CPU";
    return c;
}

/* Runs elements [lo, n) of a `par for` sequentially on this thread until
 * about `secs` have passed; returns where it stopped. */
static int64_t br_probe_for(brine_chunk_fn fn, void *ctx, int64_t lo, int64_t n, double secs, double *ns) {
    double t0 = brine_clock();
    int64_t i = lo, step = 16;
    int saved = br_in_par;
    br_in_par = 1;
    while (i < n) {
        int64_t j = i + step < n ? i + step : n;
        fn(ctx, 0, i, j);
        i = j;
        if (brine_clock() - t0 >= secs) break;
        step *= 2;
    }
    br_in_par = saved;
    double dt = brine_clock() - t0;
    *ns = i > lo ? dt * 1e9 / (double)(i - lo) : 0;
    return i;
}

static void br_gpu_note_cpu(br_kst *st, int64_t count, double secs) {
    if (count <= 0 || !st) return;
    br_lock(&br_gpu.mu);
    double ns = secs * 1e9 / (double)count;
    /* a measurement replaces the probe's estimate: the first elements may
     * not be typical */
    st->cpu_ns = st->cpu_seen ? 0.5 * (st->cpu_ns + ns) : ns;
    st->cpu_seen = 1;
    st->seq_ns = st->cpu_ns * br_threads() * BR_CPU_EFF;
    br_unlock(&br_gpu.mu);
}

/* Launches `entry` for elements [lo, n) in slices of about BR_GPU_SLICE. */
static br_gpu_res br_gpu_run_for(br_kst *st, int64_t lo, int64_t n, int *flag, double *kern) {
    double ns = st->gpu_ns;
    int64_t i = lo;
    *flag = 0;
    *kern = 0;
    while (i < n) {
        int64_t cnt = ns > 0 ? (int64_t)(BR_GPU_SLICE * 1e9 / ns) : 65536;
        if (cnt < 65536) cnt = 65536;
        if (cnt > ((int64_t)1 << 30)) cnt = (int64_t)1 << 30;
        if (cnt > n - i) cnt = n - i;
        uint64_t args[4] = {br_gpu.buf, (uint64_t)n, (uint64_t)i, (uint64_t)cnt};
        double t0 = brine_clock();
        br_gpu_res r = br_dev_run(st->fn, cnt, args, 4);
        if (r) return r;
        double dt = brine_clock() - t0;
        *kern += dt;
        ns = dt * 1e9 / (double)cnt;
        i += cnt;
        if ((r = br_gpu_errflag(flag)) != 0 || *flag) return r;
    }
    if (n > lo) st->gpu_ns = *kern * 1e9 / (double)(n - lo);
    return 0;
}

static br_gpu_res br_gpu_func(br_kst *st, const brine_kernel *k) {
    br_gpu_res r = 0;
    if (!st->fn && (r = br_dev_func(&st->fn, k->entry)) != 0) return r;
    if (k->kind == 2 && !st->comb) r = br_dev_func(&st->comb, k->comb);
    return r;
}

static void br_gpu_log_cpu(brine_kernel *k, int64_t n, const char *why, double secs) {
    if (br_gpu.log) br_gpu_logf("%s %s n=%lld -> cpu %.3f ms (%s%s)", k->where, br_gpu_kind(k), (long long)n, secs * 1e3, k->ptx ? "" : "not GPU-eligible: ", why);
}

/* The GPU part of a `par for`: returns how many leading elements are done
 * (all n when the device ran them, fewer when the CPU should run the rest).
 * Every element below the returned count has run exactly once. */
static int64_t br_gpu_for(brine_kernel *k, void *ctx, int64_t n, brine_chunk_fn fn, const char **why) {
    if (!k->ptx || br_gpu.mode == BR_GPU_OFF) {
        *why = k->ptx ? "BRINE_GPU=off" : k->why;
        return 0;
    }
    *why = "nested in a parallel construct";
    if (br_in_par) return 0;
    *why = "small";
    if (n < BR_GPU_MIN_N && br_gpu.mode != BR_GPU_ON) return 0;
    br_lock(&br_gpu.mu);
    br_kst *st = br_kstate(k);
    int failed = br_gpu.state == BR_GS_FAILED;
    int known = st && st->cpu_ns > 0;
    br_unlock(&br_gpu.mu);
    *why = failed ? br_gpu.why : "out of memory";
    if (!st || failed) return 0;
    int64_t done = 0;
    if (!known && br_gpu.mode != BR_GPU_ON) {
        double ns = 0;
        done = br_probe_for(fn, ctx, 0, n, 1e-4, &ns);
        br_lock(&br_gpu.mu);
        st->seq_ns = ns;
        st->cpu_ns = ns / (br_threads() * BR_CPU_EFF);
        br_unlock(&br_gpu.mu);
        if (done >= n) {
            *why = "done while measuring";
            return done;
        }
    }
    br_gplan p;
    if (!br_gplan_make(&p, k, ctx)) {
        br_gplan_free(&p);
        *why = "out of memory";
        return done;
    }
    /* the arrays go back whole, so the elements the probe wrote must be on
     * the device too */
    if (done > 0)
        for (int64_t i = 0; i < p.na; i++)
            if (p.a[i].written) p.a[i].read = 1;
    br_lock(&br_gpu.mu);
    br_gpu_choice c = br_gpu_choose(k, st, n - done, br_gplan_bytes(&p), 1 + (int64_t)((n - done) / ((int64_t)1 << 22)));
    br_unlock(&br_gpu.mu);
    if (!c.use) {
        br_gplan_free(&p);
        *why = c.why;
        return done;
    }
    *why = "the GPU is busy";
    if (!br_trylock(&br_gpu.run)) {
        br_gplan_free(&p);
        return done;
    }
    double t0 = brine_clock();
    br_gpu_res r = br_dev_bind();
    int flag = 0, oom = 0;
    double kern = 0, t1 = t0, t2 = t0;
    if (r == 0) r = br_gpu_func(st, k);
    if (r == 0 && !br_gpu_reserve(br_gplan_size(&p), &r)) oom = r == BR_GPU_OOM;
    if (r == 0) r = br_gpu_copy_in(&p, ctx);
    t1 = brine_clock();
    if (r == 0) r = br_gpu_run_for(st, done, n, &flag, &kern);
    t2 = brine_clock();
    if (r == 0 && !flag) r = br_gpu_copy_out(&p);
    double t3 = brine_clock();
    uint64_t bytes = br_gplan_bytes(&p);
    br_unlock(&br_gpu.run);
    br_gplan_free(&p);
    if (oom) {
        *why = "too big for the GPU's memory";
        return done;
    }
    if (r) {
        br_gpu_fail("running a kernel", r);
        *why = br_gpu.why;
        return done;
    }
    if (flag) {
        *why = "a check failed on the GPU";
        if (br_gpu.log) br_gpu_logf("%s %s n=%lld: a check failed on the GPU, running it on the CPU", k->where, br_gpu_kind(k), (long long)n);
        return done;
    }
    double copy = (t1 - t0) + (t3 - t2);
    if (bytes > (1 << 20) && copy > 0) {
        br_lock(&br_gpu.mu);
        br_gpu.bw = 0.5 * (br_gpu.bw + (double)bytes / copy);
        br_unlock(&br_gpu.mu);
    }
    if (br_gpu.log)
        br_gpu_logf("%s %s n=%lld -> gpu %.3f ms (kernel %.3f ms, copies %.3f ms, %.1f MB; cpu estimate %.3f ms)", k->where,
                    br_gpu_kind(k), (long long)n, (t3 - t0) * 1e3, kern * 1e3, copy * 1e3, (double)bytes / 1e6, c.cpu_s * 1e3);
    return n;
}

void brine_par_for(brine_kernel *k, void *ctx, int64_t n, brine_chunk_fn fn) {
    if (n <= 0) return;
    const char *why = NULL;
    int64_t done = br_gpu_for(k, ctx, n, fn, &why);
    if (done >= n) return;
    double t0 = brine_clock();
    brine_par_run(done, n, brine_par_plan(n - done), fn, ctx);
    double dt = brine_clock() - t0;
    if (k->ptx && !br_in_par && n - done >= BR_GPU_MIN_N) br_gpu_note_cpu(br_kstate(k), n - done, dt);
    if (br_gpu.log && !br_in_par) br_gpu_log_cpu(k, n, why, dt);
}

/* ------------------------------------------------------------ reductions on the GPU */
/* Folds leaves [l0, l1) on this thread, pushing each at height 0. */
static void br_fold_leaves(br_fold *f, brine_leaf_fn fn, void *ctx, int64_t n, int64_t l0, int64_t l1) {
    _Alignas(16) char v[16];
    int saved = br_in_par;
    br_in_par = 1;
    for (int64_t l = l0; l < l1; l++) {
        int64_t a = l * BRINE_LEAF;
        int64_t b = a + BRINE_LEAF < n ? a + BRINE_LEAF : n;
        bool h = false;
        fn(ctx, l, a, b, v, &h);
        br_fold_push(f, 0, v, h);
    }
    br_in_par = saved;
}

/* One aligned subtree of 2^h leaves starting at leaf l0 (fewer at the end) on
 * the device; its root goes to *v, *has. vals and the buffer after it: room
 * for 2^h values and flags, twice. */
static br_gpu_res br_gpu_subtree(brine_kernel *k, br_kst *st, int64_t n, int64_t l0, int64_t cnt, br_gpu_ptr bufa, br_gpu_ptr bufb,
                                 int64_t room, void *v, bool *has) {
    int64_t sz = br_vk_size[k->vk];
    br_gpu_ptr va = bufa, ha = bufa + (br_gpu_ptr)(room * sz);
    br_gpu_ptr vb = bufb, hb = bufb + (br_gpu_ptr)(room * sz);
    int64_t count = cnt;
    uint64_t args[6] = {br_gpu.buf, (uint64_t)n, (uint64_t)l0, (uint64_t)cnt, va, ha};
    br_gpu_res r = br_dev_run(st->fn, cnt, args, 6);
    while (r == 0 && count > 1) {
        int64_t blocks = (count + BR_GPU_BLOCK - 1) / BR_GPU_BLOCK;
        uint64_t cargs[5] = {va, ha, (uint64_t)count, vb, hb};
        r = br_dev_comb(st->comb, blocks, cargs, 5);
        br_gpu_ptr t = va;
        va = vb;
        vb = t;
        t = ha;
        ha = hb;
        hb = t;
        count = blocks;
    }
    if (r == 0) r = br_dev_dtoh(v, va, (size_t)sz);
    if (r == 0) r = br_dev_dtoh(has, ha, 1);
    return r;
}

/* The GPU part of a reduction: true when the device (with the CPU's first
 * leaves, if it measured them) folded every leaf into f. */
static int br_gpu_reduce(brine_kernel *k, int64_t n, brine_leaf_fn fn, void *ctx, br_fold *f, const char **why, double *cpu_est) {
    if (!k->ptx || br_gpu.mode == BR_GPU_OFF) {
        *why = k->ptx ? "BRINE_GPU=off" : k->why;
        return 0;
    }
    *why = "nested in a parallel construct";
    if (br_in_par) return 0;
    *why = "small";
    if (n < BR_GPU_MIN_N && br_gpu.mode != BR_GPU_ON) return 0;
    br_lock(&br_gpu.mu);
    br_kst *st = br_kstate(k);
    int failed = br_gpu.state == BR_GS_FAILED;
    int known = st && st->cpu_ns > 0;
    br_unlock(&br_gpu.mu);
    *why = failed ? br_gpu.why : "out of memory";
    if (!st || failed) return 0;
    int64_t nleaves = (n + BRINE_LEAF - 1) / BRINE_LEAF;
    /* measure: leaves [0, p), p a power of two, on this thread */
    int64_t p = 0;
    if (!known && br_gpu.mode != BR_GPU_ON) {
        double t0 = brine_clock();
        p = 1;
        br_fold_leaves(f, fn, ctx, n, 0, 1);
        while (p < nleaves && p * 8 < nleaves && brine_clock() - t0 < 1e-4) {
            br_fold_leaves(f, fn, ctx, n, p, 2 * p);
            p *= 2;
        }
        double ns = (brine_clock() - t0) * 1e9 / (double)(p * BRINE_LEAF);
        br_lock(&br_gpu.mu);
        st->seq_ns = ns;
        st->cpu_ns = ns / (br_threads() * BR_CPU_EFF);
        br_unlock(&br_gpu.mu);
    }
    br_gplan pl;
    if (!br_gplan_make(&pl, k, ctx)) {
        br_gplan_free(&pl);
        *why = "out of memory";
        return 0;
    }
    int64_t rest = n - p * BRINE_LEAF;
    br_lock(&br_gpu.mu);
    br_gpu_choice c = br_gpu_choose(k, st, rest, br_gplan_bytes(&pl), 2 + rest / ((int64_t)1 << 22));
    double gpu_ns = st->gpu_ns > 0 ? st->gpu_ns : br_gpu_guess_ns(k, st->seq_ns);
    br_unlock(&br_gpu.mu);
    *cpu_est = c.cpu_s;
    if (!c.use) {
        br_gplan_free(&pl);
        *why = c.why;
        return 0;
    }
    *why = "the GPU is busy";
    if (!br_trylock(&br_gpu.run)) {
        br_gplan_free(&pl);
        return 0;
    }
    /* subtrees of up to 2^h leaves, about BR_GPU_SLICE each */
    int h = 12;
    while (h < 24 && gpu_ns * BRINE_LEAF * (double)((int64_t)1 << (h + 1)) < BR_GPU_SLICE * 1e9) h++;
    int64_t room = (int64_t)1 << h;
    int64_t sz = br_vk_size[k->vk];
    uint64_t half = BR_ALIGN((uint64_t)(room * (sz + 1)), 256);
    uint64_t at = BR_ALIGN(br_gplan_size(&pl), 256);
    double t0 = brine_clock();
    br_gpu_res r = br_dev_bind();
    int flag = 0, oom = 0;
    if (r == 0) r = br_gpu_func(st, k);
    if (r == 0 && !br_gpu_reserve(at + 2 * half, &r)) oom = r == BR_GPU_OOM;
    if (r == 0) r = br_gpu_copy_in(&pl, ctx);
    double t1 = brine_clock();
    int64_t l = p;
    int64_t seg = p > 0 ? p : room;
    _Alignas(16) char v[16];
    while (r == 0 && l < nleaves) {
        if (seg > room) seg = room;
        int64_t cnt = nleaves - l < seg ? nleaves - l : seg;
        bool hv = false;
        r = br_gpu_subtree(k, st, n, l, cnt, br_gpu.buf + at, br_gpu.buf + at + half, room, v, &hv);
        if (r == 0) r = br_gpu_errflag(&flag);
        if (r || flag) break;
        int sh = 0;
        while (((int64_t)1 << sh) < seg) sh++;
        br_fold_push(f, sh, v, hv);
        l += seg;
        /* aligned: the next subtree is as big as everything before it, up to `room` */
        seg = p > 0 && l < room ? l : room;
    }
    double t2 = brine_clock();
    uint64_t bytes = br_gplan_bytes(&pl);
    br_unlock(&br_gpu.run);
    br_gplan_free(&pl);
    if (oom) {
        *why = "too big for the GPU's memory";
        return 0;
    }
    if (r) {
        br_gpu_fail("running a kernel", r);
        *why = br_gpu.why;
        return 0;
    }
    if (flag) {
        *why = "a check failed on the GPU";
        if (br_gpu.log) br_gpu_logf("%s %s n=%lld: a check failed on the GPU, running it on the CPU", k->where, br_gpu_kind(k), (long long)n);
        return 0;
    }
    if (rest > 0) {
        br_lock(&br_gpu.mu);
        st->gpu_ns = (t2 - t1) * 1e9 / (double)rest;
        if (bytes > (1 << 20) && t1 > t0) br_gpu.bw = 0.5 * (br_gpu.bw + (double)bytes / (t1 - t0));
        br_unlock(&br_gpu.mu);
    }
    if (br_gpu.log)
        br_gpu_logf("%s %s n=%lld -> gpu %.3f ms (kernels %.3f ms, copies %.3f ms, %.1f MB; cpu estimate %.3f ms)", k->where,
                    br_gpu_kind(k), (long long)n, (t2 - t0) * 1e3, (t2 - t1) * 1e3, (t1 - t0) * 1e3, (double)bytes / 1e6, c.cpu_s * 1e3);
    return 1;
}

void brine_par_reduce(brine_kernel *k, int64_t n, brine_leaf_fn fn, void *ctx, void *val, bool *has, const char *loc) {
    if (n <= 0) return;
    br_fold f;
    br_fold_init(&f, br_comb_pick(k->op, k->vk), br_vk_size[k->vk], loc);
    const char *why = NULL;
    double est = 0;
    if (br_gpu_reduce(k, n, fn, ctx, &f, &why, &est)) {
        if (br_fold_finish(&f, val)) *has = true;
        return;
    }
    double t0 = brine_clock();
    br_reduce_cpu(n, fn, ctx, k->op, k->vk, val, has, loc);
    double dt = brine_clock() - t0;
    if (k->ptx && !br_in_par && n >= BR_GPU_MIN_N) br_gpu_note_cpu(br_kstate(k), n, dt);
    if (br_gpu.log && !br_in_par) br_gpu_log_cpu(k, n, why, dt);
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
    /* what this rank printed so far comes out before anything another rank
       prints after hearing from it (ranks share stdout, each with its own buffer) */
    br_lock(&br_io_mu);
    fflush(stdout);
    br_unlock(&br_io_mu);
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
    /* when every rank is on this machine, listen on loopback only: nothing
       outside can reach it, and firewalls don't ask about it */
    bool local = true;
    for (int64_t i = 0; i < br_size; i++)
        if (strncmp(hosts[i], "127.", 4) != 0 && strcmp(hosts[i], "localhost") != 0) local = false;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(local ? INADDR_LOOPBACK : INADDR_ANY);
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
    br_heap_init();
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
    br_gpu_setup();
    static char outbuf[1 << 16];
    setvbuf(stdout, outbuf, _IOFBF, sizeof outbuf);
    br_world_init();
}

static void br_procs_free(void);

void brine_rt_shutdown(void) {
    br_procs_free();
    br_flush();
    br_gpu_settle();
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

bool brine_os_write_bytes(brine_str *path, brine_arr *data, brine_str **err) {
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
    size_t put = data->len ? fwrite(data->data, 1, (size_t)data->len, f) : 0;
    int bad = put != (size_t)data->len;
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

/* ------------------------------------------------------------ entry points for native code
 * Natively compiled brine (the x86-64 backend) can't call the `static inline`
 * helpers of brine_rt.h, so each has an ordinary twin here: `brine_<name>` is
 * `brine_x_<name>`. 128-bit integers go by pointer: an i128/u128 argument is a
 * pointer to it, and an i128/u128 result is written through an extra first
 * argument. The `brine_x_i128_*` / `brine_x_u128_*` functions are the plain
 * (wrapping) 128-bit operations the native backend doesn't do inline. */
void brine_x_str_retain(brine_str *s) { brine_str_retain(s); }
void brine_x_str_release(brine_str *s) { brine_str_release(s); }
void brine_x_arr_retain(brine_arr *a) { brine_arr_retain(a); }
void brine_x_arr_release(brine_arr *a) { brine_arr_release(a); }
void brine_x_box_retain(void *p) { brine_box_retain(p); }
bool brine_x_box_dec(void *p) { return brine_box_dec(p); }
void brine_x_map_retain(brine_map *m) { brine_map_retain(m); }
void brine_x_map_release(brine_map *m) { brine_map_release(m); }
brine_str *brine_x_str_at(brine_str *s, int64_t i, const char *loc) { return brine_str_at(s, i, loc); }
brine_str *brine_x_str_byte(brine_str *s, int64_t i) { return brine_str_byte(s, i); }
void brine_x_str_add(brine_str **ps, brine_str *b) { brine_str_add(ps, b); }
bool brine_x_map_has(brine_map *m, const void *k) { return brine_map_has(m, k); }
uint64_t brine_x_hash_u64(uint64_t x) { return brine_hash_u64(x); }
uint64_t brine_x_hash_mix(uint64_t h, uint64_t x) { return brine_hash_mix(h, x); }
uint64_t brine_x_hash_u128(const brine_u128 *x) { return brine_hash_u128(*x); }
int64_t brine_x_idx(int64_t i, int64_t len, const char *loc) { return brine_idx(i, len, loc); }
int64_t brine_x_par_plan(int64_t n) { return brine_par_plan(n); }

#define BR_X_MATH1(name) \
    double brine_x_f_##name(double x) { return brine_f_##name(x); } \
    float brine_x_f_##name##f(float x) { return brine_f_##name##f(x); }
BR_X_MATH1(sqrt)
BR_X_MATH1(sin)
BR_X_MATH1(cos)
BR_X_MATH1(tan)
BR_X_MATH1(exp)
BR_X_MATH1(log)
BR_X_MATH1(floor)
BR_X_MATH1(ceil)
BR_X_MATH1(round)
BR_X_MATH1(fabs)
double brine_x_f_pow(double a, double b) { return brine_f_pow(a, b); }
double brine_x_f_fmod(double a, double b) { return brine_f_fmod(a, b); }
float brine_x_f_fmodf(float a, float b) { return brine_f_fmodf(a, b); }
double brine_x_f_fmin(double a, double b) { return brine_f_fmin(a, b); }
double brine_x_f_fmax(double a, double b) { return brine_f_fmax(a, b); }
float brine_x_f_fminf(float a, float b) { return brine_f_fminf(a, b); }
float brine_x_f_fmaxf(float a, float b) { return brine_f_fmaxf(a, b); }

/* integer helpers for types up to 64 bits */
#define BR_X_COMMON(T, S)                                                                         \
    T brine_x_add_##S(T a, T b, const char *loc) { return brine_add_##S(a, b, loc); }            \
    T brine_x_sub_##S(T a, T b, const char *loc) { return brine_sub_##S(a, b, loc); }            \
    T brine_x_mul_##S(T a, T b, const char *loc) { return brine_mul_##S(a, b, loc); }            \
    T brine_x_shl_##S(T a, int64_t b, const char *loc) { return brine_shl_##S(a, b, loc); }      \
    T brine_x_shr_##S(T a, int64_t b, const char *loc) { return brine_shr_##S(a, b, loc); }      \
    T brine_x_conv_##S##_from_i64(int64_t x, const char *loc) { return brine_conv_##S##_from_i64(x, loc); } \
    T brine_x_conv_##S##_from_u64(uint64_t x, const char *loc) { return brine_conv_##S##_from_u64(x, loc); } \
    T brine_x_conv_##S##_from_i128(const brine_i128 *x, const char *loc) { return brine_conv_##S##_from_i128(*x, loc); } \
    T brine_x_conv_##S##_from_u128(const brine_u128 *x, const char *loc) { return brine_conv_##S##_from_u128(*x, loc); } \
    T brine_x_fdiv_##S(T a, T b) { return brine_fdiv_##S(a, b); }                                \
    T brine_x_fmod_##S(T a, T b) { return brine_fmod_##S(a, b); }                                \
    T brine_x_div_##S(T a, T b, const char *loc) { return brine_div_##S(a, b, loc); }            \
    T brine_x_mod_##S(T a, T b, const char *loc) { return brine_mod_##S(a, b, loc); }            \
    T brine_x_abs_##S(T a, const char *loc) { return brine_abs_##S(a, loc); }                    \
    T brine_x_conv_##S##_from_f64(double x, const char *loc) { return brine_conv_##S##_from_f64(x, loc); }
#define BR_X_SIGNED(T, S) \
    BR_X_COMMON(T, S)     \
    T brine_x_neg_##S(T a, const char *loc) { return brine_neg_##S(a, loc); }
BR_X_SIGNED(int8_t, i8)
BR_X_SIGNED(int16_t, i16)
BR_X_SIGNED(int32_t, i32)
BR_X_SIGNED(int64_t, i64)
BR_X_COMMON(uint8_t, u8)
BR_X_COMMON(uint16_t, u16)
BR_X_COMMON(uint32_t, u32)
BR_X_COMMON(uint64_t, u64)

/* the same for 128-bit integers, by pointer */
#define BR_X_COMMON128(T, S)                                                                        \
    void brine_x_add_##S(T *r, const T *a, const T *b, const char *loc) { *r = brine_add_##S(*a, *b, loc); } \
    void brine_x_sub_##S(T *r, const T *a, const T *b, const char *loc) { *r = brine_sub_##S(*a, *b, loc); } \
    void brine_x_mul_##S(T *r, const T *a, const T *b, const char *loc) { *r = brine_mul_##S(*a, *b, loc); } \
    void brine_x_shl_##S(T *r, const T *a, int64_t b, const char *loc) { *r = brine_shl_##S(*a, b, loc); } \
    void brine_x_shr_##S(T *r, const T *a, int64_t b, const char *loc) { *r = brine_shr_##S(*a, b, loc); } \
    void brine_x_conv_##S##_from_i64(T *r, int64_t x, const char *loc) { *r = brine_conv_##S##_from_i64(x, loc); } \
    void brine_x_conv_##S##_from_u64(T *r, uint64_t x, const char *loc) { *r = brine_conv_##S##_from_u64(x, loc); } \
    void brine_x_conv_##S##_from_i128(T *r, const brine_i128 *x, const char *loc) { *r = brine_conv_##S##_from_i128(*x, loc); } \
    void brine_x_conv_##S##_from_u128(T *r, const brine_u128 *x, const char *loc) { *r = brine_conv_##S##_from_u128(*x, loc); } \
    void brine_x_fdiv_##S(T *r, const T *a, const T *b) { *r = brine_fdiv_##S(*a, *b); }        \
    void brine_x_fmod_##S(T *r, const T *a, const T *b) { *r = brine_fmod_##S(*a, *b); }        \
    void brine_x_div_##S(T *r, const T *a, const T *b, const char *loc) { *r = brine_div_##S(*a, *b, loc); } \
    void brine_x_mod_##S(T *r, const T *a, const T *b, const char *loc) { *r = brine_mod_##S(*a, *b, loc); } \
    void brine_x_abs_##S(T *r, const T *a, const char *loc) { *r = brine_abs_##S(*a, loc); }    \
    void brine_x_conv_##S##_from_f64(T *r, double x, const char *loc) { *r = brine_conv_##S##_from_f64(x, loc); }
BR_X_COMMON128(brine_i128, i128)
BR_X_COMMON128(brine_u128, u128)
void brine_x_neg_i128(brine_i128 *r, const brine_i128 *a, const char *loc) { *r = brine_neg_i128(*a, loc); }

/* runtime functions that take or return 128-bit integers */
void brine_x_parse_i128(brine_i128 *r, brine_str *s, const char *loc) { *r = brine_parse_i128(s, loc); }
void brine_x_parse_u128(brine_u128 *r, brine_str *s, const char *loc) { *r = brine_parse_u128(s, loc); }
void brine_x_sb_i128(brine_sb *sb, const brine_i128 *v) { brine_sb_i128(sb, *v); }
void brine_x_sb_u128(brine_sb *sb, const brine_u128 *v) { brine_sb_u128(sb, *v); }
void brine_x_sb_fmt_i128(brine_sb *sb, const brine_i128 *v, int kind) { brine_sb_fmt_i128(sb, *v, kind); }
void brine_x_sb_fmt_u128(brine_sb *sb, const brine_u128 *v, int kind) { brine_sb_fmt_u128(sb, *v, kind); }

/* plain 128-bit operations (wrapping; division by zero is excluded by the caller) */
#define BR_X_W128(T, S)                                                                             \
    void brine_x_##S##_add(T *r, const T *a, const T *b) { *r = (T)((brine_u128)*a + (brine_u128)*b); } \
    void brine_x_##S##_sub(T *r, const T *a, const T *b) { *r = (T)((brine_u128)*a - (brine_u128)*b); } \
    void brine_x_##S##_mul(T *r, const T *a, const T *b) { *r = (T)((brine_u128)*a * (brine_u128)*b); } \
    void brine_x_##S##_div(T *r, const T *a, const T *b) { *r = *a / *b; }                         \
    void brine_x_##S##_rem(T *r, const T *a, const T *b) { *r = *a % *b; }                         \
    void brine_x_##S##_shl(T *r, const T *a, int64_t b) { *r = (T)((brine_u128)*a << b); }         \
    void brine_x_##S##_shr(T *r, const T *a, int64_t b) { *r = *a >> b; }                          \
    void brine_x_##S##_neg(T *r, const T *a) { *r = (T)(0 - (brine_u128)*a); }                    \
    int brine_x_##S##_cmp(const T *a, const T *b) { return (*a > *b) - (*a < *b); }                \
    void brine_x_##S##_from_i64(T *r, int64_t x) { *r = (T)x; }                                    \
    void brine_x_##S##_from_u64(T *r, uint64_t x) { *r = (T)x; }                                   \
    void brine_x_##S##_from_f64(T *r, double x) { *r = (T)x; }                                     \
    void brine_x_##S##_from_f32(T *r, float x) { *r = (T)x; }                                      \
    double brine_x_##S##_to_f64(const T *a) { return (double)*a; }                                 \
    float brine_x_##S##_to_f32(const T *a) { return (float)*a; }
BR_X_W128(brine_i128, i128)
BR_X_W128(brine_u128, u128)
