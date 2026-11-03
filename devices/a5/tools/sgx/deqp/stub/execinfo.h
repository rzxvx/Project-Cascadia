/* musl has no backtrace(): dEQP's crash handler prints none */
#ifndef STUB_EXECINFO_H
#define STUB_EXECINFO_H
static inline int backtrace(void **buf, int n) { (void)buf; (void)n; return 0; }
static inline char **backtrace_symbols(void *const *buf, int n) { (void)buf; (void)n; return 0; }
static inline void backtrace_symbols_fd(void *const *buf, int n, int fd) { (void)buf; (void)n; (void)fd; }
#endif
