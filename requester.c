#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

int
main(int ac, char **av)
{
  char *fname = ".hiper.fifo";
  int opt;
  int sock;
  int i;

  while ((opt = getopt(ac, av, "f")) != -1) {
    switch (opt) {
    case 'f':	/* socket name */
      fname = optarg;
      break;

    default:
      fprintf(stderr, "%s -f SOCK_NAME\n", av[0]);
      exit(0);
    }
  }


  if ((sock = open(fname, O_RDWR, 0)) >= 0) {
    FILE *f = fdopen(sock, "w");

    for (i = 0; i < 1100; i++) {
      fprintf(f, "http://1.1.1.1/%d\n", i);
      fprintf(stdout, "%d\n", i);
    }
  } else {
    fprintf(stderr, "failed\n");

  }

  return 0;
}
