/* Minimal amd64 steamwebhelper stub — immediate exit for CEF subprocess */
#include <unistd.h>

int main(void) {
    static const char msg[] = "NEXXON_STEAMWEBHELPER_STUB\n";
    (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
    return 0;
}
