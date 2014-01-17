/*
 * bi-direction fifo
 */

#include <sys/types.h>
#include <sys/eventfd.h>

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include <event_handler.h>

#include "queue.h"


#define	MALLOC(s)	malloc(s)
#define	FREE(p)		free(p)

enum {
  DIR_TO_SERVER = 0,
  DIR_TO_CLIENT,
  DIR_NUM,
};


typedef struct {
  int dir;
  int fd;
  eventfd_t val;

  queue *fifo;		/* exec fifo */
  bool server_binded;
  bool client_binded;
} dir_info_t;


typedef struct {
  dir_info_t info[DIR_NUM];
} bd_fifo_t;


struct {
  void *arg;
  void (*cb)(void *);
} bd_fifo_msg_t;


static bool
ring_bell(dir_info_t *info)
{
  if (writable_safe(info->fd)) {
    int err = 0;

    while (!err && info->val) {
      if (bd_fifo_write(info->fd, info->val) < 0) {
        err = errno;
        if (err == EINTR) {
          err = 0;
        } else if (err == EAGAIN) {
          break;
        } else {
          /* major error */
          set_writeable_safe(info->fd, false);
          return false;
        }
      } else {
        fifo->val = 0;
        set_writeable_safe(info->fd, false);
      }
    }
  } else {
    if (info->val)
      set_writeable_safe(info->fd, true);
    else
      set_writeable_safe(fifo->fd, false);
  }
  return true;
}


static void
read_cb(int fd, void *arg)
{
  dir_info_t *info = arg;
  assert(fd == info->fd);

  bd_fifo_msg_t *msg;

  while (1) {
    eventfd_t val;

    if (bd_fifo_read(info->fd, &val) < 0) {
      int err = errno;
      if (err == EAGAIN)
        break;
      else if (err == EINTR)
        continue;
      else
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
  bd_fifo_t *fifo = arg;
  assert(fd == fifo->fd);

  ring_bell(fifo);
}


static bool
ringer_fifo( bd_fifo_t *fifo,
             int dir,
             void (*cb)( void * ),
             void *arg ) {
  bd_fifo_msg_t *msg;

  if (fifo->invalid)
    return false;

  if ((msg = MALLOC(sizeof(*msg))) != NULL) {
    msg->cb = cb;
    msg->arg = arg;

    enqueue( fifo->info[dir].queue, msg );
    fifo->info[dir].val++;

    return ring_bell(fifo, dir);
  }
  return false;
}


bool
call_fifo_server( bd_fifo_t *fifo,
                  void (*cb)( void * ),
                  void *arg )
{

}

bool
call_fifo_client( bd_fifo_t *fifo,
                  void (*cb)( void * ),
                  void *arg )
{

}


bool
bind_fifo_server( bd_fifo_t *fifo )
{

}


bool
bind_fifo_clent( bd_fifo_t *fifo )
{

}


bool
detach_fifo_server(bd_fifo_t *fifo)
{

}


bool
detach_fifo_client(bd_fifo_t *fifo)
{

}


/*****************************************************************
 * main thread (client side) functions
 *****************************************************************/
bd_fifo_t *
create_fifo( void )
{
  bd_fifo_t *fifo;

  fifo = MALLOC(sizeof(*fifo));
  if (fifo) {
    memset(fifo, 0, sizeof(*fifo));
    fifo->fd = -1;
  }
  return fifo;
}


void
destroy_fifo( bd_fifo_t *fifo )
{
  /* XXX: not yet */
}
