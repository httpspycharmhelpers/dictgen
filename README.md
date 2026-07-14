# dictgen – Dictionary Generator

A highly customizable command-line tool for generating passwords, wordlists, binary sequences, etc.

## Build
```bash
gcc -o dictgen dictgen.c -O2 -Wall
```

Options

Option Description
-h, --help Show help
--version Show version
-v, --verbose Show progress
-o FILE Output file (default: passwords.txt)
-m N Minimum length (default: 1)
-M N Maximum length (default: 8)
-l N Fixed length (overrides -m/-M)
-c STR Custom character set
--lowercase Include a-z
--uppercase Include A-Z
--digits Include 0-9
--symbols Include symbols !@#$%^&*...
--binary Use charset 01
--hex Use charset 0-9A-F
--octal Use charset 0-7
--ascii-names Map binary (01) to ASCII names (e.g. 000 → NUL)
-p STR Add prefix
-s STR Add suffix
-n N Limit number of generated items
-r Enable random mode
-R N Generate N random items (requires -r)
-a MODE Mode: password (default) or word
-d FILE Dictionary file (required for word mode)
--format FORMAT Output format: none (default), space, escape

Examples

```bash
# Generate 3‑digit combinations from abc
./dictgen -l 3 -c "abc"

# Word mode with suffix
./dictgen -a word -d words.txt -s "!"

# 4‑bit binary with spaces
./dictgen --binary -l 4 --format space
```
