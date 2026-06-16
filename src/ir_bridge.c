#include "ir_bridge.h"

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

static int bridge_fd = -1;

void ir_bridge_attach(int fd) {
 bridge_fd = fd;
 if (fd < 0) return;
 int fl = fcntl(fd, F_GETFL, 0);
 if (fl != -1) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
 signal(SIGPIPE, SIG_IGN);
}

int ir_bridge_fd(void) { return bridge_fd; }
