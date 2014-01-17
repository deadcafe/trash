#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#include <trema.h>
#include "libevent_wrapper.h"
#include "http_client.h"

static void
init_module(void)
{
  if (!init_http_client( 10 ))
    exit(1);
}

static void
finalize_module(void)
{
  if (!finalize_http_client())
    exit(1);
}

int
main(int ac, char **av )
{
  init_libevent_wrapper( xmalloc, NULL, xfree, 0 );
  init_trema( &ac, &av );
  init_module();

  start_trema();

  finalize_module();
  finalize_libevent_wrapper();
  fprintf(stderr, "END\n");
  return 0;
}
