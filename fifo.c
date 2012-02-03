#include "fifo.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <string.h>

struct maru_fifo
{
   /** The underlying ring buffer. */
   uint8_t *buffer;

   /** Hold the total allocated size of the buffer. */
   size_t buffer_size;

   /** A bitmask to wrap around the pointers.
    * As buffer is power-of-two sized, a simple AND will work.
    */
   size_t buffer_mask;

   /** Holds the beginning of the locked read region.
    * If no reading lock is held, read_lock_begin will equal read_lock_end. */
   size_t read_lock_begin;

   /** Holds the end of the locked read region.
    * If no reading lock is held, read_lock_begin will equal read_lock_end. */
   size_t read_lock_end;

   /** Holds the beginning of the locked write region.
    * If no writer lock is held, write_lock_begin will equal write_lock_end. */
   size_t write_lock_begin;

   /** Holds the end of the locked write region.
    * If no write lock is held, write_lock_begin will equal write_lock_end. */
   size_t write_lock_end;

   /** Notification pipes for writer side.
    * Dummy bytes will be written into write_pipe[1] to notify about
    * data being available, to be polled by write_pipe[0]. */
   maru_fd write_fd[2];

   /** Notification pipes for reader side.
    * Dummy bytes will be written into read_pipe[1] to notify about
    * data being available, to be polled by read_pipe[0]. */
   maru_fd read_fd[2];
};

void maru_fifo_free(maru_fifo *fifo)
{
   if (!fifo)
      return;

   maru_fifo_kill_notification(fifo);

   free(fifo->buffer);
   free(fifo);
}

static size_t next_pow2(size_t v)
{
   v--;
   v |= v >> 1;
   v |= v >> 2;
   v |= v >> 4;
#if SIZE_MAX >= UINT16_C(0xffff)
      v |= v >> 8;
#endif
#if SIZE_MAX >= UINT32_C(0xffffffff)
      v |= v >> 16;
#endif
#if SIZE_MAX >= UINT64_C(0xffffffffffffffff)
      v |= v >> 32;
#endif
   v++;

   return v;
}

maru_fifo *maru_fifo_new(size_t size)
{
   if (!size)
      return NULL;

   size = next_pow2(size);

   maru_fifo *fifo = calloc(1, sizeof(*fifo));
   if (!fifo)
      goto error;

   fifo->write_fd[0] = fifo->write_fd[1] =
      fifo->read_fd[0] = fifo->read_fd[1] = -1;

   fifo->buffer_size = size;
   fifo->buffer_mask = size - 1;

   fifo->buffer = calloc(1, size);
   if (!fifo->buffer)
      goto error;

   if (pipe(fifo->write_fd) < 0)
      goto error;
   if (pipe(fifo->read_fd) < 0)
      goto error;

   const int fds[4] = {
      fifo->write_fd[0],
      fifo->write_fd[1],
      fifo->read_fd[0],
      fifo->read_fd[1],
   };

   // Nonblock to avoid a theoretically
   // possible scenario where we block when notifying
   // reader/writer side.
   // Also makes implementation simpler.
   for (unsigned i = 0; i < 4; i++)
   {
      if (fcntl(fds[i], F_SETFL,
               fcntl(fds[i], F_GETFL) | O_NONBLOCK) < 0)
         goto error;
   }

   // Disable SIGPIPE for the off-chance that SIGPIPE kills our application when we're killing notification handles.
   struct sigaction sa = { .sa_handler = SIG_IGN };
   sigaction(SIGPIPE, &sa, NULL);

   return fifo;

error:
   maru_fifo_free(fifo);
   return NULL;
}

maru_fd maru_fifo_write_notify_fd(maru_fifo *fifo)
{
   return fifo->write_fd[0];
}

maru_fd maru_fifo_read_notify_fd(maru_fifo *fifo)
{
   return fifo->read_fd[0];
}

size_t maru_fifo_read_avail(maru_fifo *fifo)
{
   return (fifo->write_lock_begin + fifo->buffer_size - fifo->read_lock_end) & fifo->buffer_mask;
}

size_t maru_fifo_write_avail(maru_fifo *fifo)
{
   return (fifo->read_lock_begin + fifo->buffer_size - fifo_write_lock_end - 1) & fifo->buffer_mask;
}

maru_error maru_fifo_write_lock(maru_fifo *fifo,
      size_t size, struct maru_fifo_locked_region *region)
{
   size_t avail_first = fifo->buffer_size - fifo->write_lock_end;
   size_t write_first = size;
   if (write_first > avail_first)
      write_first = avail_first;
   size_t write_second = size - write_first;

   region->first = fifo->buffer + fifo->write_lock_end;
   region->first_size = write_first;
   region->second = write_second ? fifo->buffer : NULL;
   region->second_size = write_second;

   if (region->second_size)
      fifo->write_lock_end = region->second_size;
   else
      fifo->write_lock_end += region->first_size;

   return LIBMARU_SUCCESS;
}

maru_error maru_fifo_write_unlock(maru_fifo *fifo,
      const struct maru_fifo_locked_region *region)
{
   // Check if ordering of unlocks differ from order of locks.
   if (fifo->buffer + fifo->write_lock_begin != region->first)
      return LIBMARU_ERROR_INVALID;

   size_t new_begin = (fifo->write_lock_begin + region->first_size) & fifo->buffer_mask;

   if (region->second_size && new_begin != 0)
      return LIBMARU_ERROR_INVALID;

   new_begin += region->second_size;
   fifo->write_lock_begin = new_begin;

   // Signal reader that there is new data to be read.
   if (write(fifo->read_fd[1], (uint8_t[]) {0}, 1) < 0 &&
         errno != EAGAIN)
      return LIBMARU_ERROR_IO;

   return LIBMARU_SUCCESS;
}

maru_error maru_fifo_read_lock(maru_fifo *fifo,
      size_t size, struct maru_fifo_locked_region *region)
{
   size_t avail_first = fifo->buffer_size - fifo->read_lock_end;
   size_t read_first = size;
   if (read_first > avail_first)
      read_first = avail_first;
   size_t read_second = size - read_first;

   region->first = fifo->buffer + fifo->read_lock_end;
   region->first_size = read_first;
   region->second = read_second ? fifo->buffer : NULL;
   region->second_size = read_second;

   if (region->second_size)
      fifo->read_lock_end = region->second_size;
   else
      fifo->read_lock_end += region->first_size;

   return LIBMARU_SUCCESS;
}

maru_error maru_fifo_read_unlock(maru_fifo *fifo,
      const struct maru_fifo_locked_region *region)
{
   // Check if ordering of unlocks differ from order of locks.
   if (fifo->buffer + fifo->read_lock_begin != region->first)
      return LIBMARU_ERROR_INVALID;

   size_t new_begin = (fifo->read_lock_begin + region->first_size) & fifo->buffer_mask;

   if (region->second_size && new_begin != 0)
      return LIBMARU_ERROR_INVALID;

   new_begin += region->second_size;
   fifo->read_lock_begin = new_begin;

   // Signal writer that there is data available for writing.
   if (write(fifo->write_fd[1], (uint8_t[]) {0}, 1) < 0 &&
         errno != EAGAIN)
      return LIBMARU_ERROR_IO;

   return LIBMARU_SUCCESS;
}

ssize_t maru_fifo_write(maru_fifo *fifo,
      const void *data_, size_t size)
{
   const uint8_t *data = data_;

   size_t write_avail = maru_fifo_write_avail(fifo);
   if (size > write_avail)
      size = write_avail;

   struct maru_fifo_locked_region region;
   if (maru_fifo_write_lock(fifo, size, &region) != LIBMARU_SUCCESS)
      return -1;

   memcpy(region.first, data, region.first_size);
   memcpy(region.second, data + region.first_size, region.second_size);

   if (maru_fifo_write_unlock(fifo, &region) != LIBMARU_SUCCESS)
      return -1;

   return size;
}

ssize_t maru_fifo_read(maru_fifo *fifo, void *data_, size_t size)
{
   uint8_t *data = data_;

   size_t read_avail = maru_fifo_read_avail(fifo);
   if (size > read_avail)
      size = read_avail;

   struct maru_fifo_locked_region region;
   if (maru_fifo_read_lock(fifo, size, &region) != LIBMARU_SUCCESS)
      return -1;

   memcpy(data, region.first, region.first_size);
   memcpy(data + region.first_size, region.second, region.second_size);

   if (maru_fifo_read_unlock(fifo, &region) != LIBMARU_SUCCESS)
      return -1;

   return size;
}

void maru_fifo_read_notify_ack(maru_fifo *fifo)
{
   char dummy[1024];
   // Flush out all data.
   while (read(fifo->read_fd[0], dummy, sizeof(dummy)) > 0);
   // If there is still data to be read, poll() should give POLLIN.
   if (maru_fifo_read_avail(fifo) > 0)
      write(fifo->read_fd[1], (uint8_t[]) {0}, 1);
}

void maru_fifo_write_notify_ack(maru_fifo *fifo)
{
   char dummy[1024];
   // Flush out all data.
   while (read(fifo->write_fd[0], dummy, sizeof(dummy)) > 0);
   // If there is still data to write, poll() should give POLLIN.
   if (maru_fifo_write_avail(fifo) > 0)
      write(fifo->write_fd[1], (uint8_t[]) {0}, 1);
}

void maru_fifo_kill_notification(maru_fifo *fifo)
{
   if (fifo->write_fd[0] >= 0)
      close(fifo->write_fd[0]);
   if (fifo->write_fd[1] >= 0)
      close(fifo->write_fd[1]);
   if (fifo->read_fd[0] >= 0)
      close(fifo->read_fd[0]);
   if (fifo->read_fd[1] >= 0)
      close(fifo->read_fd[1]);

   fifo->write_fd[0] = fifo->write_fd[1] =
      fifo->read_fd[0] = fifo->read_fd[1] = -1;
}

