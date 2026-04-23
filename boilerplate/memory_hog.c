#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define SOFT_LIMIT (10 * 1024 * 1024)   // 10 MB
#define HARD_LIMIT (20 * 1024 * 1024)   // 20 MB

int main() {
    size_t total = 0;
    const size_t chunk = 1024 * 1024; // 1 MB

    printf("memory_hog started\n");

    while (1) {
        void *p = malloc(chunk);
        if (!p) {
            perror("malloc");
            break;
        }

        total += chunk;

        printf("Allocated: %zu MB\n", total / (1024 * 1024));
        sleep(1);

        if (total >= SOFT_LIMIT && total < HARD_LIMIT) {
            printf("[monitor] SOFT LIMIT exceeded at %zu MB\n",
                   total / (1024 * 1024));
        }

        if (total >= HARD_LIMIT) {
            printf("[monitor] HARD LIMIT exceeded at %zu MB\n",
                   total / (1024 * 1024));
            printf("[monitor] Killing process...\n");
            break;
        }
    }

    return 0;
}
