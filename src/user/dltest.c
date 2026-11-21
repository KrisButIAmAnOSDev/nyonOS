#include <stdio.h>
#include <dlfcn.h>

int main(void) {
    printf("DLTEST: start\n");
    void *h = dlopen("/usr/lib/libtest.so", RTLD_LAZY);
    printf("DLTEST: dlopen = %p err=%s\n", h, h ? "(none)" : dlerror());
    if (!h) return 1;
    int (*add)(int, int) = (int (*)(int, int))dlsym(h, "nyon_add");
    int (*answer)(void) = (int (*)(void))dlsym(h, "nyon_answer");
    printf("DLTEST: dlsym add=%p answer=%p\n", (void *)add, (void *)answer);
    if (add && answer) {
        printf("DLTEST: add(2,3)=%d answer()=%d\n", add(2, 3), answer());
    }
    printf("DLTEST: dlclose=%d\n", dlclose(h));
    printf("DLTEST: done\n");
    return 0;
}
