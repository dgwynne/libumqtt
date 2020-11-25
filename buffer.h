/* 
 * MIT License
 *
 * Copyright (c) 2019 Jianhui Zhao <jianhuizhao329@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef _BUFFER_H
#define _BUFFER_H

#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <sys/types.h>

/* Test for GCC < 2.96 */
#if __GNUC__ < 2 || (__GNUC__ == 2 && (__GNUC_MINOR__ < 96))
#define __builtin_expect(x) (x)
#endif

#ifndef likely
#define likely(x)	__builtin_expect(!!(x), 1)
#endif

#ifndef unlikely
#define unlikely(x)	__builtin_expect(!!(x), 0)
#endif

enum {
    P_FD_EOF = 0,
    P_FD_ERR = -1,
    P_FD_PENDING = -2
};

struct buffer {
    size_t persistent; /* persistent size */
    uint8_t *head;  /* Head of buffer */
    uint8_t *data;  /* Data head pointer */
    uint8_t *tail;  /* Data tail pointer */
    uint8_t *end;   /* End of buffer */
};

int buffer_init(struct buffer *b, size_t size);
int buffer_resize(struct buffer *b, size_t size);
void buffer_free(struct buffer *b);


/*  Actual data Length */
static inline size_t buffer_length(const struct buffer *b)
{
    return b->tail - b->data;
}

/* The total buffer size  */
static inline size_t buffer_size(const struct buffer *b)
{
    return b->end - b->head;
}

static inline size_t buffer_headroom(const struct buffer *b)
{
    return b->data - b->head;
}

static inline size_t buffer_tailroom(const struct buffer *b)
{
    return b->end - b->tail;
}

static inline void *buffer_data(const struct buffer *b)
{
    return b->data;
}

static inline void buffer_set_persistent_size(struct buffer *b, size_t size)
{
    size_t new_size = getpagesize();

    while (new_size < size)
        new_size <<= 1;

    b->persistent = new_size;
}

static inline void buffer_check_persistent_size(struct buffer *b)
{
    if (b->persistent > 0 &&
        buffer_size(b) > b->persistent && buffer_length(b) < b->persistent)
        buffer_resize(b, b->persistent);
}

/*
 *	buffer_put - add data to a buffer
 *	@b: buffer to use
 *	@len: amount of data to add
 *
 *	This function extends the used data area of the buffer. A pointer to the
 *	first byte of the extra data is returned.
 *  If this would exceed the total buffer size the buffer will grow automatically.
 */
void *buffer_put(struct buffer *b, size_t len);

static inline void *buffer_put_zero(struct buffer *b, size_t len)
{
    void *tmp = buffer_put(b, len);

    if (likely(tmp))
        memset(tmp, 0, len);
    return tmp;
}

static inline void *buffer_put_data(struct buffer *b, const void *data,   size_t len)
{
    void *tmp = buffer_put(b, len);

    if (likely(tmp))
        memcpy(tmp, data, len);
    return tmp;
}


static inline int buffer_put_u8(struct buffer *b, uint8_t val)
{
    uint8_t *p = buffer_put(b, 1);

    if (likely(p)) {
        *p = val;
        return 0;
    }

    return -1;
}

static inline int buffer_put_u16(struct buffer *b, uint16_t val)
{
    uint16_t *p = buffer_put(b, 2);

    if (likely(p)) {
        *p = val;
        return 0;
    }

    return -1;
}

static inline int buffer_put_u32(struct buffer *b, uint32_t val)
{
    uint32_t *p = buffer_put(b, 4);

    if (likely(p)) {
        *p = val;
        return 0;
    }

    return -1;
}

static inline int buffer_put_u64(struct buffer *b, uint64_t val)
{
    uint64_t *p = buffer_put(b, 8);

    if (likely(p)) {
        *p = val;
        return 0;
    }

    return -1;
}

static inline int buffer_put_string(struct buffer *b, const char *s)
{
    size_t len = strlen(s);
    char *p = buffer_put(b, len);

    if (likely(p)) {
        memcpy(p, s, len);
        return 0;
    }

    return -1;
}

int buffer_put_vprintf(struct buffer *b, const char *fmt, va_list ap) __attribute__((format(printf, 2, 0)));
int buffer_put_printf(struct buffer *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/*
 *  buffer_put_fd - Append data from a file to the end of a buffer. The file must be opened in nonblocking.
 *  @fd: file descriptor
 *  @len: how much data to read, or -1 to read as much as possible.
 *  @eof: indicates end of file
 *  @rd: A customized read function. Generally used for SSL.
 *       The customized read function should be return:
 *       P_FD_EOF/P_FD_ERR/P_FD_PENDING or number of bytes read.
 *
 *  Return the number of bytes append
 */
int buffer_put_fd(struct buffer *b, int fd, ssize_t len, bool *eof,
    int (*rd)(int fd, void *buf, size_t count, void *arg), void *arg);

/**
 *	buffer_truncate - remove end from a buffer
 *	@b: buffer to alter
 *	@len: new length
 *
 *	Cut the length of a buffer down by removing data from the tail. If
 *	the buffer is already under the length specified it is not modified.
 */
static inline void buffer_truncate(struct buffer *b, size_t len)
{
    if (buffer_length(b) > len) {
        b->tail = b->data + len;
        buffer_check_persistent_size(b);
    }
}

/*
 *	buffer_pull - remove data from the start of a buffer
 *	@b: buffer to use
 *	@len: amount of data to remove
 *
 *	This function removes data from the start of a buffer,
 *  returning the actual length removed.
 *  Just remove the data if the dest is NULL.
 */
size_t buffer_pull(struct buffer *b, void *dest, size_t len);

static inline uint8_t buffer_pull_u8(struct buffer *b)
{
    uint8_t val = 0;

    if (likely(buffer_length(b) > 0)) {
        val = b->data[0];
        b->data += 1;
    }

    return val;
}

static inline uint16_t buffer_pull_u16(struct buffer *b)
{
    uint16_t val = 0;

    if (likely(buffer_length(b) > 1)) {
        val = *((uint16_t *)b->data);
        b->data += 2;
    }

    return val;
}

static inline uint32_t buffer_pull_u32(struct buffer *b)
{
    uint32_t val = 0;

    if (likely(buffer_length(b) > 3)) {
        val = *((uint32_t *)b->data);
        b->data += 4;
    }

    return val;
}

static inline uint64_t buffer_pull_u64(struct buffer *b)
{
    uint64_t val = 0;

    if (likely(buffer_length(b) > 7)) {
        val = *((uint64_t *)b->data);
        b->data += 8;
    }

    return val;
}

static inline uint8_t buffer_get_u8(struct buffer *b, ssize_t offset)
{
    uint8_t val = 0;

    if (likely(buffer_length(b) > offset))
        val = b->data[offset];

    return val;
}

static inline uint16_t buffer_get_u16(struct buffer *b, ssize_t offset)
{
    uint16_t val = 0;

    if (likely(buffer_length(b) > offset + 1))
        val = *((uint16_t *)(b->data + offset));

    return val;
}

static inline uint32_t buffer_get_u32(struct buffer *b, ssize_t offset)
{
    uint32_t val = 0;

    if (likely(buffer_length(b) > offset + 3))
        val = *((uint32_t *)(b->data + offset));

    return val;
}

static inline uint64_t buffer_get_u64(struct buffer *b, ssize_t offset)
{
    uint64_t val = 0;

    if (likely(buffer_length(b) > offset + 7))
        val = *((uint64_t *)(b->data + offset));

    return val;
}

/*
 *  buffer_pull_to_fd - remove data from the start of a buffer and write to a file
 *  @fd: file descriptor
 *  @len: how much data to remove, or -1 to remove as much as possible.
 *  @wr: A customized write function. Generally used for SSL.
 *       The customized write function should be return:
 *       P_FD_EOF/P_FD_ERR/P_FD_PENDING or number of bytes write.
 *
 *  Return the number of bytes removed
 */
int buffer_pull_to_fd(struct buffer *b, int fd, size_t len,
    int (*wr)(int fd, void *buf, size_t count, void *arg), void *arg);

void buffer_hexdump(struct buffer *b, size_t offset, size_t len);

/*
 *	buffer_find - finds the start of the first occurrence of the sep of length seplen in the buffer
 *  @limit: 0 indicates unlimited
 *	Return -1 if sep is not present in the buffer
 */
int buffer_find(struct buffer *b, size_t offset, size_t limit, void *sep, size_t seplen);

#endif
