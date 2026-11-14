#include <pthread.h>
#include <stdio.h>
#include <errno.h>

__thread int tls_val = 0x1234;
__thread long tls_arr[4] = { 1, 2, 3, 4 };

struct child_result {
    long isolated;
    long parent_value;
    long parent_array;
    long child_errno;
};

static void *worker(void *arg) {
    struct child_result *out = arg;

    tls_val = 0x5678;
    tls_arr[0] = 99;

    out->isolated = (tls_val == 0x5678 && tls_arr[0] == 99) ? 1 : 0;
    out->parent_value = 0;
    out->parent_array = 0;

    errno = 42;
    out->child_errno = (errno == 42) ? 1 : 0;
    return 0;
}

int main(void) {
    struct child_result res;
    res.isolated = 0;
    res.parent_value = 0;
    res.parent_array = 0;

    pthread_t th;
    errno = 0;
    if (pthread_create(&th, 0, worker, &res) != 0) {
        printf("tls: pthread_create failed\n");
        return 1;
    }
    pthread_join(th, 0);

    int ok = 1;

    printf("tls: main tls_val=0x%x (want 0x1234)\n", tls_val);
    if (tls_val != 0x1234) ok = 0;

    printf("tls: main tls_arr[0]=%ld (want 1)\n", tls_arr[0]);
    if (tls_arr[0] != 1) ok = 0;

    printf("tls: main tls_arr[3]=%ld (want 4)\n", tls_arr[3]);
    if (tls_arr[3] != 4) ok = 0;

    printf("tls: child saw its own copy: %ld (want 1)\n", res.isolated);
    if (!res.isolated) ok = 0;

    printf("tls: child errno round-trip: %ld (want 1)\n", res.child_errno);
    if (!res.child_errno) ok = 0;

    printf("tls: parent errno untouched by child: %d (want 0)\n", errno);
    if (errno != 0) ok = 0;


    printf("tls: %s\n", ok ? "TLS OK" : "TLS BROKEN");

    FILE *f = fopen("/TLSRESULT", "w");
    if (f) {
        fputs(ok ? "OK" : "NO", f);
        fclose(f);
    }

    return ok ? 0 : 1;
}
