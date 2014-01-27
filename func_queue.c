/*
 * function queue
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
#include "func_queue.h"

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
} func_q_msg_t;


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

/*
 * wake up
 */
static bool
wakeup(func_q_info_t *info)
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
read_cb(int fd,
        void *arg)
{
  func_q_info_t *info = arg;
  func_q_msg_t *msg;
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
write_cb(int fd,
         void *arg)
{
  func_q_info_t *info = arg;
  assert(fd == info->fd);

  wakeup(info);
}


static void
null_cb(int fd __attribute__((unused)),
        void *arg __attribute__((unused)))
{
  /* nothing to do */
  ;
}


bool
func_q_request( func_q_t *func_q,
                func_q_dir_t dir,
                void (*cb)( void * ),
                void *arg )
{
  assert(cb);
  assert(dir == DIR_TO_UP || dir == DIR_TO_DOWN);
  func_q_msg_t *msg;

  func_q_info_t *info = &func_q->info[dir];
  size_t state = *info->state_p;
  RMB();

  if (state != STATE_READY)
    return false;

  if ((msg = MALLOC(sizeof(*msg))) != NULL) {
    msg->cb = cb;
    msg->arg = arg;

    enqueue( info->queue, msg );
    info->val++;

    return wakeup(info);
  }
  return false;
}


bool
func_q_bind( func_q_t *func_q,
             func_q_dir_t dir )
{
  assert(dir == DIR_TO_UP || dir == DIR_TO_DOWN);
  func_q_info_t *info = &func_q->info[dir];

  assert(info->queue == NULL);
  assert(info->fd >= 0);
  assert(info->another && info->another->fd >= 0);

  queue *queue = create_queue();
  if (queue) {
    set_fd_handler_safe(info->fd, read_cb, info, null_cb, NULL);
    set_readable_safe(info->fd, true);
    set_writable_safe(info->fd, false);

    set_fd_handler_safe(info->another->fd, null_cb, NULL,
                        write_cb, info->another);
    set_readable_safe(info->another->fd, false);
    set_writable_safe(info->another->fd, false);

    info->queue = queue;
    WMB();

    queue = info->another->queue;
    RMB();
    if (queue) {
      *info->state_p = STATE_READY;
      WMB();
    }
    return true;
  }
  return false;
}


void
func_q_unbind(func_q_t *func_q,
              func_q_dir_t dir)
{
  assert(dir == DIR_TO_UP || dir == DIR_TO_DOWN);
  func_q_info_t *info = (&func_q->info[dir]);
  int fd = info->fd;
  int another_fd = info->another->fd;
  queue *queue = info->queue;
  func_q_msg_t *msg;

  RMB();
  *info->state_p = STATE_INVALID;
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


/*****************************************************************
 * main thread (client side) functions
 *****************************************************************/
void
func_q_destroy( func_q_t *func_q )
{
  int i;
  assert(func_q->state == STATE_INVALID);

  for (i = 0; i < DIR_NUM; i++) {
    assert(func_q->info[i].queue == NULL);
    if (func_q->info[i].fd >= 0) {
      CLOSE(func_q->info[i].fd);
      func_q->info[i].fd = -1;
    }
  }
  FREE(func_q);
}


func_q_t *
func_q_create( void )
{
  func_q_t *func_q = MALLOC(sizeof(*func_q));
  if (func_q) {
    int fd;

    memset(func_q, 0, sizeof(*func_q));
    func_q->info[DIR_TO_UP].fd = -1;
    func_q->info[DIR_TO_DOWN].fd = -1;
    func_q->info[DIR_TO_UP].state_p = &func_q->state;
    func_q->info[DIR_TO_DOWN].state_p = &func_q->state;
    func_q->info[DIR_TO_UP].another = &func_q->info[DIR_TO_DOWN];
    func_q->info[DIR_TO_DOWN].another = &func_q->info[DIR_TO_UP];

    if ((fd = eventfd(0, EFD_NONBLOCK)) < 0) {
      func_q_destroy(func_q);
      return NULL;
    }
    func_q->info[DIR_TO_UP].fd = fd;

    if ((fd = eventfd(0, EFD_NONBLOCK)) < 0) {
      func_q_destroy(func_q);
      return NULL;
    }
    func_q->info[DIR_TO_DOWN].fd = fd;
  }
  return func_q;
}


