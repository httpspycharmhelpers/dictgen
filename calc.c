/*
 * calc.c - arbitrary-precision expression evaluator built on GMP (mpz) and
 * MPFR (mpfr).
 *
 * Grammar (loosest to tightest binding):
 *   expr    := term (('+' | '-') term)*
 *   term    := unary (('*' | '/' | '%') unary  |  implicit-mul unary)*
 *   unary   := ('+' | '-') unary | power
 *   power   := postfix ('^' unary)?            // right associative
 *   postfix := primary ('!')*
 *   primary := number | ident | '(' expr ')' | func '(' args ')'
 *
 * Values are kept exact when possible: numbers, + - * % ^ ! stay in GMP mpz.
 * pi, e, sqrt() and non-exact division promote to MPFR at the requested
 * precision.  Implicit multiplication is supported ("2pi", "2(3+4)", "pi2").
 */
#define _GNU_SOURCE
#include "calc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <gmp.h>
#include <mpfr.h>

typedef struct {
    int      is_real;
    mpz_t    z;
    mpfr_t   r;
} Num;

typedef struct {
    const char *s;
    size_t      pos;
    long        digits;
    mpfr_prec_t prec;
    const char *err;
} Ctx;

/* cached constants */
static mpfr_t g_pi, g_e;
static int    g_pi_ready = 0, g_e_ready = 0;
static mpfr_prec_t g_const_prec = 0;

static mpfr_prec_t digits_to_bits(long digits)
{
    if (digits < 1) digits = 1;
    return (mpfr_prec_t)((double)digits * 3.3219280948873626 + 128.0);
}

/* ---- pi via Chudnovsky binary splitting --------------------------------
 * ~2.2x faster than MPFR's mpfr_const_pi at high precision and equally
 * accurate (|diff| < 2^(prec-8)).  Each term contributes ~14.18 decimal
 * digits, i.e. ~47.1 bits.                                                 */
#define CHUD_A     13591409UL
#define CHUD_B     545140134UL
#define CHUD_C3_24 10939058860032000UL   /* 640320^3 / 24 */

static void chud_bs(mpz_t P, mpz_t Q, mpz_t T, long a, long b)
{
    if (b - a == 1) {
        if (a == 0) { mpz_set_ui(P, 1); mpz_set_ui(Q, 1); mpz_set_ui(T, CHUD_A); return; }
        mpz_t p; mpz_init(p);
        mpz_set_si(p, (long)(6*a - 5));
        mpz_mul_si(p, p, (long)(2*a - 1));
        mpz_mul_si(p, p, (long)(6*a - 1));
        mpz_set(P, p);
        mpz_set_ui(Q, 1);
        mpz_mul_ui(Q, Q, (unsigned long)a * (unsigned long)a * (unsigned long)a);
        mpz_mul_ui(Q, Q, CHUD_C3_24);
        mpz_mul_ui(T, P, CHUD_A + CHUD_B * (unsigned long)a);
        if (a & 1) mpz_neg(T, T);
        mpz_clear(p);
    } else {
        long m = a + (b - a) / 2;
        mpz_t Pl,Ql,Tl,Pr,Qr,Tr,t1,t2;
        mpz_inits(Pl,Ql,Tl,Pr,Qr,Tr,t1,t2,NULL);
        chud_bs(Pl,Ql,Tl,a,m);
        chud_bs(Pr,Qr,Tr,m,b);
        mpz_mul(P, Pl, Pr);
        mpz_mul(Q, Ql, Qr);
        mpz_mul(t1, Qr, Tl);
        mpz_mul(t2, Pl, Tr);
        mpz_add(T, t1, t2);
        mpz_clears(Pl,Ql,Tl,Pr,Qr,Tr,t1,t2,NULL);
    }
}

static void chudnovsky_pi(mpfr_t out, mpfr_prec_t prec)
{
    long n = (long)((double)prec / 47.0) + 4;   /* terms needed for prec bits */
    if (n < 8) n = 8;

    mpz_t P,Q,T; mpz_inits(P,Q,T,NULL);
    chud_bs(P,Q,T,0,n);

    mpfr_t t,s; mpfr_inits2(prec,t,s,NULL);
    mpfr_set_z(t, Q, MPFR_RNDN);
    mpfr_set_z(s, T, MPFR_RNDN);
    mpfr_div(t, t, s, MPFR_RNDN);
    mpfr_sqrt_ui(s, 10005, MPFR_RNDN);
    mpfr_mul(t, t, s, MPFR_RNDN);
    mpfr_mul_ui(t, t, 426880, MPFR_RNDN);
    mpfr_set(out, t, MPFR_RNDN);
    mpfr_clears(t,s,NULL);
    mpz_clears(P,Q,T,NULL);
}

static void num_init(Num *n, Ctx *c) { n->is_real = 0; mpz_init(n->z); mpfr_init2(n->r, c->prec); }
static void num_clear(Num *n)        { mpz_clear(n->z); mpfr_clear(n->r); }

static void to_real(Num *n)
{
    if (!n->is_real) { mpfr_set_z(n->r, n->z, MPFR_RNDN); n->is_real = 1; }
}

static void get_pi(Ctx *c, mpfr_t out)
{
    if (!g_pi_ready || g_const_prec < c->prec) {
        if (g_pi_ready) mpfr_clear(g_pi);
        mpfr_init2(g_pi, c->prec);
        chudnovsky_pi(g_pi, c->prec);
        g_pi_ready = 1;
        g_const_prec = c->prec;
    }
    mpfr_set(out, g_pi, MPFR_RNDN);
}

static void get_e(Ctx *c, mpfr_t out)
{
    if (!g_e_ready || g_const_prec < c->prec) {
        if (g_e_ready) mpfr_clear(g_e);
        mpfr_init2(g_e, c->prec);
        mpfr_set_ui(g_e, 1, MPFR_RNDN);
        mpfr_exp(g_e, g_e, MPFR_RNDN);
        g_e_ready = 1;
    }
    mpfr_set(out, g_e, MPFR_RNDN);
}

/* ---- arithmetic -------------------------------------------------------- */

static void num_add(Num *r, Num *a, Num *b)
{
    if (!a->is_real && !b->is_real) { mpz_add(r->z, a->z, b->z); r->is_real = 0; }
    else { to_real(a); to_real(b); mpfr_add(r->r, a->r, b->r, MPFR_RNDN); r->is_real = 1; }
}
static void num_sub(Num *r, Num *a, Num *b)
{
    if (!a->is_real && !b->is_real) { mpz_sub(r->z, a->z, b->z); r->is_real = 0; }
    else { to_real(a); to_real(b); mpfr_sub(r->r, a->r, b->r, MPFR_RNDN); r->is_real = 1; }
}
static void num_mul(Num *r, Num *a, Num *b)
{
    if (!a->is_real && !b->is_real) { mpz_mul(r->z, a->z, b->z); r->is_real = 0; }
    else { to_real(a); to_real(b); mpfr_mul(r->r, a->r, b->r, MPFR_RNDN); r->is_real = 1; }
}
static void num_div(Num *r, Num *a, Num *b, Ctx *c)
{
    if (!a->is_real && !b->is_real) {
        if (mpz_sgn(b->z) == 0) { if (!c->err) c->err = "除以零"; mpz_set_ui(r->z, 0); r->is_real = 0; return; }
        mpz_t q, rem; mpz_init(q); mpz_init(rem);
        mpz_tdiv_qr(q, rem, a->z, b->z);
        if (mpz_sgn(rem) == 0) { mpz_set(r->z, q); r->is_real = 0; }
        else { to_real(a); to_real(b); mpfr_div(r->r, a->r, b->r, MPFR_RNDN); r->is_real = 1; }
        mpz_clear(q); mpz_clear(rem);
    } else {
        to_real(a); to_real(b); mpfr_div(r->r, a->r, b->r, MPFR_RNDN); r->is_real = 1;
    }
}
static void num_mod(Num *r, Num *a, Num *b, Ctx *c)
{
    if (!a->is_real && !b->is_real) {
        if (mpz_sgn(b->z) == 0) { if (!c->err) c->err = "对零取模"; mpz_set_ui(r->z, 0); r->is_real = 0; return; }
        mpz_tdiv_r(r->z, a->z, b->z); r->is_real = 0;
    } else {
        to_real(a); to_real(b); mpfr_fmod(r->r, a->r, b->r, MPFR_RNDN); r->is_real = 1;
    }
}
static void num_pow(Num *r, Num *a, Num *b, Ctx *c)
{
    if (!a->is_real && !b->is_real && mpz_sgn(b->z) >= 0 && mpz_fits_ulong_p(b->z)) {
        mpz_pow_ui(r->z, a->z, mpz_get_ui(b->z)); r->is_real = 0;
    } else {
        to_real(a); to_real(b);
        if (mpfr_sgn(a->r) < 0 && !mpfr_integer_p(b->r)) { if (!c->err) c->err = "负数的非整数次幂"; }
        mpfr_pow(r->r, a->r, b->r, MPFR_RNDN); r->is_real = 1;
    }
}
static void num_fact(Num *r, Num *a, Ctx *c)
{
    if (a->is_real) {
        if (!mpfr_integer_p(a->r)) { if (!c->err) c->err = "阶乘只接受整数"; return; }
        if (mpfr_sgn(a->r) < 0) { if (!c->err) c->err = "阶乘只接受非负整数"; return; }
        mpfr_get_z(r->z, a->r, MPFR_RNDN);
    } else {
        if (mpz_sgn(a->z) < 0) { if (!c->err) c->err = "阶乘只接受非负整数"; return; }
        mpz_set(r->z, a->z);
    }
    if (!mpz_fits_ulong_p(r->z)) { if (!c->err) c->err = "阶乘参数过大（上限约 2^64）"; return; }
    mpz_fac_ui(r->z, mpz_get_ui(r->z));
    r->is_real = 0;
}

/* ---- parser ------------------------------------------------------------ */

static void skip_ws(Ctx *c) { while (isspace((unsigned char)c->s[c->pos])) c->pos++; }

static int peek(Ctx *c) { skip_ws(c); return (unsigned char)c->s[c->pos]; }

static Num parse_expr(Ctx *c);
static Num parse_unary(Ctx *c);

static Num parse_primary(Ctx *c)
{
    Num v; num_init(&v, c);
    int ch = peek(c);
    if (ch == '(') {
        c->pos++;
        Num inner = parse_expr(c);
        if (peek(c) == ')') c->pos++;
        else if (!c->err) c->err = "缺少右括号 )";
        num_clear(&v);
        return inner;
    }
    if (isdigit(ch)) {
        size_t start = c->pos;
        while (isdigit((unsigned char)c->s[c->pos])) c->pos++;
        int has_dot = 0;
        if (c->s[c->pos] == '.') {
            has_dot = 1;
            c->pos++;
            while (isdigit((unsigned char)c->s[c->pos])) c->pos++;
        }
        size_t len = c->pos - start;
        char *tmp = malloc(len + 1);
        memcpy(tmp, c->s + start, len);
        tmp[len] = '\0';
        if (has_dot) {
            if (mpfr_set_str(v.r, tmp, 10, MPFR_RNDN) != 0 && !c->err)
                c->err = "无法解析小数";
            v.is_real = 1;
        } else {
            mpz_set_str(v.z, tmp, 10);
            v.is_real = 0;
        }
        free(tmp);
        return v;
    }
    if (isalpha(ch) || ch == '_') {
        size_t start = c->pos;
        while (isalnum((unsigned char)c->s[c->pos]) || c->s[c->pos] == '_') c->pos++;
        size_t len = c->pos - start;
        char name[64];
        if (len >= sizeof(name)) { c->err = "未知标识符"; return v; }
        memcpy(name, c->s + start, len); name[len] = '\0';

        if (strcmp(name, "pi") == 0) { get_pi(c, v.r); v.is_real = 1; return v; }
        if (strcmp(name, "e") == 0)  { get_e(c, v.r);  v.is_real = 1; return v; }

        /* function call */
        if (peek(c) == '(') c->pos++;
        else { c->err = "未知标识符（函数需要括号）"; return v; }

        Num a; num_init(&a, c);
        if (strcmp(name, "sqrt") == 0) {
            a = parse_expr(c);
            if (peek(c) == ')') c->pos++;
            to_real(&a);
            if (mpfr_sgn(a.r) < 0 && !c->err) c->err = "对负数开方";
            mpfr_sqrt(v.r, a.r, MPFR_RNDN); v.is_real = 1;
            num_clear(&a); return v;
        }
        if (strcmp(name, "abs") == 0) {
            a = parse_expr(c);
            if (peek(c) == ')') c->pos++;
            if (!a.is_real) { mpz_abs(v.z, a.z); v.is_real = 0; }
            else { mpfr_abs(v.r, a.r, MPFR_RNDN); v.is_real = 1; }
            num_clear(&a); return v;
        }
        if (strcmp(name, "fact") == 0) {
            a = parse_expr(c);
            if (peek(c) == ')') c->pos++;
            num_fact(&v, &a, c);
            num_clear(&a); return v;
        }
        if (strcmp(name, "pow") == 0) {
            a = parse_expr(c);
            if (peek(c) == ',') c->pos++;
            Num b; num_init(&b, c);
            b = parse_expr(c);
            if (peek(c) == ')') c->pos++;
            num_pow(&v, &a, &b, c);
            num_clear(&a); num_clear(&b); return v;
        }
        if (strcmp(name, "digit") == 0) {
            a = parse_expr(c);
            if (peek(c) == ',') c->pos++;
            Num b; num_init(&b, c);
            b = parse_expr(c);
            if (peek(c) == ')') c->pos++;
            if (b.is_real || mpz_sgn(b.z) < 0 || !mpz_fits_ulong_p(b.z)) {
                if (!c->err) c->err = "digit 的位数必须是非负整数";
                v.is_real = 0; mpz_set_ui(v.z, 0);
                num_clear(&a); num_clear(&b); return v;
            }
            unsigned long k = mpz_get_ui(b.z);
            if ((long)k > c->digits) {
                if (!c->err) c->err = "digit() 需要 --digits ≥ 指定的位数";
                num_clear(&a); num_clear(&b); return v;
            }
            to_real(&a);
            mpfr_t t, ten;
            mpfr_init2(t, c->prec); mpfr_init2(ten, c->prec);
            mpfr_set_ui(ten, 10, MPFR_RNDN);
            mpfr_pow_ui(t, ten, k, MPFR_RNDN);     /* 10^k */
            mpfr_mul(t, a.r, t, MPFR_RNDN);
            mpfr_floor(t, t);
            mpfr_fmod(t, t, ten, MPFR_RNDN);
            long d = (long)mpfr_get_si(t, MPFR_RNDN);
            mpz_set_si(v.z, d); v.is_real = 0;
            mpfr_clear(t); mpfr_clear(ten);
            num_clear(&a); num_clear(&b); return v;
        }
        c->err = "未知函数";
        num_clear(&a);
        return v;
    }
    c->err = "表达式语法错误";
    return v;
}

static Num parse_postfix(Ctx *c)
{
    Num v = parse_primary(c);
    while (peek(c) == '!') {
        c->pos++;
        Num r; num_init(&r, c);
        num_fact(&r, &v, c);
        num_clear(&v);
        v = r;
    }
    return v;
}

static Num parse_power(Ctx *c)
{
    Num v = parse_postfix(c);
    if (peek(c) == '^') {
        c->pos++;
        Num e = parse_unary(c);
        Num r; num_init(&r, c);
        num_pow(&r, &v, &e, c);
        num_clear(&v); num_clear(&e);
        v = r;
    }
    return v;
}

static Num parse_unary(Ctx *c)
{
    int ch = peek(c);
    if (ch == '+' ) { c->pos++; return parse_unary(c); }
    if (ch == '-') {
        c->pos++;
        Num v = parse_unary(c);
        if (!v.is_real) mpz_neg(v.z, v.z);
        else mpfr_neg(v.r, v.r, MPFR_RNDN);
        return v;
    }
    return parse_power(c);
}

static int starts_primary(int ch)
{
    return ch == '(' || isdigit(ch) || isalpha(ch) || ch == '_';
}

static Num parse_term(Ctx *c)
{
    Num v = parse_unary(c);
    for (;;) {
        int ch = peek(c);
        if (ch == '*' || ch == '/' || ch == '%') {
            c->pos++;
            Num b = parse_unary(c);
            Num r; num_init(&r, c);
            if (ch == '*') num_mul(&r, &v, &b);
            else if (ch == '/') num_div(&r, &v, &b, c);
            else num_mod(&r, &v, &b, c);
            num_clear(&v); num_clear(&b);
            v = r;
        } else if (starts_primary(ch)) {
            /* implicit multiplication: 2pi, 2(3), (1+2)(3) */
            Num b = parse_unary(c);
            Num r; num_init(&r, c);
            num_mul(&r, &v, &b);
            num_clear(&v); num_clear(&b);
            v = r;
        } else {
            break;
        }
    }
    return v;
}

static Num parse_expr(Ctx *c)
{
    Num v = parse_term(c);
    for (;;) {
        int ch = peek(c);
        if (ch == '+' || ch == '-') {
            c->pos++;
            Num b = parse_term(c);
            Num r; num_init(&r, c);
            if (ch == '+') num_add(&r, &v, &b);
            else num_sub(&r, &v, &b);
            num_clear(&v); num_clear(&b);
            v = r;
        } else {
            break;
        }
    }
    return v;
}

/* ---- output formatting ------------------------------------------------- */

static char *real_to_string(const mpfr_t v, long digits)
{
    if (mpfr_zero_p(v)) return strdup("0");

    mpfr_exp_t e = 0;
    char *s = mpfr_get_str(NULL, &e, 10, (size_t)digits, v, MPFR_RNDN);
    if (!s) return NULL;

    int neg = (s[0] == '-');
    char *dig = s + neg;
    size_t nd = strlen(dig);

    /* value = 0.<dig> * 10^e */
    size_t cap = nd + (size_t)(e > 0 ? e : 0) + (size_t)(e < 0 ? -e : 0) + 8;
    char *out = malloc(cap);
    if (!out) { mpfr_free_str(s); return NULL; }
    size_t p = 0;
    if (neg) out[p++] = '-';

    long epos = (long)e;              /* position of the decimal point */
    if (epos <= 0) {
        out[p++] = '0';
        out[p++] = '.';
        for (long i = 0; i < -epos; i++) out[p++] = '0';
        for (size_t i = 0; i < nd; i++) out[p++] = dig[i];
    } else if ((size_t)epos >= nd) {
        for (size_t i = 0; i < nd; i++) out[p++] = dig[i];
        for (long i = 0; i < epos - (long)nd; i++) out[p++] = '0';
        /* integer, no fraction */
    } else {
        for (long i = 0; i < epos; i++) out[p++] = dig[i];
        out[p++] = '.';
        for (size_t i = (size_t)epos; i < nd; i++) out[p++] = dig[i];
    }
    out[p] = '\0';

    /* trim trailing zeros in the fractional part */
    char *dot = strchr(out, '.');
    if (dot) {
        char *end = out + strlen(out) - 1;
        while (end > dot && *end == '0') *end-- = '\0';
        if (end == dot) *end = '\0';   /* "12." -> "12" */
    }
    mpfr_free_str(s);
    return out;
}

char *calc_run(const char *expr, long digits, const char **errmsg)
{
    if (errmsg) *errmsg = NULL;
    if (!expr || !*expr) { if (errmsg) *errmsg = "空表达式"; return NULL; }
    if (digits < 1) digits = 1;

    Ctx c;
    c.s = expr; c.pos = 0; c.digits = digits;
    c.prec = digits_to_bits(digits);
    c.err = NULL;

    Num v = parse_expr(&c);
    skip_ws(&c);
    if (!c.err && c.s[c.pos] != '\0') c.err = "表达式尾部有多余字符";

    if (c.err) {
        num_clear(&v);
        if (errmsg) *errmsg = c.err;
        return NULL;
    }

    char *result;
    if (!v.is_real) {
        result = mpz_get_str(NULL, 10, v.z);
    } else {
        result = real_to_string(v.r, digits);
    }
    num_clear(&v);
    if (!result && errmsg) *errmsg = "内存不足";
    return result;
}
