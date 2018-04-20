/*
 * Copyright © 2013-2014 Collabora, Ltd.
 * Copyright © 2016 DENSO CORPORATION
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifndef MARSHALLER_H
#define MARSHALLER_H

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <errno.h>
#include <signal.h>

#include "message.h"
#include "marshaller_log.h"

static inline int send_all (int sock, const struct iovec *iov, int iovcnt)
{
   int ret;

   signal(SIGPIPE, SIG_IGN);

   do {
      if (errno == EPIPE) {
         fprintf(stderr, "send_all %d : %s\n", ret, strerror(errno));
         errno = 0;
         ret = 0;
      }
      ret = writev (sock, iov, iovcnt);
   } while (ret == -1 && errno == EINTR);

   return ret;
}

static inline int recv_all (int sock, struct iovec *iov, int iovcnt)
{
   ssize_t ret;

   do {
      ret = readv (sock, iov, iovcnt);

      while (ret > 0) {
         if ((size_t) ret < iov[0].iov_len) {
            iov[0].iov_base = ((uint8_t *) iov[0].iov_base) + ret;
            iov[0].iov_len -= ret;
            break;
         }
         ret -= iov[0].iov_len;
         iov++;
         iovcnt--;
      }

   } while ((ret == -1 && errno == EINTR) || iovcnt > 0);

   return ret;
}

#define ABORT_CALL_NR() { \
   ABORT_TIMING(""); \
   return; \
}

#define ABORT_CALL(ret, _fmt, ...) { \
   ABORT_TIMING(_fmt, ## __VA_ARGS__); \
   return ret; \
}

#define PADDED(sz) \
   (((sz) + 3) & ~3)

#define START_MESSAGE(conn, name, sz, opcode) \
   const char *msg_name __attribute__((unused)) = name; \
   hdr_t hdr = { 0, sz, opcode, 0 }; \
   int send_ret; \
   struct iovec marshaller_params[16]; \
   int marshaller_paramid = 1; \
   int param_padding __attribute__((unused)) = 0; \
   wth_connection_start_write(conn, &hdr, sz);  \
   DEBUG_STAMP (); \
   STREAM_DEBUG ((unsigned char *) &hdr, sizeof (hdr), "header -> ");

#define ADD_PADDING(conn, sz)		 \
   param_padding = (4 - ((sz) & 3)) & 3; \
   if (param_padding > 0) { \
      wth_buffer_put(conn, (void *) &marshaller_paramid, param_padding); \
      marshaller_paramid++; \
   }

#define SERIALIZE_PARAM(conn, param, sz)				    \
   wth_buffer_put(conn, (void *) param, sz); \
   ADD_PADDING (conn, sz);					\
   STREAM_DEBUG ((unsigned char *) param, sz, "param  -> ");

#define SERIALIZE_PARAM_SILENT(conn, param, sz)		   \
   wth_buffer_put(conn, (void *) param, sz); \
   ADD_PADDING (conn, sz);			\
   STREAM_DEBUG_DATA (param, sz);

#define SERIALIZE_DATA(conn, data, sz)				  \
   wth_buffer_put(conn, (void *) &sz, sizeof (int)); \
   STREAM_DEBUG ((unsigned char *) &sz, sizeof (int), "data sz   -> "); \
   wth_buffer_put(conn, (void *) data, sz); \
   ADD_PADDING (conn, sz);					\
   STREAM_DEBUG ((unsigned char *) data, sz, "data   -> ");

#define SERIALIZE_ARRAY(conn, array) \
   wth_buffer_put(conn, (void *) &array->size, sizeof (size_t));  \
   STREAM_DEBUG ((unsigned char *) &sz, sizeof (int), "array sz  -> "); \
   wth_buffer_put(conn, array->data, array->size);  \
   ADD_PADDING (conn, array->size); \
   STREAM_DEBUG ((unsigned char *) array->data, array->size, "array  -> ");

#define FLUSH_RECV() \
   if (marshaller_paramid > 0) { \
      int recv_ret; \
      recv_ret = recv_all (marshaller_get_fd (), marshaller_params, marshaller_paramid); \
      if (recv_ret == -1) \
         exit (errno); \
      marshaller_paramid = 0; \
   }

#define SKIP_PADDING(sz) \
   param_padding = (4 - ((sz) & 3)) & 3; \
   if (param_padding > 0) { \
      marshaller_params[marshaller_paramid].iov_base = (void *) &padding_buffer; \
      marshaller_params[marshaller_paramid].iov_len = param_padding; \
      marshaller_paramid++; \
   }

#define DESERIALIZE_PARAM(param, sz) \
   marshaller_params[marshaller_paramid].iov_base = (void *) param; \
   marshaller_params[marshaller_paramid].iov_len = sz; \
   marshaller_paramid++; \
   SKIP_PADDING(sz);

#define DESERIALIZE_PARAM_SILENT(param, sz) \
   marshaller_params[marshaller_paramid].iov_base = (void *) param; \
   marshaller_params[marshaller_paramid].iov_len = sz; \
   marshaller_paramid++; \
   SKIP_PADDING(sz);

#define DESERIALIZE_DATA(data) \
   marshaller_params[marshaller_paramid].iov_base = (void *) &data_sz; \
   marshaller_params[marshaller_paramid].iov_len = sizeof (data_sz); \
   marshaller_paramid++; \
   FLUSH_RECV(); \
   if (data_sz > 0) { \
      DESERIALIZE_PARAM (data, data_sz) \
   }

#define DESERIALIZE_OPTIONAL_PARAM(param, sz) \
   if (param) { \
      DESERIALIZE_PARAM (param, sz); \
   } else { \
      DESERIALIZE_PARAM (recv_buffer, sz); \
   }

#endif
