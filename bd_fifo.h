#ifndef _BD_FIFO_H_
#define	_BD_FIFO_H_

#include <sys/types.h>
#include <sys/eventfd.h>
#include <stdbool.h>
#include "queue.h"

enum {
  DIR_TO_SERVER = 0,
  DIR_TO_CLIENT,
  DIR_NUM,
};

typedef struct _dir_info_t {
  int fd;
  int padding;
  eventfd_t val;
  queue *queue;
  struct _dir_info_t *another;
  volatile size_t *state_p;
} dir_info_t;


typedef struct {
  volatile size_t state;
  dir_info_t info[DIR_NUM];
} bd_fifo_t;

extern bd_fifo_t *fifo_create( void );
extern void fifo_destroy( bd_fifo_t *fifo );
extern void fifo_detach_client(bd_fifo_t *fifo);
extern void fifo_detach_server(bd_fifo_t *fifo);
extern bool fifo_bind_clent( bd_fifo_t *fifo );
extern bool fifo_bind_server( bd_fifo_t *fifo );
extern bool fifo_to_client( bd_fifo_t *fifo,
                            void (*cb)( void * ),
                            void *arg );
extern bool fifo_to_server( bd_fifo_t *fifo,
                            void (*cb)( void * ),
                            void *arg );


#endif	/* !_BD_FIFO_H_ */
