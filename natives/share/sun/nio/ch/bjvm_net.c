// Shared tables for the ch/ natives. These must live in exactly one
// translation unit: bjvm_net.h declares them extern so every native sees
// the same fake-fd table and the same nonblocking-fd set.
#include "bjvm_net.h"

bjvm_fakefd *bjvm_fakefds;
int bjvm_next_fake_fd = BJVM_FAKE_FD_BASE;

int *bjvm_nonblock_fds;
