#include "stdio_impl.h"
#include <drivers/console.hh>

// There is no filesystem or file-descriptor table: stdout/stderr are written
// straight to the console.
extern "C"
size_t __stdout_write(FILE *f, const unsigned char *buf, size_t len)
{
	console::write((const char *)f->wbase, f->wpos - f->wbase);
	f->wpos = f->wbase;
	console::write((const char *)buf, len);
	return len;
}
