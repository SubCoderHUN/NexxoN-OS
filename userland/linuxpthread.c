/* musl-static pthread probe: CLONE_THREAD + blocking futex via pthread API. */
#include <pthread.h>
#include <stdint.h>
#include <unistd.h>

static volatile int g_counter;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static void *worker(void *arg) {
    int id = (int)(intptr_t)arg;
    pthread_mutex_lock(&g_mu);
    g_counter += id;
    pthread_mutex_unlock(&g_mu);
    return NULL;
}

static int fail(int step) {
    static const char msg[] = "NEXXON_LINUX_PTHREAD_FAIL\n";
    (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
    return step;
}

int main(void) {
    pthread_t t1, t2;
    g_counter = 0;

    if (pthread_create(&t1, NULL, worker, (void *)(intptr_t)1) != 0)
        return fail(10);
    if (pthread_create(&t2, NULL, worker, (void *)(intptr_t)2) != 0)
        return fail(11);
    if (pthread_join(t1, NULL) != 0) return fail(12);
    if (pthread_join(t2, NULL) != 0) return fail(13);
    if (g_counter != 3) return fail(14);

    static const char ok[] = "NEXXON_LINUX_PTHREAD_OK\n";
    if (write(STDOUT_FILENO, ok, sizeof(ok) - 1) != (ssize_t)(sizeof(ok) - 1))
        return fail(15);
    return 0;
}
