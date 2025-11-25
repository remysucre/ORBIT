// Minimal syscall stubs for Playdate device build
// These are required by newlib but not used in our embedded environment

// Only compile these stubs for the Playdate device, not for the simulator
#if TARGET_PLAYDATE

#include <sys/stat.h>
#include <errno.h>
#undef errno
extern int errno;

// Exit - just loop forever
void _exit(int status) {
    (void)status;
    while(1);
}

// Close - not supported
int _close(int file) {
    (void)file;
    return -1;
}

// File status - not supported
int _fstat(int file, struct stat *st) {
    (void)file;
    st->st_mode = S_IFCHR;
    return 0;
}

// Is a terminal - not supported
int _isatty(int file) {
    (void)file;
    return 1;
}

// Seek - not supported
int _lseek(int file, int ptr, int dir) {
    (void)file;
    (void)ptr;
    (void)dir;
    return 0;
}

// Read - not supported
int _read(int file, char *ptr, int len) {
    (void)file;
    (void)ptr;
    (void)len;
    return 0;
}

// Write - not supported (we could redirect to Playdate's log)
int _write(int file, char *ptr, int len) {
    (void)file;
    (void)ptr;
    return len;  // Pretend we wrote it
}

// Get process ID - not applicable
int _getpid(void) {
    return 1;
}

// Kill - not supported
int _kill(int pid, int sig) {
    (void)pid;
    (void)sig;
    errno = EINVAL;
    return -1;
}

// Fini - empty implementation
void _fini(void) {
}

// Thread-local storage pointer - not supported
// This is needed for __aeabi_read_tp
void* __aeabi_read_tp(void) {
    return 0;
}

#endif // TARGET_PLAYDATE
