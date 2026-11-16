#include <lib/format.hpp>
#include <lib/string.hpp>

namespace {

struct Spec {
    bool left   = false; // '-'
    bool plus   = false; // '+'
    bool space  = false; // ' '
    bool alt    = false; // '#'
    bool zero   = false; // '0'
    int width     = 0;
    int precision = -1;
};

struct Output {
    FormatSink sink;
    void* ctx;
    int count = 0;

    void put(char c) {
        sink(c, ctx);
        count++;
    }
    void pad(char c, int n) {
        while (n-- > 0)
            put(c);
    }
};

void format_integer(Output& out, u64 value, bool negative, unsigned base, bool upper, const Spec& spec) {
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char buf[64];
    int len = 0;

    // C rule: zero with an explicit precision of 0 prints no digits.
    if (value == 0 && spec.precision != 0)
        buf[len++] = '0';
    while (value) {
        buf[len++] = digits[value % base];
        value /= base;
    }

    const char* prefix = "";
    if (spec.alt) {
        if (base == 16) prefix = upper ? "0X" : "0x";
        else if (base == 2) prefix = "0b";
        else if (base == 8) prefix = "0";
    }

    char sign = negative ? '-' : spec.plus ? '+' : spec.space ? ' ' : 0;
    int zeros = spec.precision > len ? spec.precision - len : 0;
    int total = len + zeros + (sign ? 1 : 0) + static_cast<int>(strlen(prefix));
    int padding = spec.width > total ? spec.width - total : 0;
    bool zero_pad = spec.zero && !spec.left && spec.precision < 0;

    if (!spec.left && !zero_pad)
        out.pad(' ', padding);
    if (sign)
        out.put(sign);
    for (const char* p = prefix; *p; p++)
        out.put(*p);
    if (zero_pad)
        out.pad('0', padding);
    out.pad('0', zeros);
    while (len)
        out.put(buf[--len]);
    if (spec.left)
        out.pad(' ', padding);
}

void format_string(Output& out, const char* s, const Spec& spec) {
    if (!s)
        s = "(null)";
    int len = 0;
    while (s[len] && (spec.precision < 0 || len < spec.precision))
        len++;
    int padding = spec.width > len ? spec.width - len : 0;
    if (!spec.left)
        out.pad(' ', padding);
    for (int i = 0; i < len; i++)
        out.put(s[i]);
    if (spec.left)
        out.pad(' ', padding);
}

enum class Length { None, Char, Short, Long, LongLong, Size };

} // namespace

int vformat(FormatSink sink, void* ctx, const char* fmt, va_list args) {
    Output out{sink, ctx};

    while (*fmt) {
        if (*fmt != '%') {
            out.put(*fmt++);
            continue;
        }
        fmt++;

        Spec spec;
        for (bool more = true; more;) {
            switch (*fmt) {
            case '-': spec.left = true; fmt++; break;
            case '+': spec.plus = true; fmt++; break;
            case ' ': spec.space = true; fmt++; break;
            case '#': spec.alt = true; fmt++; break;
            case '0': spec.zero = true; fmt++; break;
            default: more = false;
            }
        }

        if (*fmt == '*') {
            spec.width = va_arg(args, int);
            if (spec.width < 0) {
                spec.left = true;
                spec.width = -spec.width;
            }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9')
                spec.width = spec.width * 10 + (*fmt++ - '0');
        }

        if (*fmt == '.') {
            fmt++;
            spec.precision = 0;
            if (*fmt == '*') {
                spec.precision = va_arg(args, int);
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9')
                    spec.precision = spec.precision * 10 + (*fmt++ - '0');
            }
        }

        Length length = Length::None;
        switch (*fmt) {
        case 'h':
            fmt++;
            if (*fmt == 'h') { fmt++; length = Length::Char; }
            else length = Length::Short;
            break;
        case 'l':
            fmt++;
            if (*fmt == 'l') { fmt++; length = Length::LongLong; }
            else length = Length::Long;
            break;
        case 'z': case 't': case 'j':
            fmt++;
            length = Length::Size;
            break;
        default: break;
        }

        auto fetch_signed = [&]() -> i64 {
            switch (length) {
            case Length::Char:     return static_cast<signed char>(va_arg(args, int));
            case Length::Short:    return static_cast<short>(va_arg(args, int));
            case Length::Long:     return va_arg(args, long);
            case Length::LongLong: return va_arg(args, long long);
            case Length::Size:     return va_arg(args, isize);
            default:               return va_arg(args, int);
            }
        };
        auto fetch_unsigned = [&]() -> u64 {
            switch (length) {
            case Length::Char:     return static_cast<unsigned char>(va_arg(args, unsigned));
            case Length::Short:    return static_cast<unsigned short>(va_arg(args, unsigned));
            case Length::Long:     return va_arg(args, unsigned long);
            case Length::LongLong: return va_arg(args, unsigned long long);
            case Length::Size:     return va_arg(args, usize);
            default:               return va_arg(args, unsigned);
            }
        };

        char conv = *fmt;
        if (conv)
            fmt++;

        switch (conv) {
        case 'd':
        case 'i': {
            i64 v = fetch_signed();
            bool neg = v < 0;
            u64 mag = neg ? 0 - static_cast<u64>(v) : static_cast<u64>(v);
            format_integer(out, mag, neg, 10, false, spec);
            break;
        }
        case 'u': format_integer(out, fetch_unsigned(), false, 10, false, spec); break;
        case 'x': format_integer(out, fetch_unsigned(), false, 16, false, spec); break;
        case 'X': format_integer(out, fetch_unsigned(), false, 16, true, spec); break;
        case 'o': format_integer(out, fetch_unsigned(), false, 8, false, spec); break;
        case 'b': format_integer(out, fetch_unsigned(), false, 2, false, spec); break;
        case 'p': {
            spec.alt = true;
            spec.precision = 16;
            format_integer(out, reinterpret_cast<uptr>(va_arg(args, void*)), false, 16, false, spec);
            break;
        }
        case 's': format_string(out, va_arg(args, const char*), spec); break;
        case 'c': {
            char buf[2] = {static_cast<char>(va_arg(args, int)), 0};
            spec.precision = 1;
            format_string(out, buf, spec);
            break;
        }
        case '%': out.put('%'); break;
        case 0: break;
        default:
            out.put('%');
            out.put(conv);
            break;
        }
    }
    return out.count;
}

namespace {
struct BufferCtx {
    char* buf;
    usize size;
    usize pos;
};

void buffer_sink(char c, void* ctx) {
    auto b = static_cast<BufferCtx*>(ctx);
    if (b->pos + 1 < b->size)
        b->buf[b->pos] = c;
    b->pos++;
}
} // namespace

int vsnprintf(char* buf, usize size, const char* fmt, va_list args) {
    BufferCtx ctx{buf, size, 0};
    int n = vformat(buffer_sink, &ctx, fmt, args);
    if (size)
        buf[min(ctx.pos, size - 1)] = 0;
    return n;
}

int snprintf(char* buf, usize size, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, size, fmt, args);
    va_end(args);
    return n;
}
