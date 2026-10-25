/*
 * Copyright (c) 2026 Joris Vink <joris@sanctorum.se>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>

#include <stdio.h>

#include "vigil.h"

/*
 * Set up buf to append into data, which is len bytes in size.
 */
void
vigil_buf_init(struct vigil_buf *buf, char *data, size_t len)
{
	PRECOND(buf != NULL);
	PRECOND(data != NULL);
	PRECOND(len > 0);

	buf->data = data;
	buf->flags = 0;
	buf->offset = 0;
	buf->length = len;

	buf->data[0] = '\0';
}

/*
 * Append formatted output to buf, once it is full it is marked as such
 * and all further appends are ignored.
 */
void
vigil_buf_appendf(struct vigil_buf *buf, const char *fmt, ...)
{
	int		len;
	va_list		args;
	size_t		avail;

	PRECOND(buf != NULL);
	PRECOND(fmt != NULL);

	if (buf->flags & VIGIL_BUF_FULL)
		return;

	avail = buf->length - buf->offset;

	va_start(args, fmt);
	len = vsnprintf(buf->data + buf->offset, avail, fmt, args);
	va_end(args);

	if (len < 0) {
		buf->flags |= VIGIL_BUF_FULL;
		return;
	}

	if ((size_t)len >= avail) {
		buf->flags |= VIGIL_BUF_FULL;
		return;
	}

	buf->offset += len;
}

/*
 * Append str to buf as a json string, escaping what needs to be escaped.
 */
void
vigil_buf_quote(struct vigil_buf *buf, const char *str)
{
	const unsigned char	*p;

	PRECOND(buf != NULL);
	PRECOND(str != NULL);

	vigil_buf_appendf(buf, "\"");

	for (p = (const unsigned char *)str; *p != '\0'; p++) {
		if (*p == '"' || *p == '\\')
			vigil_buf_appendf(buf, "\\%c", *p);
		else if (*p < 0x20)
			vigil_buf_appendf(buf, "\\u%04x", *p);
		else
			vigil_buf_appendf(buf, "%c", *p);
	}

	vigil_buf_appendf(buf, "\"");
}
