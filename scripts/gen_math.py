# Builds runtime/brine_math.c from musl's libm sources.
#   python scripts/gen_math.py <musl-1.2.5>/src/math runtime/brine_math.c
import re, sys

src, out = sys.argv[1], sys.argv[2]

PUBLIC = ["exp", "exp2", "log", "log2", "pow", "sin", "cos", "tan", "atan", "atan2", "asin", "acos",
          "expm1", "sinh", "cosh", "tanh", "log10", "log1p", "hypot", "cbrt"]
# helpers that stay private to the file
PRIVATE_FNS = ["scalbn"]
INTERNAL = {
    "__sin": "br_m_ksin", "__cos": "br_m_kcos", "__tan": "br_m_ktan",
    "__rem_pio2": "br_m_rem_pio2", "__rem_pio2_large": "br_m_rem_pio2_large",
    "__expo2": "br_m_expo2",
    "__math_oflow": "br_m_oflow", "__math_uflow": "br_m_uflow", "__math_xflow": "br_m_xflow",
    "__math_divzero": "br_m_divzero", "__math_invalid": "br_m_invalid",
    "__exp_data": "br_m_exp_data", "__log_data": "br_m_log_data", "__log2_data": "br_m_log2_data",
    "__pow_log_data": "br_m_pow_log_data",
}

GROUPS = [
    ["__math_xflow.c", "__math_oflow.c", "__math_uflow.c", "__math_divzero.c", "__math_invalid.c", "scalbn.c"],
    ["exp_data.h", "exp_data.c", "exp.c", "exp2.c"],
    ["log_data.h", "log_data.c", "log.c"],
    ["log2_data.h", "log2_data.c", "log2.c"],
    ["pow_data.h", "pow_data.c", "pow.c"],
    ["__rem_pio2_large.c", "__rem_pio2.c", "__sin.c", "__cos.c", "__tan.c", "sin.c", "cos.c", "tan.c"],
    ["atan.c", "atan2.c", "asin.c", "acos.c"],
    ["expm1.c", "__expo2.c", "sinh.c", "cosh.c", "tanh.c"],
    ["log10.c", "log1p.c", "hypot.c", "cbrt.c"],
]

# file-scope names that more than one file defines: renamed per file
static_re = re.compile(r"^static\s+(?:const\s+)?(?:inline\s+)?[\w\s\*]*?\b(\w+)\s*(?:\(|\[|=|,|;)", re.M)
multi_re = re.compile(r"^static\s+const\s+(?:double|float|int|uint32_t|uint64_t)\b([^;]*);", re.M | re.S)

def file_statics(text):
    names = set(m.group(1) for m in static_re.finditer(text))
    for m in multi_re.finditer(text):
        body = re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.S)
        for part in body.split(","):
            mm = re.match(r"\s*\**\s*(\w+)", part)
            if mm and "=" in part or (mm and part.strip().startswith(mm.group(1))):
                names.add(mm.group(1))
    names = {n for n in names if re.match(r"[A-Za-z_]\w*$", n)}
    names -= {"double", "float", "int", "const", "inline", "uint32_t", "uint64_t", "int32_t"}
    return names

def strip_includes(t):
    return re.sub(r'^#\s*include\s+[<"][^>"]+[>"]\s*$', "", t, flags=re.M)

parts = []
for group in GROUPS:
    for f in group:
        t = open(f"{src}/{f}", encoding="utf-8").read()
        t = strip_includes(t).replace("__FP_FAST_FMA", "BR_M_FMA")
        name = f.replace(".", "_").lstrip("_")
        if f.endswith(".h"):
            # `extern hidden const struct x {...} __x;` -> the struct type only
            t = re.sub(r"extern\s+hidden\s+const\s+struct\s+(\w+)\s*\{(.*?)\}\s*\w+\s*;",
                       lambda m: "struct %s {%s};" % (m.group(1), m.group(2)), t, flags=re.S)
            t = re.sub(r"^#ifndef _\w+_H\s*$|^#define _\w+_H\s*$", "", t, flags=re.M)
            i = t.rindex("#endif")
            t = t[:i] + t[i + len("#endif"):]
            parts.append(f"/* ---- {f} */\n{t.strip()}\n")
            continue
        # data definitions become private
        t = re.sub(r"^const struct (\w+) (__\w+)\s*=", r"static const struct \1 \2 =", t, flags=re.M)
        t = t.replace("__FP_FAST_FMA", "BR_M_FMA")
        defines = re.findall(r"^#\s*define\s+(\w+)", t, flags=re.M)
        statics = sorted(file_statics(t) - set(INTERNAL) - set(PUBLIC) - set(PRIVATE_FNS))
        pre = "".join(f"#define {s} br_m_{name}_{s}\n" for s in statics)
        post = "".join(f"#undef {d}\n" for d in sorted(set(defines) | set(statics)))
        parts.append(f"/* ---- {f} */\n{pre}{t.strip()}\n{post}")

body = "\n".join(parts)
# `hidden` functions are private to this file
body = re.sub(r"^hidden\s+", "static ", body, flags=re.M)
body = re.sub(r"^(double|float|int)\s+(__\w+)\s*\(", r"static \1 \2(", body, flags=re.M)
for f in PRIVATE_FNS:
    body = re.sub(r"^double %s\(" % f, "static double %s(" % f, body, flags=re.M)

renames = "".join(f"#undef {p}\n#define {p} brine_m_{p}\n" for p in PUBLIC)
renames += "".join(f"#define {k} {v}\n" for k, v in INTERNAL.items())
renames += "".join(f"#define {p} br_m_{p}\n" for p in PRIVATE_FNS)
unrenames = "".join(f"#undef {p}\n" for p in PUBLIC + list(INTERNAL) + PRIVATE_FNS)

lic = open(f"{src}/../../COPYRIGHT", encoding="utf-8").read()
lic_head = lic[: lic.index("Authors/contributors include:")].strip()

header = f"""/* brine runtime — the math library: one implementation of exp, log, pow,
 * sin, cos, tan and friends, compiled the same way for every target, so a
 * program computes the same bits natively and under WebAssembly (C
 * libraries differ in the last bit). Included by brine_rt.c; the public
 * functions are brine_m_<name> (brine_rt.h). Generated from musl 1.2.5
 * (src/math) with only mechanical changes: names, and the code paths that
 * don't use fused multiply-adds, which not every CPU has.
 *
 * musl's copyright notice (its COPYRIGHT file; the individual files keep
 * theirs, the Sun/FreeBSD and Arm ones among them):
 *
{chr(10).join((' * ' + l).rstrip() for l in lic_head.splitlines())}
 */

/* no fused multiply-adds, whatever the compiler, flags and CPU offer */
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#elif defined(__GNUC__)
#pragma GCC push_options
#pragma GCC optimize("fp-contract=off")
#endif
#define BR_M_FMA 0
#define WANT_ROUNDING 1
#define TOINT_INTRINSICS 0
#define issignaling_inline(x) 0
#define double_t double
#define float_t float
#define predict_true(x) __builtin_expect(!!(x), 1)
#define predict_false(x) __builtin_expect(x, 0)
static inline double eval_as_double(double x) {{
    double y = x;
    return y;
}}
static inline double fp_barrier(double x) {{
    volatile double y = x;
    return y;
}}
static inline void fp_force_eval(double x) {{
    volatile double y;
    y = x;
}}
static inline void fp_force_evalf(float x) {{
    volatile float y;
    y = x;
}}
#define FORCE_EVAL(x)                                                   \\
    do {{                                                                \\
        if (sizeof(x) == sizeof(float)) fp_force_evalf(x);              \\
        else fp_force_eval(x);                                          \\
    }} while (0)
#define asuint(f) ((union {{ float _f; uint32_t _i; }}){{f}})._i
#define asfloat(i) ((union {{ uint32_t _i; float _f; }}){{i}})._f
#define asuint64(f) ((union {{ double _f; uint64_t _i; }}){{f}})._i
#define asdouble(i) ((union {{ uint64_t _i; double _f; }}){{i}})._f
#define EXTRACT_WORDS(hi, lo, d)        \\
    do {{                                \\
        uint64_t __u = asuint64(d);     \\
        (hi) = __u >> 32;               \\
        (lo) = (uint32_t)__u;           \\
    }} while (0)
#define GET_HIGH_WORD(hi, d) do {{ (hi) = asuint64(d) >> 32; }} while (0)
#define GET_LOW_WORD(lo, d) do {{ (lo) = (uint32_t)asuint64(d); }} while (0)
#define INSERT_WORDS(d, hi, lo) do {{ (d) = asdouble(((uint64_t)(hi) << 32) | (uint32_t)(lo)); }} while (0)
#define SET_HIGH_WORD(d, hi) INSERT_WORDS(d, hi, (uint32_t)asuint64(d))
#define SET_LOW_WORD(d, lo) INSERT_WORDS(d, asuint64(d) >> 32, lo)
#define GET_FLOAT_WORD(w, d) do {{ (w) = asuint(d); }} while (0)
#define SET_FLOAT_WORD(d, w) do {{ (d) = asfloat(w); }} while (0)

{renames}
static double br_m_xflow(uint32_t, double);
static double br_m_oflow(uint32_t);
static double br_m_uflow(uint32_t);
static double br_m_divzero(uint32_t);
static double br_m_invalid(double);
static double br_m_scalbn(double, int);
static int br_m_rem_pio2_large(double *, double *, int, int, int);
static int br_m_rem_pio2(double, double *);
static double br_m_ksin(double, double, int);
static double br_m_kcos(double, double);
static double br_m_ktan(double, double, int);
static double br_m_expo2(double, double);

"""

footer = f"""
{unrenames}#if !defined(__clang__) && defined(__GNUC__)
#pragma GCC pop_options
#endif
#undef BR_M_FMA
#undef WANT_ROUNDING
#undef TOINT_INTRINSICS
#undef issignaling_inline
#undef double_t
#undef float_t
#undef predict_true
#undef predict_false
#undef FORCE_EVAL
#undef asuint
#undef asfloat
#undef asuint64
#undef asdouble
#undef EXTRACT_WORDS
#undef GET_HIGH_WORD
#undef GET_LOW_WORD
#undef INSERT_WORDS
#undef SET_HIGH_WORD
#undef SET_LOW_WORD
#undef GET_FLOAT_WORD
#undef SET_FLOAT_WORD
"""

open(out, "w", encoding="utf-8", newline="\n").write(header + body + footer)
