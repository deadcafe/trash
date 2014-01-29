#ifndef _FUNC_QUEUE_H_
#define	_FUNC_QUEUE_H_

/*
	+---------------+	dir:Up		+---------------+
        | Client	|		wake up	| Server	|
        | thread	|--->	event FD    --->| thread	|
        |		|			|		|
        |		|	+-------+	|		|
        |   call func	|--->	|fnc que|   --->| exec func	|
        |		|	+-------|	|		|
        +---------------+			+---------------+
 */

#include <sys/types.h>
#include <sys/eventfd.h>
#include <stdbool.h>
#include "queue.h"

typedef enum {
  DIR_TO_UP = 0,
  DIR_TO_DOWN,

  DIR_NUM,
} func_q_dir_t;

/* event handler type */
typedef enum {
  EH_TYPE_GENERIC = 0,
  EH_TYPE_SAFE = 1,
} func_q_ehtype_t;


typedef struct _func_q_info_t {
  int fd;
  func_q_ehtype_t eh_type;
  eventfd_t val;
  struct _func_q_info_t *another;
  queue *queue;
  pthread_t bind_th;
} func_q_info_t;


typedef struct {
  func_q_info_t info[DIR_NUM];
} func_q_t;


extern func_q_t *func_q_create( void );
extern void func_q_destroy(func_q_t *func_q);

extern bool func_q_bind(func_q_t *func_q, func_q_dir_t dir, func_q_ehtype_t eh_type);
extern void func_q_unbind(func_q_t *func_q, func_q_dir_t dir);

extern bool func_q_request(func_q_t *func_q,
                           func_q_dir_t dir,
                           void (*cb)(void *),
                           void *arg);

#endif	/* !_FUNC_QUEUE_H_ */
