// shm_processes.c
// Poor Student / Dear Old Dad using shared memory + semaphores
// Extra credit: optional Mom + N students via command line.
//
// Build:  gcc -O2 -Wall -Wextra -pedantic shm_processes.c -o psdd -pthread
// Run:    ./psdd 1 1     (Dad + 1 student)
//         ./psdd 1 3     (Dad + 3 students)
//         ./psdd 2 10    (Dad + Mom + 10 students)

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <semaphore.h>
#include <time.h>
#include <signal.h>
#include <string.h>
#include <errno.h>

typedef struct {
    int   BankAccount;
    sem_t mutex;          // protects BankAccount (critical section)
} SharedData;

static SharedData *g_shm = NULL;
static int g_fd = -1;
static char g_filename[256];

static pid_t *g_child_pids = NULL;
static int g_child_count = 0;

static void die(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

static int rand_inclusive(int lo, int hi) {
    // assumes lo <= hi
    return lo + (rand() % (hi - lo + 1));
}

static void lock_account(void) {
    if (sem_wait(&g_shm->mutex) == -1) die("sem_wait");
}

static void unlock_account(void) {
    if (sem_post(&g_shm->mutex) == -1) die("sem_post");
}

static void dad_loop(void) {
    int localBalance = 0;

    srand((unsigned int)(time(NULL) ^ (getpid() << 16)));

    for (;;) {
        sleep((unsigned int)rand_inclusive(0, 5));

        printf("Dear Old Dad: Attempting to Check Balance\n");
        fflush(stdout);

        int r = rand();
        if (r % 2 == 0) {
            if (localBalance < 100) {
                // Deposit Money
                lock_account();
                localBalance = g_shm->BankAccount;

                int amount = rand_inclusive(0, 100);

                if (amount % 2 == 0) {
                    localBalance += amount;
                    printf("Dear old Dad: Deposits $%d / Balance = $%d\n", amount, localBalance);
                    g_shm->BankAccount = localBalance;
                } else {
                    printf("Dear old Dad: Doesn't have any money to give\n");
                    // no change
                }
                fflush(stdout);
                unlock_account();
            } else {
                printf("Dear old Dad: Thinks Student has enough Cash ($%d)\n", localBalance);
                fflush(stdout);
            }
        } else {
            // Just check balance
            lock_account();
            localBalance = g_shm->BankAccount;
            printf("Dear Old Dad: Last Checking Balance = $%d\n", localBalance);
            fflush(stdout);
            unlock_account();
        }
    }
}

static void mom_loop(void) {
    int localBalance = 0;

    srand((unsigned int)(time(NULL) ^ (getpid() << 16)));

    for (;;) {
        sleep((unsigned int)rand_inclusive(0, 10));

        printf("Loveable Mom: Attempting to Check Balance\n");
        fflush(stdout);

        // Mom always checks the *shared* balance, then:
        // if last localBalance <= 100, always deposits.
        lock_account();
        localBalance = g_shm->BankAccount;

        if (localBalance <= 100) {
            int amount = rand_inclusive(0, 125);
            localBalance += amount;
            printf("Lovable Mom: Deposits $%d / Balance = $%d\n", amount, localBalance);
            g_shm->BankAccount = localBalance;
        } else {
            // Not explicitly specified what to do when > 100;
            // keeping it consistent with "check balance randomly" idea:
            printf("Loveable Mom: Last Checking Balance = $%d\n", localBalance);
        }
        fflush(stdout);
        unlock_account();
    }
}

static void student_loop(int student_id) {
    int localBalance = 0;

    srand((unsigned int)(time(NULL) ^ (getpid() << 16)));

    for (;;) {
        sleep((unsigned int)rand_inclusive(0, 5));

        printf("Poor Student(%d): Attempting to Check Balance\n", student_id);
        fflush(stdout);

        int r = rand();
        if (r % 2 == 0) {
            // Withdraw Money
            lock_account();
            localBalance = g_shm->BankAccount;

            int need = rand_inclusive(0, 50);
            printf("Poor Student(%d) needs $%d\n", student_id, need);

            if (need <= localBalance) {
                localBalance -= need;
                printf("Poor Student(%d): Withdraws $%d / Balance = $%d\n", student_id, need, localBalance);
                g_shm->BankAccount = localBalance;
            } else {
                printf("Poor Student(%d): Not Enough Cash ($%d)\n", student_id, localBalance);
                // no change
            }
            fflush(stdout);
            unlock_account();
        } else {
            // Just check balance
            lock_account();
            localBalance = g_shm->BankAccount;
            printf("Poor Student(%d): Last Checking Balance = $%d\n", student_id, localBalance);
            fflush(stdout);
            unlock_account();
        }
    }
}

static void cleanup_shared(void) {
    if (g_shm) {
        // sem_destroy is best-effort (if processes still running, this may fail)
        sem_destroy(&g_shm->mutex);
        munmap(g_shm, sizeof(SharedData));
        g_shm = NULL;
    }
    if (g_fd != -1) {
        close(g_fd);
        g_fd = -1;
    }
    if (g_filename[0] != '\0') {
        unlink(g_filename);
        g_filename[0] = '\0';
    }
}

static void kill_all_children(void) {
    for (int i = 0; i < g_child_count; i++) {
        if (g_child_pids[i] > 0) {
            kill(g_child_pids[i], SIGTERM);
        }
    }
}

static void handle_sigint(int sig) {
    (void)sig;
    kill_all_children();
    cleanup_shared();
    _exit(0);
}

int main(int argc, char *argv[]) {
    int numParents = 1;   // 1 = Dad only, 2 = Dad + Mom
    int numKids = 1;      // N students

    if (argc >= 3) {
        numParents = atoi(argv[1]);
        numKids = atoi(argv[2]);
        if (numParents < 1) numParents = 1;
        if (numParents > 2) numParents = 2;
        if (numKids < 1) numKids = 1;
    } else if (argc == 2) {
        // If they only pass one arg, treat it as numKids (common slip)
        numKids = atoi(argv[1]);
        if (numKids < 1) numKids = 1;
    }

    // Setup signal handler in the original parent
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // Create a unique mmap-backed file
    snprintf(g_filename, sizeof(g_filename), "/tmp/psdd_shm_%ld.dat", (long)getpid());
    g_fd = open(g_filename, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (g_fd == -1) die("open");

    if (ftruncate(g_fd, (off_t)sizeof(SharedData)) == -1) die("ftruncate");

    g_shm = mmap(NULL, sizeof(SharedData), PROT_READ | PROT_WRITE, MAP_SHARED, g_fd, 0);
    if (g_shm == MAP_FAILED) die("mmap");

    // Initialize shared data
    g_shm->BankAccount = 0;
    if (sem_init(&g_shm->mutex, 1 /*pshared*/, 1 /*unlocked*/) == -1) die("sem_init");

    // Track created child PIDs so we can kill them on exit
    // Total processes created from THIS parent: (numParents-1 mom) + numKids
    g_child_count = (numParents == 2 ? 1 : 0) + numKids;
    g_child_pids = calloc((size_t)g_child_count, sizeof(pid_t));
    if (!g_child_pids) die("calloc");

    int idx = 0;

    // If Mom requested, fork her
    if (numParents == 2) {
        pid_t p = fork();
        if (p < 0) die("fork mom");
        if (p == 0) {
            mom_loop();
            return 0;
        }
        g_child_pids[idx++] = p;
    }

    // Fork N students
    for (int i = 0; i < numKids; i++) {
        pid_t p = fork();
        if (p < 0) die("fork student");
        if (p == 0) {
            student_loop(i + 1);
            return 0;
        }
        g_child_pids[idx++] = p;
    }

    // Original process becomes Dad (per assignment’s “parent”)
    dad_loop();

    // Unreachable (dad_loop is infinite), but included for completeness:
    cleanup_shared();
    free(g_child_pids);
    return 0;
}
