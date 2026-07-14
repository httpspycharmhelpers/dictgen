#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <time.h>
#include <limits.h>
#include <errno.h>

#define VERSION "1.2.0"
#define DEFAULT_OUTPUT "passwords.txt"
#define BATCH_SIZE 1000
#define MAX_LINE 1024

typedef enum {
    FORMAT_NONE,
    FORMAT_SPACE,
    FORMAT_ESCAPE
} FormatType;

typedef struct {
    char mode[16];
    int min_len;
    int max_len;
    int fixed_len;
    char charset[256];
    char prefix[64];
    char suffix[64];
    char output_file[256];
    unsigned long long limit;
    int random_mode;
    unsigned long long random_count;
    char dict_file[256];
    char separator[8];
    int verbose;
    FormatType format;
    int ascii_names;
    char map_file[256];
} Config;

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
    .map_file = ""
};

#define LOWERCASE "abcdefghijklmnopqrstuvwxyz"
#define UPPERCASE "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
#define DIGITS "0123456789"
#define SYMBOLS "!@#$%^&*()_+-=[]{};:'\"\\|,./<>?"

const char* get_ascii_name(unsigned int val) {
    static const char *names[256] = {
        "NUL", "SOH", "STX", "ETX", "EOT", "ENQ", "ACK", "BEL",
        "BS",  "TAB", "LF",  "VT",  "FF",  "CR",  "SO",  "SI",
        "DLE", "DC1", "DC2", "DC3", "DC4", "NAK", "SYN", "ETB",
        "CAN", "EM",  "SUB", "ESC", "FS",  "GS",  "RS",  "US",
        "SP",  "!",   "\"",  "#",   "$",   "%",   "&",   "'",
        "(",   ")",   "*",   "+",   ",",   "-",   ".",   "/",
        "0",   "1",   "2",   "3",   "4",   "5",   "6",   "7",
        "8",   "9",   ":",   ";",   "<",   "=",   ">",   "?",
        "@",   "A",   "B",   "C",   "D",   "E",   "F",   "G",
        "H",   "I",   "J",   "K",   "L",   "M",   "N",   "O",
        "P",   "Q",   "R",   "S",   "T",   "U",   "V",   "W",
        "X",   "Y",   "Z",   "[",   "\\",  "]",   "^",   "_",
        "`",   "a",   "b",   "c",   "d",   "e",   "f",   "g",
        "h",   "i",   "j",   "k",   "l",   "m",   "n",   "o",
        "p",   "q",   "r",   "s",   "t",   "u",   "v",   "w",
        "x",   "y",   "z",   "{",   "|",   "}",   "~",   "DEL"
    };
    if (val < 256) return names[val];
    return "???";
}

void print_help(const char *prog);
void print_version(void);
int parse_args(int argc, char *argv[]);
void validate_config(void);
unsigned long long estimate_total(int len, int charset_len);
void generate_passwords(void);
void generate_words(void);
void generate_random(void);
int next_password(char *pwd, int len, const char *charset);
void safe_strcat(char *dest, size_t size, const char *src);
void format_password(const char *pwd, char *out, size_t out_size, FormatType format);
void apply_mapping(const char *pwd, char *out, size_t out_size);

int main(int argc, char *argv[]) {
    srand((unsigned)time(NULL));

    if (argc == 1) {
        print_help(argv[0]);
        return 0;
    }

    if (parse_args(argc, argv) != 0) {
        return 1;
    }
    validate_config();

    if (cfg.verbose) {
        printf("Mode: %s\n", cfg.mode);
        if (strcmp(cfg.mode, "password") == 0) {
            if (cfg.fixed_len)
                printf("Length: %d\n", cfg.fixed_len);
            else
                printf("Length range: %d ~ %d\n", cfg.min_len, cfg.max_len);
            printf("Charset: %s\n", cfg.charset[0] ? cfg.charset : "(using built-in combination)");
        }
        if (cfg.prefix[0]) printf("Prefix: %s\n", cfg.prefix);
        if (cfg.suffix[0]) printf("Suffix: %s\n", cfg.suffix);
        printf("Output file: %s\n", cfg.output_file);
        if (cfg.limit) printf("Generation limit: %llu\n", cfg.limit);
        if (cfg.random_mode) printf("Random generation: %llu items\n", cfg.random_count);
        printf("Format: ");
        switch (cfg.format) {
            case FORMAT_SPACE:  printf("space separated\n"); break;
            case FORMAT_ESCAPE: printf("\\x escaped\n"); break;
            default:            printf("plain\n");
        }
        if (cfg.ascii_names) printf("ASCII name mapping: enabled\n");
        printf("----------------------------------------\n");
    }

    if (strcmp(cfg.mode, "word") == 0) {
        generate_words();
    } else if (cfg.random_mode) {
        generate_random();
    } else {
        generate_passwords();
    }

    printf("Done! Results saved to %s\n", cfg.output_file);
    return 0;
}

void print_help(const char *prog) {
    printf("Usage: %s [options]\n\n", prog);
    printf("Basic options:\n");
    printf("  -h, --help              Show this help\n");
    printf("  --version               Show version information\n");
    printf("  -v, --verbose           Show progress information\n");
    printf("  -o, --output FILE       Output file (default: %s)\n", DEFAULT_OUTPUT);
    printf("  -m, --min N             Minimum length (default: 1)\n");
    printf("  -M, --max N             Maximum length (default: 8)\n");
    printf("  -l, --length N          Fixed length (overrides min/max)\n");
    printf("  -c, --charset STR       Custom character set\n");
    printf("  --lowercase             Include lowercase letters\n");
    printf("  --uppercase             Include uppercase letters\n");
    printf("  --digits                Include digits\n");
    printf("  --symbols               Include symbols\n");
    printf("  --binary                Use binary charset (01)\n");
    printf("  --hex                   Use hexadecimal charset (0-9A-F)\n");
    printf("  --octal                 Use octal charset (0-7)\n");
    printf("  --ascii-names           Map binary values to ASCII control character names\n");
    printf("                          (only effective when charset is 01 and length <= 8)\n");
    printf("  -p, --prefix STR        Add prefix\n");
    printf("  -s, --suffix STR        Add suffix\n");
    printf("  -n, --limit N           Maximum number of generated items (default: unlimited)\n");
    printf("  -r, --random            Enable random generation mode\n");
    printf("  -R, --count N           Number of random items to generate (requires -r)\n");
    printf("  -a, --mode MODE         Mode: password or word (default: password)\n");
    printf("  -d, --dict FILE         Dictionary file (required for word mode)\n");
    printf("  -S, --separator CHAR    Separator for combining words (not implemented yet)\n");
    printf("  --format FORMAT         Output format: none, space, escape (default: none)\n");
    printf("    none   : plain output\n");
    printf("    space  : insert spaces between characters (e.g. \"1 0 1\")\n");
    printf("    escape : escape as \\x form (e.g. \"\\x01\", per character)\n\n");
    printf("Examples:\n");
    printf("  %s -l 6 -c \"abc123\" -o pass.txt\n", prog);
    printf("  %s --min 4 --max 6 --prefix \"admin_\" --suffix \"2026\" --lowercase --digits\n", prog);
    printf("  %s -r -R 100 -l 8 -c \"0123456789\"\n", prog);
    printf("  %s --mode word --dict words.txt --suffix \"!\"\n", prog);
    printf("  %s --binary -l 4 --format space -n 10        generate 4-bit binary with spaces\n", prog);
    printf("  %s --binary -l 3 --ascii-names -n 8          generate 3-bit binary mapped to NUL, SOH...\n", prog);
}

void print_version(void) {
    printf("Dictionary Generator v%s\n", VERSION);
    printf("Compiled on %s %s\n", __DATE__, __TIME__);
}

void safe_strcat(char *dest, size_t size, const char *src) {
    strncat(dest, src, size - strlen(dest) - 1);
}

void apply_mapping(const char *pwd, char *out, size_t out_size) {
    if (!cfg.ascii_names && cfg.map_file[0] == '\0') {
        strncpy(out, pwd, out_size - 1);
        out[out_size - 1] = '\0';
        return;
    }

    if (cfg.ascii_names) {
        int len = strlen(pwd);
        int is_binary = 1;
        for (int i = 0; i < len; i++) {
            if (pwd[i] != '0' && pwd[i] != '1') {
                is_binary = 0;
                break;
            }
        }
        if (is_binary && len <= 8) {
            unsigned int val = 0;
            for (int i = 0; i < len; i++) {
                val = (val << 1) | (pwd[i] - '0');
            }
            const char *name = get_ascii_name(val);
            snprintf(out, out_size, "%s", name);
            return;
        } else {
            strncpy(out, pwd, out_size - 1);
            out[out_size - 1] = '\0';
            return;
        }
    }

    strncpy(out, pwd, out_size - 1);
    out[out_size - 1] = '\0';
}

void format_password(const char *pwd, char *out, size_t out_size, FormatType format) {
    if (format == FORMAT_NONE) {
        strncpy(out, pwd, out_size - 1);
        out[out_size - 1] = '\0';
        return;
    }

    int len = strlen(pwd);
    char *ptr = out;
    size_t remaining = out_size;

    for (int i = 0; i < len; i++) {
        switch (format) {
            case FORMAT_SPACE:
                if (i > 0) {
                    int n = snprintf(ptr, remaining, " %c", pwd[i]);
                    if (n < 0 || n >= (int)remaining) return;
                    ptr += n;
                    remaining -= n;
                } else {
                    int n = snprintf(ptr, remaining, "%c", pwd[i]);
                    if (n < 0 || n >= (int)remaining) return;
                    ptr += n;
                    remaining -= n;
                }
                break;
            case FORMAT_ESCAPE:
                {
                    int n = snprintf(ptr, remaining, "\\x%02X", (unsigned char)pwd[i]);
                    if (n < 0 || n >= (int)remaining) return;
                    ptr += n;
                    remaining -= n;
                }
                break;
            default:
                break;
        }
    }
    *ptr = '\0';
}

int parse_args(int argc, char *argv[]) {
    static struct option long_options[] = {
        {"help",        no_argument,       0, 'h'},
        {"version",     no_argument,       0, 1000 },
        {"output",      required_argument, 0, 'o'},
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
        {"symbols",     no_argument,       0, '4'},
        {"binary",      no_argument,       0, '5'},
        {"hex",         no_argument,       0, '6'},
        {"octal",       no_argument,       0, '7'},
        {"format",      required_argument, 0, 'F'},
        {"ascii-names", no_argument,       0, 'A'},
        {"map-file",    required_argument, 0, 'f'},
        {0, 0, 0, 0}
    };

    int opt, opt_idx = 0;
    char builtin[256] = "";

    while ((opt = getopt_long(argc, argv, "hvo:m:M:l:c:p:s:n:rR:a:d:S:v",
                              long_options, &opt_idx)) != -1) {
        switch (opt) {
            case 'h': print_help(argv[0]); exit(0);
            case 1000: print_version(); exit(0);
            case 'o': strncpy(cfg.output_file, optarg, sizeof(cfg.output_file)-1); break;
            case 'm': cfg.min_len = atoi(optarg); break;
            case 'M': cfg.max_len = atoi(optarg); break;
            case 'l': cfg.fixed_len = atoi(optarg); cfg.min_len = cfg.max_len = cfg.fixed_len; break;
            case 'c': strncpy(cfg.charset, optarg, sizeof(cfg.charset)-1); break;
            case 'p': strncpy(cfg.prefix, optarg, sizeof(cfg.prefix)-1); break;
            case 's': strncpy(cfg.suffix, optarg, sizeof(cfg.suffix)-1); break;
            case 'n': cfg.limit = strtoull(optarg, NULL, 10); break;
            case 'r': cfg.random_mode = 1; break;
            case 'R': cfg.random_count = strtoull(optarg, NULL, 10); break;
            case 'a': strncpy(cfg.mode, optarg, sizeof(cfg.mode)-1); break;
            case 'd': strncpy(cfg.dict_file, optarg, sizeof(cfg.dict_file)-1); break;
            case 'S': strncpy(cfg.separator, optarg, sizeof(cfg.separator)-1); break;
            case 'v': cfg.verbose = 1; break;
            case '1': safe_strcat(builtin, sizeof(builtin), LOWERCASE); break;
            case '2': safe_strcat(builtin, sizeof(builtin), UPPERCASE); break;
            case '3': safe_strcat(builtin, sizeof(builtin), DIGITS); break;
            case '4': safe_strcat(builtin, sizeof(builtin), SYMBOLS); break;
            case '5': strncpy(cfg.charset, "01", sizeof(cfg.charset)-1); break;
            case '6': strncpy(cfg.charset, "0123456789ABCDEF", sizeof(cfg.charset)-1); break;
            case '7': strncpy(cfg.charset, "01234567", sizeof(cfg.charset)-1); break;
            case 'F':
                if (strcmp(optarg, "space") == 0)
                    cfg.format = FORMAT_SPACE;
                else if (strcmp(optarg, "escape") == 0)
                    cfg.format = FORMAT_ESCAPE;
                else if (strcmp(optarg, "none") == 0)
                    cfg.format = FORMAT_NONE;
                else {
                    fprintf(stderr, "Invalid format: %s (choose: none, space, escape)\n", optarg);
                    return 1;
                }
                break;
            case 'A': cfg.ascii_names = 1; break;
            case 'f': strncpy(cfg.map_file, optarg, sizeof(cfg.map_file)-1); break;
            default:
                fprintf(stderr, "Invalid option or missing argument. Use -h for help.\n");
                return 1;
        }
    }

    if (cfg.charset[0] == '\0' && builtin[0] != '\0') {
        strncpy(cfg.charset, builtin, sizeof(cfg.charset)-1);
    }
    if (cfg.charset[0] == '\0') {
        snprintf(cfg.charset, sizeof(cfg.charset), "%s%s", LOWERCASE, DIGITS);
    }

    return 0;
}

void validate_config(void) {
    if (cfg.min_len <= 0) cfg.min_len = 1;
    if (cfg.max_len < cfg.min_len) cfg.max_len = cfg.min_len;
    if (cfg.fixed_len > 0) cfg.min_len = cfg.max_len = cfg.fixed_len;

    if (strcmp(cfg.mode, "word") == 0 && cfg.dict_file[0] == '\0') {
        fprintf(stderr, "Error: word mode requires a dictionary file (-d/--dict)\n");
        exit(1);
    }
    if (cfg.random_mode && cfg.random_count == 0) {
        fprintf(stderr, "Error: random mode requires a count (-R/--count)\n");
        exit(1);
    }
    if (cfg.charset[0] == '\0') {
        fprintf(stderr, "Error: character set cannot be empty\n");
        exit(1);
    }
}

unsigned long long estimate_total(int len, int charset_len) {
    unsigned long long total = 1;
    for (int i = 0; i < len; i++) {
        if (total > ULLONG_MAX / charset_len) {
            return ULLONG_MAX;
        }
        total *= charset_len;
    }
    return total;
}

int next_password(char *pwd, int len, const char *charset) {
    int charset_len = strlen(charset);
    int carry = len - 1;
    while (carry >= 0) {
        char *pos = strchr(charset, pwd[carry]);
        if (!pos) {
            pwd[carry] = charset[0];
            carry--;
            continue;
        }
        int idx = pos - charset;
        if (idx == charset_len - 1) {
            pwd[carry] = charset[0];
            carry--;
        } else {
            pwd[carry] = charset[idx + 1];
            return 1;
        }
    }
    return 0;
}

void generate_passwords(void) {
    FILE *fp = fopen(cfg.output_file, "w");
    if (!fp) {
        perror("Failed to open output file");
        exit(1);
    }

    int charset_len = strlen(cfg.charset);
    unsigned long long total_generated = 0;
    int len_start = cfg.min_len;
    int len_end = cfg.max_len;
    char mapped[1024];
    char formatted[1024];

    for (int len = len_start; len <= len_end; len++) {
        unsigned long long total_for_len = estimate_total(len, charset_len);
        if (cfg.verbose) {
            printf("Length %d: estimated %llu combinations\n", len, total_for_len);
        }
        if (total_for_len == ULLONG_MAX) {
            printf("Warning: length %d overflows, cannot complete.\n", len);
            break;
        }

        char *pwd = malloc(len + 1);
        if (!pwd) {
            perror("Memory allocation failed");
            fclose(fp);
            exit(1);
        }
        memset(pwd, cfg.charset[0], len);
        pwd[len] = '\0';

        unsigned long long count = 0;
        int cont = 1;
        while (cont) {
            if (cfg.limit > 0 && total_generated >= cfg.limit) {
                cont = 0;
                break;
            }
            apply_mapping(pwd, mapped, sizeof(mapped));
            format_password(mapped, formatted, sizeof(formatted), cfg.format);
            fprintf(fp, "%s%s%s\n", cfg.prefix, formatted, cfg.suffix);
            total_generated++;
            count++;
            if (count % BATCH_SIZE == 0) fflush(fp);

            if (!next_password(pwd, len, cfg.charset)) {
                cont = 0;
            }
        }
        free(pwd);
        if (cfg.limit > 0 && total_generated >= cfg.limit) break;
    }

    fclose(fp);
}

void generate_words(void) {
    FILE *fp_dict = fopen(cfg.dict_file, "r");
    if (!fp_dict) {
        perror("Failed to open dictionary file");
        exit(1);
    }

    FILE *fp_out = fopen(cfg.output_file, "w");
    if (!fp_out) {
        perror("Failed to open output file");
        fclose(fp_dict);
        exit(1);
    }

    char line[MAX_LINE];
    unsigned long long count = 0;
    while (fgets(line, sizeof(line), fp_dict)) {
        if (cfg.limit > 0 && count >= cfg.limit) break;
        line[strcspn(line, "\n")] = '\0';
        fprintf(fp_out, "%s%s%s\n", cfg.prefix, line, cfg.suffix);
        count++;
        if (count % BATCH_SIZE == 0) fflush(fp_out);
    }

    fclose(fp_dict);
    fclose(fp_out);
}

void generate_random(void) {
    FILE *fp = fopen(cfg.output_file, "w");
    if (!fp) {
        perror("Failed to open output file");
        exit(1);
    }

    int len = cfg.fixed_len ? cfg.fixed_len : (cfg.min_len + cfg.max_len) / 2;
    if (cfg.min_len != cfg.max_len && cfg.verbose) {
        printf("Random mode using length %d (midpoint of %d~%d)\n", len, cfg.min_len, cfg.max_len);
    }

    int charset_len = strlen(cfg.charset);
    char *pwd = malloc(len + 1);
    if (!pwd) {
        perror("Memory allocation failed");
        fclose(fp);
        exit(1);
    }
    pwd[len] = '\0';
    char mapped[1024];
    char formatted[1024];

    unsigned long long generated = 0;
    while (generated < cfg.random_count) {
        for (int i = 0; i < len; i++) {
            pwd[i] = cfg.charset[rand() % charset_len];
        }
        apply_mapping(pwd, mapped, sizeof(mapped));
        format_password(mapped, formatted, sizeof(formatted), cfg.format);
        fprintf(fp, "%s%s%s\n", cfg.prefix, formatted, cfg.suffix);
        generated++;
        if (generated % BATCH_SIZE == 0) fflush(fp);
    }

    free(pwd);
    fclose(fp);
}