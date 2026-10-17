/*
 * calc.h - arbitrary-precision expression evaluation (GMP + MPFR)
 */
#ifndef DICTGEN_CALC_H
#define DICTGEN_CALC_H

/* Evaluate an arbitrary-precision expression and return a newly allocated
 * decimal string (caller frees).  Integer-only expressions stay exact (GMP
 * mpz); anything touching pi/e/sqrt or a non-exact division uses MPFR at the
 * requested decimal precision.
 *
 *   expr   : expression, e.g. "2^1000000", "100000!", "2*pi*10", "digit(2*pi,32)"
 *   digits : decimal significant digits of precision (e.g. 50, 1000000)
 *
 * Returns NULL on error and, if errmsg != NULL, sets *errmsg to a static
 * human-readable message. */
char *calc_run(const char *expr, long digits, const char **errmsg);

#endif /* DICTGEN_CALC_H */
