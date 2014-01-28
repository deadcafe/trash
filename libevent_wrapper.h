/*
 * test bed
 */

#ifndef _LIBEVENT_WRAPPER_H_
#define _LIBEVENT_WRAPPER_H_

#include <sys/types.h>
#include <sys/time.h>
#include <stdint.h>
#include <stdbool.h>

#include <trema.h>


/*
 *
 */
#ifndef LIBEVENT_WRAPPER_AUTO_INIT
extern bool init_libevent_wrapper( void *( *malloc_fn ) ( size_t sz ),
                                   void *( *realloc_fn ) ( void *ptr, size_t sz ),
                                   void ( *free_fn ) ( void *ptr ),
                                   suseconds_t tick );
extern bool finalize_libevent_wrapper( void );
#endif /* !LIBEVENT_WRAPPER_AUTO_INIT */

#endif /* !_LIBEVENT_WRAPPER_H_ */
