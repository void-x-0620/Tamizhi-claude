/* tz_std.h — Tamizhi standard library (compiled into libtamizhi.a).
 * Strings returned from here are malloc'd and live until exit.
 * Installed as <prefix>/include/tamizhi/nool/tz_std.h. */
#ifndef TZ_STD_H
#define TZ_STD_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

extern int tz_argc;
extern char **tz_argv;

char *tz_dup(const char *s, size_t n);
const char *tz_read_line(void);
long long tz_read_int(void);
double tz_read_float(void);
const char *tz_read_file(const char *path);
long long tz_write_file(const char *p, const char *t);
long long tz_append_file(const char *p, const char *t);
long long tz_file_exists(const char *p);
long long tz_str_len(const char *s);
const char *tz_str_sub(const char *s, long long start, long long len);
long long tz_str_find(const char *s, const char *needle);
const char *tz_str_upper(const char *s);
const char *tz_str_lower(const char *s);
const char *tz_str_trim(const char *s);
long long tz_str_starts(const char *s, const char *p);
long long tz_str_ends(const char *s, const char *p);
const char *tz_str_replace(const char *s, const char *from, const char *to);
const char *tz_str_repeat(const char *s, long long n);
const char *tz_chr(long long c);
const char *tz_int_to_str(long long v);
const char *tz_float_to_str(double v);
long long tz_parse_int(const char *s);
double tz_parse_float(const char *s);
double tz_sqrt(double x);
double tz_pow(double x, double y);
double tz_sin(double x);
double tz_cos(double x);
double tz_tan(double x);
double tz_log(double x);
double tz_exp(double x);
double tz_floor(double x);
double tz_ceil(double x);
double tz_round(double x);
double tz_fabs(double x);
long long tz_abs(long long x);
long long tz_min(long long a, long long b);
long long tz_max(long long a, long long b);
double tz_pi(void);
long long tz_arg_count(void);
const char *tz_arg_get(long long i);
void tz_exit_program(long long code);
const char *tz_env_get(const char *n);
long long tz_clock_ms(void);
long long tz_time_now(void);
void tz_sleep_ms(long long ms);
void tz_rand_seed(long long n);
long long tz_rand_int(long long lo, long long hi);
double tz_rand_float(void);
/* ---- io (more) ---- */
const char *tz_read_all(void);
void tz_print_str(const char *s);
void tz_eprint(const char *s);
long long tz_remove_file(const char *p);
/* ---- string (more) ---- */
long long tz_char_at(const char *s, long long i);
long long tz_ord(const char *s);
const char *tz_str_reverse(const char *s);
long long tz_str_count(const char *s, const char *sub);
const char *tz_str_pad_left(const char *s, long long w, const char *fill);
const char *tz_str_pad_right(const char *s, long long w, const char *fill);
long long tz_str_cmp(const char *a, const char *b);
long long tz_str_is_digits(const char *s);
long long tz_str_is_alpha(const char *s);
const char *tz_str_capitalize(const char *s);
const char *tz_to_hex(long long v);
const char *tz_to_bin(long long v);
long long tz_parse_int_base(const char *s, long long base);
/* ---- math (more) ---- */
double tz_asin(double x); double tz_acos(double x); double tz_atan(double x);
double tz_atan2(double y, double x);
double tz_sinh(double x); double tz_cosh(double x); double tz_tanh(double x);
double tz_log10(double x); double tz_log2(double x);
double tz_hypot(double x, double y); double tz_trunc(double x);
double tz_fmod(double x, double y); double tz_cbrt(double x);
double tz_e(void); double tz_inf(void);
long long tz_is_nan(double x);
double tz_deg_to_rad(double d); double tz_rad_to_deg(double r);
double tz_minf(double a, double b); double tz_maxf(double a, double b);
long long tz_clamp(long long v, long long lo, long long hi);
double tz_clampf(double v, double lo, double hi);
long long tz_sign(long long v);
long long tz_gcd(long long a, long long b);
long long tz_lcm(long long a, long long b);
long long tz_ipow(long long b, long long e);
long long tz_factorial(long long n);
long long tz_is_prime(long long n);
/* ---- time (more) ---- */
const char *tz_format_time(long long ts, const char *fmt);
const char *tz_now_iso(void);
long long tz_time_part(long long ts, long long which);
long long tz_year(long long ts); long long tz_month(long long ts); long long tz_day(long long ts);
long long tz_hour(long long ts); long long tz_minute(long long ts); long long tz_second(long long ts);
long long tz_weekday(long long ts);
void tz_rand_seed_now(void);
/* ---- fs ---- */
long long tz_file_size(const char *p);
long long tz_is_dir(const char *p);
long long tz_is_file(const char *p);
long long tz_rename_file(const char *a, const char *b);
long long tz_copy_file(const char *a, const char *b);
long long tz_make_dir(const char *p);
long long tz_remove_dir(const char *p);
const char *tz_list_dir(const char *p);
const char *tz_current_dir(void);
long long tz_change_dir(const char *p);
const char *tz_home_dir(void);
const char *tz_temp_dir(void);
const char *tz_path_join(const char *a, const char *b);
const char *tz_path_base(const char *p);
const char *tz_path_dir(const char *p);
const char *tz_path_ext(const char *p);
/* ---- process ---- */
long long tz_run_command(const char *cmd);
const char *tz_capture_command(const char *cmd);
long long tz_get_pid(void);
const char *tz_hostname(void);
/* ---- encoding and hashing ---- */
long long tz_hash_djb2(const char *s);
long long tz_hash_fnv1a(const char *s);
long long tz_crc32(const char *s);
const char *tz_sha256(const char *s);
const char *tz_base64_encode(const char *s);
const char *tz_base64_decode(const char *s);
const char *tz_hex_encode(const char *s);
const char *tz_hex_decode(const char *s);
const char *tz_url_encode(const char *s);
const char *tz_url_decode(const char *s);
/* ---- regex (POSIX extended) ---- */
long long tz_regex_match(const char *pattern, const char *s);
const char *tz_regex_find(const char *pattern, const char *s);
const char *tz_regex_replace(const char *pattern, const char *s, const char *repl);

#ifdef __cplusplus
}
#endif
#endif
