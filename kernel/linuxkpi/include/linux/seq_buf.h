/* LinuxKPI: seq_buf - bounded string building. */
#pragma once
#include <linux/lkpi_all.h>
struct seq_buf { char *buffer; size_t size; size_t len; };
#define DECLARE_SEQ_BUF(NAME, SIZE) \
	char __##NAME##_buffer[SIZE] = ""; \
	struct seq_buf NAME = { .buffer = __##NAME##_buffer, .size = SIZE, .len = 0 }
static inline void seq_buf_init(struct seq_buf *s, char *buf, unsigned int size)
{
	s->buffer = buf;
	s->size = size;
	s->len = 0;
	if (size)
		buf[0] = 0;
}
static inline bool seq_buf_has_overflowed(struct seq_buf *s) { return s->len > s->size; }
static inline const char *seq_buf_str(struct seq_buf *s)
{
	if (s->size)
		s->buffer[s->len < s->size ? s->len : s->size - 1] = 0;
	return s->buffer;
}
static inline int seq_buf_printf(struct seq_buf *s, const char *fmt, ...)
{
	va_list a;
	int n;
	if (s->len >= s->size)
		return -1;
	va_start(a, fmt);
	n = vsnprintf(s->buffer + s->len, s->size - s->len, fmt, a);
	va_end(a);
	s->len += n;
	return s->len < s->size ? 0 : -1;
}
static inline int seq_buf_puts(struct seq_buf *s, const char *str) { return seq_buf_printf(s, "%s", str); }
