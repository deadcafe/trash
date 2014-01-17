/*
 * bi-direction fifo
 * exec function by peer thread context
 */

#include <sys/types.h>
#include <sys/eventfd.h>

#include <assert.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include <trema.h>

#include "memory_barrier.h"
#include "bd_fifo.h"

#define	WMB()	write_memory_barrier()
#define	RMB()	read_memory_barrier()

#define	MALLOC(s)	xmalloc(s)
#define	FREE(p)		xfree(p)


enum {
  STATE_INVALID = 0,
  STATE_READY,
};

typedef struct {
  void *arg;
  void (*cb)(void *);
} bd_fifo_msg_t;


static inline void
CLOSE(int fd)
{
  int err;

  do {
    err = 0;
    if (close(fd) < 0)
      err = errno;
  } while (err == EINTR);
}

static bool
ring_bell(dir_info_t *info)
{
  if (writable_safe(info->fd)) {
    int err = 0;

    while (!err && info->val) {
      if (eventfd_write(info->fd, info->val) < 0) {
        err = errno;
        if (err == EINTR) {
          err = 0;
        } else if (err == EAGAIN) {
          break;
        } else {
          /* major error */
          set_writable_safe(info->fd, false);
          return false;
        }
      } else {
        info->val = 0;
        set_writable_safe(info->fd, false);
      }
    }
  } else {
    if (info->val)
      set_writable_safe(info->fd, true);
    else
      set_writable_safe(info->fd, false);
  }
  return true;
}


static void
read_cb(int fd, void *arg)
{
  dir_info_t *info = arg;
  bd_fifo_msg_t *msg;
  assert(fd == info->fd);

  while (1) {
    eventfd_t val;

    if (eventfd_read(info->fd, &val) < 0) {
      if (errno != EINTR)
        break;
    }
  }

  while ((msg = dequeue(info->queue)) != NULL) {
    msg->cb(msg->arg);
    FREE(msg);
  }
}


static void
write_cb(int fd, void *arg)
{
  dir_info_t *info = arg;
  assert(fd == info->fd);

  ring_bell(info);
}

static void
null_cb(int fd __attribute__((unused)), void *arg __attribute__((unused)))
{
  /* nothing to do */
  ;
}


static bool
ringer_fifo( dir_info_t *info,
             void (*cb)( void * ),
             void *arg ) {
  bd_fifo_msg_t *msg;
  assert(cb);

  size_t state = *info->state_p;
  RMB();

  if (state != STATE_READY)
    return false;

  if ((msg = MALLOC(sizeof(*msg))) != NULL) {
    msg->cb = cb;
    msg->arg = arg;

    enqueue( info->queue, msg );
    info->val++;

    return ring_bell(info);
  }
  return false;
}


/*
 * called by client
 */
bool
fifo_to_server( bd_fifo_t *fifo,
                void (*cb)( void * ),
                void *arg )
{
  return ringer_fifo(&fifo->info[DIR_TO_SERVER], cb, arg);
}


/*
 * called by server;
 */
bool
fifo_to_client( bd_fifo_t *fifo,
                void (*cb)( void * ),
                void *arg )
{
  return ringer_fifo(&fifo->info[DIR_TO_CLIENT], cb, arg);
}


static bool
fifo_bind_info( dir_info_t *info )
{
  assert(info->queue == NULL);
  assert(info->fd >= 0);
  assert(info->another->fd >= 0);

  queue *queue = create_queue();
  if (!queue)
    return false;
  info->queue = queue;
  WMB();

  set_fd_handler_safe(info->fd, read_cb, info, null_cb, NULL);
  set_readable_safe(info->fd, true);
  set_writable_safe(info->fd, false);

  set_fd_handler_safe(info->another->fd, null_cb, NULL, write_cb, info->another);
  set_readable_safe(info->another->fd, false);
  set_writable_safe(info->another->fd, false);

  queue = info->another->queue;
  RMB();
  if (queue) {
    *info->state_p = STATE_READY;
    WMB();
  }
  return true;
}


bool
fifo_bind_server( bd_fifo_t *fifo )
{
  return fifo_bind_info( &fifo->info[DIR_TO_SERVER] );
}


bool
fifo_bind_clent( bd_fifo_t *fifo )
{
  return fifo_bind_info( &fifo->info[DIR_TO_CLIENT] );
}


static void
fifo_detach_info(dir_info_t *info )
{
  bd_fifo_msg_t *msg;

  *info->state_p = STATE_INVALID;
  WMB();

  queue *queue = info->queue;
  int fd = info->fd;
  int another_fd = info->another->fd;
  info->queue = NULL;
  info->val = 0;
  WMB();

  set_readable_safe(fd, false);
  set_writable_safe(fd, false);
  delete_fd_handler_safe(fd);

  set_readable_safe(another_fd, false);
  set_writable_safe(another_fd, false);
  delete_fd_handler_safe(another_fd);

  while ((msg = dequeue(queue)) != NULL) {
    msg->cb(msg->arg);
    FREE(msg);
  }
  delete_queue(queue);
}


void
fifo_detach_server(bd_fifo_t *fifo)
{
  fifo_detach_info(&fifo->info[DIR_TO_SERVER]);
}


void
fifo_detach_client(bd_fifo_t *fifo)
{
  fifo_detach_info(&fifo->info[DIR_TO_CLIENT]);
}


/*****************************************************************
 * main thread (client side) functions
 *****************************************************************/
void
fifo_destroy( bd_fifo_t *fifo )
{
  int i;
  assert(fifo->state == STATE_INVALID);

  for (i = 0; i < DIR_NUM; i++) {
    if (fifo->info[i].queue) {

      assert(fifo->info[i].queue->length == 0);

      delete_queue(fifo->info[i].queue);
      fifo->info[i].queue = NULL;
    }

    if (fifo->info[i].fd >= 0) {
      CLOSE(fifo->info[i].fd);
      fifo->info[i].fd = -1;
    }
  }
  FREE(fifo);
}


bd_fifo_t *
fifo_create( void )
{
  bd_fifo_t *fifo;

  fifo = MALLOC(sizeof(*fifo));
  if (fifo) {
    int fd;

    memset(fifo, 0, sizeof(*fifo));
    fifo->info[DIR_TO_SERVER].fd = -1;
    fifo->info[DIR_TO_CLIENT].fd = -1;
    fifo->info[DIR_TO_SERVER].state_p = &fifo->state;
    fifo->info[DIR_TO_CLIENT].state_p = &fifo->state;
    fifo->info[DIR_TO_SERVER].another = &fifo->info[DIR_TO_CLIENT];
    fifo->info[DIR_TO_CLIENT].another = &fifo->info[DIR_TO_SERVER];

    if ((fd = eventfd(0, EFD_NONBLOCK)) < 0) {
      fifo_destroy(fifo);
      return NULL;
    }
    fifo->info[DIR_TO_SERVER].fd = fd;

    if ((fd = eventfd(0, EFD_NONBLOCK)) < 0) {
      fifo_destroy(fifo);
      return NULL;
    }
    fifo->info[DIR_TO_CLIENT].fd = fd;
  }
  return fifo;
}


