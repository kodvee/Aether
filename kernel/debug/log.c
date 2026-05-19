#include <kernel/panic.h>
#include <kernel/kprintf.h>
#include <stdarg.h>
#include <deps/printf.h>

void klog(log_severity_t sev, const char *subsys, const char *fmt, ...) {
    static const char *const prefixes[] = {
        "[DBG] ", "[INF] ", "[WRN] ", "[ERR] ",
    };
    const char *pfx = (sev <= LOG_ERROR) ? prefixes[(int)sev] : "[???] ";

    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    kprintf("%s%s: %.*s\n", pfx, subsys ? subsys : "?", n > 0 ? n : 0, buf);
}
