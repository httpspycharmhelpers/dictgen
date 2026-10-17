/*
 * dictgen - dictionary / password generator + arbitrary-precision calculator
 *
 *   dictgen [GENERATION OPTIONS]            generate a wordlist (fast)
 *   dictgen calc [OPTIONS] "EXPRESSION"     evaluate a big-number expression
 *   dictgen pi [N]                          pi to N decimal digits
 *
 * Generation is streaming and single-pass; the odometer uses an index array
 * (no strchr per character) and output is buffered in large blocks, so
 * hundreds of millions of entries are produced at memory/IO bandwidth.
 *
 * Calculations use GMP (exact big integers) and MPFR (arbitrary-precision
 * reals) - e.g. 2^1000000, 100000!, pi to a million digits, k*pi, pi^k.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <time.h>
#include <limits.h>
#include <errno.h>
#include <ctype.h>

#include "out.h"
#include "calc.h"
#include <gmp.h>
#include <mpfr.h>

#define VERSION "2.0.0"
#define DEFAULT_OUTPUT "passwords.txt"
#define MAX_LINE 4096
#define OUT_BLOCK (1u << 20)   /* 1 MiB line-staging buffer */

static const char *g_prog = "dictgen";

typedef enum { FORMAT_NONE, FORMAT_SPACE, FORMAT_ESCAPE } FormatType;

typedef struct {
    char mode[16];
    int min_len;
    int max_len;
    int fixed_len;
    char charset[256];
    char prefix[64];
    char suffix[64];
    char output_file[512];
    unsigned long long limit;
    int random_mode;
    unsigned long long random_count;
    char dict_file[512];
    char separator[8];
    int verbose;
    FormatType format;
    int ascii_names;
    char map_file[512];
    int to_stdout;
    int output_set;
    long calc_digits;
    char expr[8192];
} Config;

static int parse_args(int argc, char *argv[]);

/* Number of following arguments consumed by an option-looking token, or -1 if
 * the token is not a known option (in which case it is the start of the
 * expression, e.g. "-5" or "-5+1"). */
static int opt_arity(const char *a)
{
    if (a[0] != '-' || a[1] == '\0') return -1;
    if (a[1] == '-') {                                   /* long option */
        if (strchr(a, '=')) return 0;                    /* --opt=val */
        static const char *with_val[] = {
            "--output","--min","--max","--length","--charset","--prefix","--suffix",
            "--limit","--count","--mode","--dict","--separator","--precision","--places",
            "--format","--map-file", NULL };
        for (int i = 0; with_val[i]; i++) if (strcmp(a, with_val[i]) == 0) return 1;
        static const char *flags[] = {
            "--help","--verbose","--version","--stdout","--random","--lowercase",
            "--uppercase","--digits","--numbers","--symbols","--binary","--hex","--octal",
            "--ascii-names", NULL };
        for (int i = 0; flags[i]; i++) if (strcmp(a, flags[i]) == 0) return 0;
        return -1;
    }
    if (strcmp(a, "-tff") == 0) return 0;
    switch (a[1]) {                                      /* short options */
        case 'o': case 'm': case 'M': case 'l': case 'c': case 'p': case 's':
        case 'n': case 'R': case 'a': case 'd': case 'S': case 'D': case 'F': case 'f':
            return a[2] == '\0' ? 1 : 0;                 /* -oFILE attached => 0 */
        case 'h': case 'v': case 't': case 'r':
            return 0;
        default:
            return -1;
    }
}

/* Index of the first argument (>= 2) that begins the expression, i.e. after
 * all recognised options and their values. */
static int find_expr_start(int argc, char **argv)
{
    int i = 2;
    while (i < argc) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0) { i++; break; }
        if (a[0] != '-' || a[1] == '\0') break;
        int ar = opt_arity(a);
        if (ar < 0) break;
        i += 1 + ar;
    }
    return i;
}

/* Parse the options of a subcommand while leaving the expression untouched.
 * Returns 0 on success and stores the expression-start index in *expr_start. */
static int parse_calc_args(int argc, char **argv, int *expr_start)
{
    int es = find_expr_start(argc, argv);
    int on = 1 + (es - 2);                 /* argv[0] + options */
    char **oa = malloc(sizeof(char *) * (size_t)(on + 1));
    if (!oa) { fprintf(stderr, "内存不足\n"); return 1; }
    oa[0] = argv[0];
    for (int j = 2; j < es; j++) oa[j - 1] = argv[j];
    oa[on] = NULL;
    int rc = parse_args(on, oa);
    free(oa);
    *expr_start = es;
    return rc;
}

/* In calc/pi mode "--digits N" means the output precision.  Rewrite it to
 * "--precision" so the same token keeps working as the charset flag for
 * generation.  Returns a malloc'd argv-style array (argv[0] preserved). */
static char **map_calc_argv(int argc, char **argv)
{
    char **a = malloc(sizeof(char *) * (size_t)(argc + 1));
    if (!a) return NULL;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--digits") == 0) {
            a[i] = (char *)"--precision";
        } else if (strncmp(argv[i], "--digits=", 9) == 0) {
            size_t n = strlen(argv[i]) + 4;
            char *s = malloc(n);
            if (s) snprintf(s, n, "--precision=%s", argv[i] + 9);
            a[i] = s ? s : argv[i];
        } else {
            a[i] = argv[i];
        }
    }
    a[argc] = NULL;
    return a;
}

static Config cfg = {
    .mode = "password",
    .min_len = 1,
    .max_len = 8,
    .fixed_len = 0,
    .charset = "",
    .prefix = "",
    .suffix = "",
    .output_file = DEFAULT_OUTPUT,
    .limit = 0,
    .random_mode = 0,
    .random_count = 0,
    .dict_file = "",
    .separator = "",
    .verbose = 0,
    .format = FORMAT_NONE,
    .ascii_names = 0,
    .map_file = "",
    .to_stdout = 0,
    .output_set = 0,
    .calc_digits = 50,
    .expr = "",
};

#define LOWERCASE "abcdefghijklmnopqrstuvwxyz"
#define UPPERCASE "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
#define DIGITS "0123456789"
#define SYMBOLS "!@#$%^&*()_+-=[]{};:'\"\\|,./<>?"

static const char *ascii_name(unsigned int val)
{
    static const char *names[256] = {
        "NUL","SOH","STX","ETX","EOT","ENQ","ACK","BEL",
        "BS","TAB","LF","VT","FF","CR","SO","SI",
        "DLE","DC1","DC2","DC3","DC4","NAK","SYN","ETB",
        "CAN","EM","SUB","ESC","FS","GS","RS","US",
        "SP","!","\"","#","$","%","&","'",
        "(",")","*","+",",","-",".","/",
        "0","1","2","3","4","5","6","7",
        "8","9",":",";","<","=",">","?",
        "@","A","B","C","D","E","F","G",
        "H","I","J","K","L","M","N","O",
        "P","Q","R","S","T","U","V","W",
        "X","Y","Z","[","\\","]","^","_",
        "`","a","b","c","d","e","f","g",
        "h","i","j","k","l","m","n","o",
        "p","q","r","s","t","u","v","w",
        "x","y","z","{","|","}","~","DEL"
    };
    return val < 256 ? names[val] : "???";
}

static void print_help(const char *prog);
static void print_version(void);
static int  parse_args(int argc, char *argv[]);
static void validate_config(void);

/* ----------------------------------------------------------------------- */
/* generation                                                              */
/* ----------------------------------------------------------------------- */

static void safe_strcat(char *dest, size_t size, const char *src)
{
    size_t dl = strlen(dest);
    if (dl >= size - 1) return;
    strncat(dest, src, size - dl - 1);
}

/* Apply ascii-name mapping (binary -> control-char name).  Returns 1 if the
 * value was remapped, else 0 (caller keeps the raw text). */
static int map_binary_name(const char *pwd, int len, char *out, size_t outsz)
{
    if (!cfg.ascii_names || len > 8) return 0;
    unsigned int val = 0;
    for (int i = 0; i < len; i++) {
        if (pwd[i] != '0' && pwd[i] != '1') return 0;
        val = (val << 1) | (unsigned)(pwd[i] - '0');
    }
    snprintf(out, outsz, "%s", ascii_name(val));
    return 1;
}

/* Append one generated item to the staging buffer, followed by '\n'. */
static void emit(char *buf, size_t *len, const char *pwd, int pwlen)
{
    /* fast path: plain output, no prefix/suffix/ascii mapping */
    if (cfg.format == FORMAT_NONE && !cfg.prefix[0] && !cfg.suffix[0] &&
        !cfg.ascii_names) {
        memcpy(buf + *len, pwd, (size_t)pwlen);
        *len += (size_t)pwlen;
        buf[(*len)++] = '\n';
        return;
    }

    char mapped[64];
    char *body = (char *)pwd;
    int bodylen = pwlen;

    if (cfg.ascii_names && map_binary_name(pwd, pwlen, mapped, sizeof(mapped))) {
        body = mapped;
        bodylen = (int)strlen(mapped);
    }

    const char *pre = cfg.prefix, *suf = cfg.suffix;
    memcpy(buf + *len, pre, strlen(pre)); *len += strlen(pre);

    switch (cfg.format) {
        case FORMAT_SPACE:
            for (int i = 0; i < bodylen; i++) {
                if (i) buf[(*len)++] = ' ';
                buf[(*len)++] = body[i];
            }
            break;
        case FORMAT_ESCAPE:
            for (int i = 0; i < bodylen; i++) {
                static const char hex[] = "0123456789ABCDEF";
                unsigned char cv = (unsigned char)body[i];
                buf[(*len)++] = '\\'; buf[(*len)++] = 'x';
                buf[(*len)++] = hex[cv >> 4]; buf[(*len)++] = hex[cv & 15];
            }
            break;
        default:
            memcpy(buf + *len, body, bodylen); *len += bodylen;
            break;
    }

    memcpy(buf + *len, suf, strlen(suf)); *len += strlen(suf);
    buf[(*len)++] = '\n';
}

static void generate_passwords(void)
{
    Out *o = out_open(cfg.output_file, cfg.to_stdout);
    if (!o) { fprintf(stderr, "无法打开输出: %s: %s\n", cfg.output_file, strerror(errno)); exit(1); }

    const char *cs = cfg.charset;
    int cslen = (int)strlen(cs);
    char z = cs[0];
    int fast = (cfg.format == FORMAT_NONE && !cfg.prefix[0] && !cfg.suffix[0] &&
                !cfg.ascii_names);
    unsigned long long limit = cfg.limit;
    int has_limit = limit != 0;
    unsigned long long total = 0;

    /* one item can expand to 4 bytes/char (escape) plus prefix/suffix/newline */
    size_t itemmax = (size_t)cfg.max_len * 4 + strlen(cfg.prefix) + strlen(cfg.suffix) + 16;
    size_t bufsz = OUT_BLOCK + itemmax;
    char *buf = malloc(bufsz);
    if (!buf) { perror("malloc"); exit(1); }

    for (int len = cfg.min_len; len <= cfg.max_len && !out_error(o); len++) {
        char *pwd = malloc((size_t)len + 1);
        int *idx = malloc((size_t)len * sizeof(int));
        if (!pwd || !idx) { perror("malloc"); exit(1); }
        for (int i = 0; i < len; i++) { idx[i] = 0; pwd[i] = z; }
        pwd[len] = '\0';

        if (cfg.verbose) printf("长度 %d: 开始生成\n", len);

        size_t blen = 0;
        int done = 0;
        while (!done) {
            if (fast) {
                memcpy(buf + blen, pwd, (size_t)len);
                blen += (size_t)len;
                buf[blen++] = '\n';
            } else {
                emit(buf, &blen, pwd, len);
            }
            total++;
            if (blen >= OUT_BLOCK) {
                out_write(o, buf, blen);
                blen = 0;
            }
            if (has_limit && total >= limit) break;
            /* odometer increment (rightmost position) */
            int i = len - 1;
            while (i >= 0 && idx[i] == cslen - 1) { idx[i] = 0; pwd[i] = z; i--; }
            if (i < 0) done = 1;
            else { pwd[i] = cs[++idx[i]]; }
        }
        if (blen) { out_write(o, buf, blen); blen = 0; }
        free(pwd); free(idx);
        if (has_limit && total >= limit) break;
    }

    free(buf);
    if (cfg.verbose) printf("共生成 %llu 项\n", total);
    out_close(o);
}

static void generate_random(void)
{
    Out *o = out_open(cfg.output_file, cfg.to_stdout);
    if (!o) { fprintf(stderr, "无法打开输出: %s: %s\n", cfg.output_file, strerror(errno)); exit(1); }

    int len = cfg.fixed_len ? cfg.fixed_len : (cfg.min_len + cfg.max_len) / 2;
    if (len < 1) len = 1;
    int cslen = (int)strlen(cfg.charset);
    char *pwd = malloc((size_t)len + 1);
    size_t itemmax = (size_t)len * 4 + strlen(cfg.prefix) + strlen(cfg.suffix) + 16;
    char *buf = malloc(OUT_BLOCK + itemmax);
    if (!pwd || !buf) { perror("malloc"); exit(1); }
    pwd[len] = '\0';

    /* random bytes via getrandom-equivalent (rand() fallback) */
    unsigned long long generated = 0;
    size_t blen = 0;
    while (generated < cfg.random_count && !out_error(o)) {
        for (int i = 0; i < len; i++) pwd[i] = cfg.charset[rand() % cslen];
        emit(buf, &blen, pwd, len);
        generated++;
        if (blen >= OUT_BLOCK) { out_write(o, buf, blen); blen = 0; }
    }
    if (blen) out_write(o, buf, blen);
    free(pwd); free(buf);
    out_close(o);
}

static void generate_words(void)
{
    FILE *fp_dict = fopen(cfg.dict_file, "r");
    if (!fp_dict) { fprintf(stderr, "无法打开词典: %s: %s\n", cfg.dict_file, strerror(errno)); exit(1); }
    Out *o = out_open(cfg.output_file, cfg.to_stdout);
    if (!o) { fclose(fp_dict); fprintf(stderr, "无法打开输出: %s: %s\n", cfg.output_file, strerror(errno)); exit(1); }

    char line[MAX_LINE];
    unsigned long long count = 0;
    while (fgets(line, sizeof(line), fp_dict)) {
        if (cfg.limit && count >= cfg.limit) break;
        line[strcspn(line, "\n")] = '\0';
        out_printf(o, "%s%s%s\n", cfg.prefix, line, cfg.suffix);
        count++;
    }
    fclose(fp_dict);
    out_close(o);
}

/* ----------------------------------------------------------------------- */
/* calc mode                                                               */
/* ----------------------------------------------------------------------- */

/* Safety: refuse absurd precisions that would exhaust memory/CPU.  A request
 * like --digits 100000000 would need GB of RAM and hours of CPU. */
#define MAX_CALC_DIGITS 50000000L

static int run_calc(void)
{
    if (cfg.calc_digits > MAX_CALC_DIGITS) {
        double mb = (double)cfg.calc_digits * 3.3219280948873626 / 8.0 / 1048576.0;
        fprintf(stderr,
            "拒绝: --digits %ld 超过上限 %ld。\n"
            "  单个高精度数即约需 %.0f MB，加上中间结果通常要求数百 MB 内存，\n"
            "  且计算可能耗时数小时，会耗尽内存/电池。\n"
            "  建议: 先用较小值 (如 1000000) 试算，确认无误后再逐步提高。\n",
            cfg.calc_digits, MAX_CALC_DIGITS, mb);
        return 1;
    }
    const char *err = NULL;
    char *res = calc_run(cfg.expr, cfg.calc_digits, &err);
    if (!res) {
        fprintf(stderr, "计算出错: %s\n", err ? err : "未知错误");
        return 1;
    }
    /* A calculator should print to the terminal by default; only write to a
     * file when -o was given or --stdout is explicit. */
    int to_stdout = cfg.to_stdout || !cfg.output_set;
    Out *o = out_open(cfg.output_file, to_stdout);
    if (!o) { fprintf(stderr, "无法打开输出: %s: %s\n", cfg.output_file, strerror(errno)); free(res); return 1; }
    out_puts(o, res);
    out_puts(o, "\n");
    out_close(o);
    free(res);
    return 0;
}

/* ----------------------------------------------------------------------- */
/* CLI                                                                     */
/* ----------------------------------------------------------------------- */

static void print_version(void)
{
    printf("dictgen v%s (GMP %s / MPFR %s)\n", VERSION,
           gmp_version, mpfr_get_version());
    printf("Compiled %s %s\n", __DATE__, __TIME__);
}

static void print_help(const char *prog)
{
    printf("dictgen v%s - 字典生成器 + 任意精度计算器\n\n", VERSION);
    printf("用法:\n");
    printf("  %s [生成选项]                    生成字典（默认写入 %s）\n", prog, DEFAULT_OUTPUT);
    printf("  %s calc [选项] \"表达式\"          计算大数表达式\n", prog);
    printf("  %s pi [N]                       输出 π 的 N 位小数（默认 50）\n\n", prog);

    printf("生成选项:\n");
    printf("  -l, --length N         固定长度        -m/--min N  最小长度(默认1)\n");
    printf("  -M, --max N            最大长度(默认8)\n");
    printf("  -c, --charset STR      自定义字符集\n");
    printf("      --lowercase --uppercase --digits --symbols\n");
    printf("      --binary(01) --hex(0-9A-F) --octal(0-7)\n");
    printf("  -p, --prefix STR       前缀            -s, --suffix STR  后缀\n");
    printf("  -n, --limit N          最多生成 N 项（默认不限）\n");
    printf("  -r, --random           随机模式        -R, --count N  随机项数\n");
    printf("  -a, --mode MODE        password | word | calc\n");
    printf("  -d, --dict FILE        word 模式词典文件\n");
    printf("      --format FORMAT    none | space | escape\n");
    printf("      --ascii-names      二进制映射为 ASCII 名称\n\n");

    printf("计算选项:\n");
    printf("  -D, --precision N      精度（有效数字，默认 50；别名 --places）\n");
    printf("  支持: + - * / %% ^ ! ( )  pi e sqrt() abs() fact() pow() digit(x,k)\n");
    printf("  示例: \"2^1000000\"  \"100000!\"  \"2*pi*10\"  \"digit(2*pi,32)\"  \"pi^2\"\n\n");

    printf("输出选项:\n");
    printf("  -o, --output FILE      输出文件；以 .tar/.tar.gz/.tgz 结尾则打包\n");
    printf("  -t, --stdout           直接输出到终端（-tff 同义），不写文件\n");
    printf("  -v, --verbose          显示进度        --version  版本\n");
    printf("  -h, --help             帮助\n\n");

    printf("示例:\n");
    printf("  %s -l 8 -c \"0123456789\" -n 100000000 -o num.tar.gz\n", prog);
    printf("  %s calc \"2^1000000\" -o power.txt\n", prog);
    printf("  %s calc -D 1000000 \"2*pi*10\" -o two_pi.tar.gz\n", prog);
    printf("  %s calc \"digit(2*pi,32)\" -t\n", prog);
    printf("  %s pi 1000000 -t\n", prog);
}

static int parse_args(int argc, char *argv[])
{
    static struct option long_options[] = {
        {"help",        no_argument,       0, 'h'},
        {"version",     no_argument,       0, 1000},
        {"output",      required_argument, 0, 'o'},
        {"stdout",      no_argument,       0, 't'},
        {"min",         required_argument, 0, 'm'},
        {"max",         required_argument, 0, 'M'},
        {"length",      required_argument, 0, 'l'},
        {"charset",     required_argument, 0, 'c'},
        {"prefix",      required_argument, 0, 'p'},
        {"suffix",      required_argument, 0, 's'},
        {"limit",       required_argument, 0, 'n'},
        {"random",      no_argument,       0, 'r'},
        {"count",       required_argument, 0, 'R'},
        {"mode",        required_argument, 0, 'a'},
        {"dict",        required_argument, 0, 'd'},
        {"separator",   required_argument, 0, 'S'},
        {"verbose",     no_argument,       0, 'v'},
        {"lowercase",   no_argument,       0, '1'},
        {"uppercase",   no_argument,       0, '2'},
        {"digits",      no_argument,       0, '3'},
        {"numbers",     no_argument,       0, '3'},
        {"symbols",     no_argument,       0, '4'},
        {"binary",      no_argument,       0, '5'},
        {"hex",         no_argument,       0, '6'},
        {"octal",       no_argument,       0, '7'},
        {"precision",   required_argument, 0, 'D'},
        {"places",      required_argument, 0, 'D'},
        {"format",      required_argument, 0, 'F'},
        {"ascii-names", no_argument,       0, 'A'},
        {"map-file",    required_argument, 0, 'f'},
        {0, 0, 0, 0}
    };

    int opt, opt_idx = 0;
    char builtin[256] = "";
    opterr = 0;

    /* Support "-tff" as an alias for "--stdout" (getopt would otherwise split
     * it into -t -f f, treating "f" as the --map-file argument). */
    char **av = malloc(sizeof(char *) * (argc + 1));
    if (!av) { fprintf(stderr, "内存不足\n"); return 1; }
    for (int i = 0; i < argc; i++)
        av[i] = (strcmp(argv[i], "-tff") == 0) ? (char *)"--stdout" : argv[i];
    av[argc] = NULL;

    while ((opt = getopt_long(argc, av, "hvo:m:M:l:c:p:s:n:rR:a:d:S:tD:",
                              long_options, &opt_idx)) != -1) {
        switch (opt) {
            case 'h': print_help(g_prog); free(av); exit(0);
            case 1000: print_version(); free(av); exit(0);
            case 'o': snprintf(cfg.output_file, sizeof(cfg.output_file), "%s", optarg); cfg.output_set = 1; break;
            case 't': cfg.to_stdout = 1; break;
            case 'm': cfg.min_len = atoi(optarg); break;
            case 'M': cfg.max_len = atoi(optarg); break;
            case 'l': cfg.fixed_len = atoi(optarg); cfg.min_len = cfg.max_len = cfg.fixed_len; break;
            case 'c': snprintf(cfg.charset, sizeof(cfg.charset), "%s", optarg); break;
            case 'p': snprintf(cfg.prefix, sizeof(cfg.prefix), "%s", optarg); break;
            case 's': snprintf(cfg.suffix, sizeof(cfg.suffix), "%s", optarg); break;
            case 'n': cfg.limit = strtoull(optarg, NULL, 10); break;
            case 'r': cfg.random_mode = 1; break;
            case 'R': cfg.random_count = strtoull(optarg, NULL, 10); break;
            case 'a': snprintf(cfg.mode, sizeof(cfg.mode), "%s", optarg); break;
            case 'd': snprintf(cfg.dict_file, sizeof(cfg.dict_file), "%s", optarg); break;
            case 'S': snprintf(cfg.separator, sizeof(cfg.separator), "%s", optarg); break;
            case 'D': cfg.calc_digits = atol(optarg); break;
            case 'v': cfg.verbose = 1; break;
            case '1': safe_strcat(builtin, sizeof(builtin), LOWERCASE); break;
            case '2': safe_strcat(builtin, sizeof(builtin), UPPERCASE); break;
            case '3': safe_strcat(builtin, sizeof(builtin), DIGITS); break;
            case '4': safe_strcat(builtin, sizeof(builtin), SYMBOLS); break;
            case '5': snprintf(cfg.charset, sizeof(cfg.charset), "01"); break;
            case '6': snprintf(cfg.charset, sizeof(cfg.charset), "0123456789ABCDEF"); break;
            case '7': snprintf(cfg.charset, sizeof(cfg.charset), "01234567"); break;
            case 'F':
                if (strcmp(optarg, "space") == 0) cfg.format = FORMAT_SPACE;
                else if (strcmp(optarg, "escape") == 0) cfg.format = FORMAT_ESCAPE;
                else if (strcmp(optarg, "none") == 0) cfg.format = FORMAT_NONE;
                else { fprintf(stderr, "无效 format: %s (none|space|escape)\n", optarg); return 1; }
                break;
            case 'A': cfg.ascii_names = 1; break;
            case 'f': snprintf(cfg.map_file, sizeof(cfg.map_file), "%s", optarg); break;
            default:
                fprintf(stderr, "无效选项，用 -h 查看帮助。\n");
                free(av);
                return 1;
        }
    }

    /* trailing positional tokens become the expression (calc mode).
     * NOTE: iterate over av (getopt may have permuted it), not argv. */
    for (int i = optind; i < argc; i++) {
        if (cfg.expr[0]) safe_strcat(cfg.expr, sizeof(cfg.expr), " ");
        safe_strcat(cfg.expr, sizeof(cfg.expr), av[i]);
    }

    if (cfg.charset[0] == '\0' && builtin[0] != '\0')
        snprintf(cfg.charset, sizeof(cfg.charset), "%s", builtin);
    if (cfg.charset[0] == '\0')
        snprintf(cfg.charset, sizeof(cfg.charset), "%s%s", LOWERCASE, DIGITS);

    free(av);
    (void)opt_idx; (void)opt;
    return 0;
}

static void validate_config(void)
{
    if (cfg.min_len <= 0) cfg.min_len = 1;
    if (cfg.max_len < cfg.min_len) cfg.max_len = cfg.min_len;
    if (cfg.fixed_len > 0) cfg.min_len = cfg.max_len = cfg.fixed_len;

    if (strcmp(cfg.mode, "word") == 0 && cfg.dict_file[0] == '\0') {
        fprintf(stderr, "错误: word 模式需要词典文件 (-d)\n"); exit(1);
    }
    if (cfg.random_mode && cfg.random_count == 0) {
        fprintf(stderr, "错误: 随机模式需要 -R/--count\n"); exit(1);
    }
    if (cfg.charset[0] == '\0') {
        fprintf(stderr, "错误: 字符集不能为空\n"); exit(1);
    }
}

/* ----------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    srand((unsigned)time(NULL));
    if (argc > 0 && argv[0]) g_prog = argv[0];

    if (argc == 1) { print_help(g_prog); return 0; }

    if (argc >= 2 && strcmp(argv[1], "help") == 0) { print_help(argv[0]); return 0; }
    if (argc >= 2 && strcmp(argv[1], "version") == 0) { print_version(); return 0; }

    if (argc >= 2 && strcmp(argv[1], "calc") == 0) {
        /* calc [options] expr [options] */
        char **ca = map_calc_argv(argc, argv);
        if (!ca) { fprintf(stderr, "内存不足\n"); return 1; }
        int es = find_expr_start(argc, ca);
        if (es < argc && ca[es][0] == '-' && ca[es][1] != '\0') {
            /* expression itself starts with '-', e.g. "-5": options may only
             * appear before it (getopt would misparse "-5" as options). */
            if (parse_calc_args(argc, ca, &es)) return 1;
            for (int j = es; j < argc; j++) {
                if (cfg.expr[0]) safe_strcat(cfg.expr, sizeof(cfg.expr), " ");
                safe_strcat(cfg.expr, sizeof(cfg.expr), ca[j]);
            }
        } else {
            if (parse_args(argc - 1, ca + 1)) return 1;
        }
        if (cfg.expr[0] == '\0') { fprintf(stderr, "用法: %s calc [选项] \"表达式\"\n", argv[0]); return 1; }
        return run_calc();
    }
    if (argc >= 2 && strcmp(argv[1], "pi") == 0) {
        /* pi [N] [options]  - print pi to N decimal places (default 50) */
        char **ca = map_calc_argv(argc, argv);
        if (!ca) { fprintf(stderr, "内存不足\n"); return 1; }
        int es = find_expr_start(argc, ca);
        if (es < argc && ca[es][0] == '-' && ca[es][1] != '\0') {
            if (parse_calc_args(argc, ca, &es)) return 1;
            for (int j = es; j < argc; j++) {
                if (cfg.expr[0]) safe_strcat(cfg.expr, sizeof(cfg.expr), " ");
                safe_strcat(cfg.expr, sizeof(cfg.expr), ca[j]);
            }
        } else {
            if (parse_args(argc - 1, ca + 1)) return 1;
        }
        long n = 50;
        int explicit_digits = (cfg.calc_digits != 50);
        if (cfg.expr[0]) {
            char *end; long v = strtol(cfg.expr, &end, 10);
            if (*end == '\0' && v > 0) n = v;
        }
        if (explicit_digits) n = cfg.calc_digits;
        snprintf(cfg.expr, sizeof(cfg.expr), "pi");
        /* pi has one integer digit: N decimal places => N+1 significant digits */
        cfg.calc_digits = n + 1;
        return run_calc();
    }

    if (parse_args(argc, argv) != 0) return 1;
    validate_config();

    if (strcmp(cfg.mode, "calc") == 0) {
        if (cfg.expr[0] == '\0') { fprintf(stderr, "calc 模式需要表达式\n"); return 1; }
        return run_calc();
    }

    if (cfg.verbose) {
        printf("模式: %s\n", cfg.mode);
        if (strcmp(cfg.mode, "password") == 0) {
            if (cfg.fixed_len) printf("长度: %d\n", cfg.fixed_len);
            else printf("长度范围: %d ~ %d\n", cfg.min_len, cfg.max_len);
            printf("字符集: %s\n", cfg.charset);
        }
        if (cfg.prefix[0]) printf("前缀: %s\n", cfg.prefix);
        if (cfg.suffix[0]) printf("后缀: %s\n", cfg.suffix);
        printf("输出: %s\n", cfg.to_stdout ? "(stdout)" : cfg.output_file);
        if (cfg.limit) printf("数量上限: %llu\n", cfg.limit);
        printf("----------------------------------------\n");
    }

    if (strcmp(cfg.mode, "word") == 0) generate_words();
    else if (cfg.random_mode) generate_random();
    else generate_passwords();

    if (!cfg.to_stdout)
        printf("完成！结果已写入 %s\n", cfg.output_file);
    return 0;
}
