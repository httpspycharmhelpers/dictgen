
# dictgen – 字典生成器 + 任意精度计算器

高性能命令行工具，用于生成密码、字典、二进制序列等，并支持任意精度的大数计算
（GMP + MPFR）。生成走单遍流式管线，可轻松输出上亿条目；输出可直接写入普通文件、
tar / tar.gz 打包，或打印到终端。

## 构建

依赖：`gmp`、`mpfr`、`zlib`（Termux: `pkg install libgmp libmpfr zlib`）。

```
make            # 生成 ./dictgen
make test       # 运行测试套件
```

或手动编译：

```
gcc -O3 -o dictgen dictgen.c out.c calc.c -lgmp -lmpfr -lz -lm
```

## 用法

```
dictgen [生成选项]                    生成字典（默认写入 passwords.txt）
dictgen calc [选项] "表达式"          计算大数表达式
dictgen pi [N]                        输出 π 的 N 位小数（默认 50）
```

### 生成选项

| 选项 | 说明 |
| --- | --- |
| `-h, --help` | 帮助 |
| `--version` | 版本 |
| `-v, --verbose` | 显示进度 |
| `-o, --output FILE` | 输出文件；以 `.tar` / `.tar.gz` / `.tgz` 结尾则自动打包 |
| `-t, --stdout` / `-tff` | 直接输出到终端，不写文件 |
| `-m N` / `-M N` | 最小 / 最大长度（默认 1 / 8） |
| `-l N` | 固定长度（覆盖 -m/-M） |
| `-c STR` | 自定义字符集 |
| `--lowercase` `--uppercase` `--digits` `--symbols` | 组合内置字符集（`--numbers` 为 `--digits` 别名） |
| `--binary` `--hex` `--octal` | 使用 01 / 0-9A-F / 0-7 |
| `--ascii-names` | 二进制映射为 ASCII 名称（`00000000` → `NUL`） |
| `-p STR` / `-s STR` | 前缀 / 后缀 |
| `-n, --limit N` | 数量上限 |
| `-r, --random` / `-R, --count N` | 随机模式 / 随机项数 |
| `-a, --mode MODE` | `password`（默认）、`word`、`calc` |
| `-d, --dict FILE` | word 模式词典文件 |
| `--format FORMAT` | `none`（默认）、`space`、`escape` |

### 计算选项

在 `calc` / `pi` 模式下：

| 选项 | 说明 |
| --- | --- |
| `-D, --precision N` | 精度：有效数字位数（默认 50），可为 1000000；别名 `--places` |
| `--digits N` | 同 `--precision`（仅在 `calc`/`pi` 模式下） |
| `-o, --output FILE` | 结果写入文件或 tar 包（默认打印到终端） |
| `-t, --stdout` / `-tff` | 强制打印到终端 |

> 注意：`--digits` 在**生成**模式下表示"包含数字字符集"，在 **calc/pi** 模式下表示精度。

支持的表达式：

- 运算符：`+  -  *  /  %  ^`（`^` 右结合）、后缀阶乘 `!`、一元 `-`、括号
- 常量：`pi`、`e`
- 函数：`sqrt(x)`、`abs(x)`、`fact(n)`、`pow(a,b)`、`digit(x,k)`（x 的小数第 k 位）
- 隐式乘法：`2pi`、`2(3+4)`、`pi(1+1)`
- 整数（任意长度）、小数（如 `3.14`）

整数运算保持精确（GMP），涉及 `pi`/`e`/`sqrt` 或非整除除法时按给定精度用 MPFR 计算。
`digit(x,k)` 要求 `--digits ≥ k`。

## 示例

```bash
# 生成 1 亿个 8 位数字字典，直接写入 gzip 包
./dictgen -l 8 -c "0123456789" -n 100000000 -o num.tar.gz

# 直接打印到终端
./dictgen -l 2 -c abc -t
./dictgen -l 8 -c 0123456789 -n 100 -tff

# 大数计算
./dictgen calc "2^1000000"                 # 2 的一百万次方（30 万位）
./dictgen calc "100000!"                   # 10 万阶乘
./dictgen calc "2*pi*10" -D 1000000 -o two_pi.tar.gz   # 计算到 100 万位并存包
./dictgen calc "2*pi*10" -o two_pi.tar.gz -D 1000000   # 选项可放前或后
./dictgen calc "pi^2" -D 100               # π 的平方
./dictgen calc "digit(2*pi,32)"            # 2π 的第 32 位小数
./dictgen pi 1000000                       # π 的一百万位小数

# 其他
./dictgen -a word -d words.txt -s "!"      # 词典加后缀
./dictgen --binary -l 4 --format space     # 4 位二进制，空格分隔
```

## 性能

单机（Termux, aarch64）实测：

| 任务 | 耗时 |
| --- | --- |
| 生成 1000 万个 8 位数字 | ~0.16 s |
| 生成 1 亿个 8 位数字 | ~1.4 s |
| `2^1000000`（30 万位整数） | < 0.1 s |
| `100000!`（45 万位整数） | < 0.1 s |
| π 到 100 万位 | ~3 s |

生成器使用大块缓冲输出、索引式 odometer，并对"无前缀/后缀/格式"的常见情形走直拷快路径。
高精度计算的耗时主要来自 MPFR 的 `mpfr_const_pi`，属库层面的下限。
