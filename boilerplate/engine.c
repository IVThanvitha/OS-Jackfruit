#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/resource.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define CONTROL_SOCK_PATH "/tmp/jackfruit_supervisor.sock"
#define STACK_SIZE (1024 * 1024)
#define MAX_CONTAINERS 64
#define LOG_QUEUE_CAP 256
#define LOG_MSG_SIZE 512

typedef enum {
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP,
    CMD_INVALID
} command_type_t;

typedef struct {
    command_type_t type;
    char id[64];
    char rootfs[256];
    char command[256];
    int soft_mib;
    int hard_mib;
    int nice_value;
} control_request_t;

typedef struct {
    int status;              // 0 success, non-zero error
    int exit_code;           // for run
    char message[1024];
} control_response_t;

typedef enum {
    STATE_EMPTY,
    STATE_STARTING,
    STATE_RUNNING,
    STATE_EXITED,
    STATE_STOPPED,
    STATE_HARD_LIMIT_KILLED,
    STATE_FAILED
} container_state_t;

typedef struct {
    char id[64];
    char rootfs[256];
    char command[256];
    pid_t host_pid;
    time_t start_time;
    container_state_t state;
    int soft_mib;
    int hard_mib;
    int nice_value;
    int exit_code;
    int exit_signal;
    int stop_requested;
    char log_path[256];

    int stdout_fd;
    int stderr_fd;

    pthread_t stdout_thread;
    pthread_t stderr_thread;
} container_t;

typedef struct {
    char container_id[64];
    char stream[16];
    char data[LOG_MSG_SIZE];
    int eof;
} log_entry_t;

typedef struct {
    log_entry_t items[LOG_QUEUE_CAP];
    int head;
    int tail;
    int count;
    int shutdown;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} log_queue_t;

typedef struct {
    container_t containers[MAX_CONTAINERS];
    int running;
    int monitor_fd;
    char base_rootfs[256];
} supervisor_t;

typedef struct {
    char id[64];
    char rootfs[256];
    char command[256];
    int nice_value;
    int stdout_pipe[2];
    int stderr_pipe[2];
} child_args_t;

typedef struct {
    int fd;
    char container_id[64];
    char stream[16];
    log_queue_t *queue;
} producer_arg_t;

pthread_mutex_t containers_lock = PTHREAD_MUTEX_INITIALIZER;
supervisor_t supervisor;
log_queue_t log_queue;
pthread_t log_consumer_thread;
volatile sig_atomic_t child_exited = 0;

void sigchld_handler(int signo) {
    (void)signo;
    child_exited = 1;
}

void log_queue_init(log_queue_t *q) {
    q->head = 0; q->tail = 0; q->count = 0; q->shutdown = 0;
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
}

void log_enqueue(log_queue_t *q, log_entry_t *entry) {
    pthread_mutex_lock(&q->lock);
    while (q->count == LOG_QUEUE_CAP && !q->shutdown) {
        pthread_cond_wait(&q->not_full, &q->lock);
    }
    if (q->shutdown) {
        pthread_mutex_unlock(&q->lock);
        return;
    }
    q->items[q->tail] = *entry;
    q->tail = (q->tail + 1) % LOG_QUEUE_CAP;
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
}

int log_dequeue(log_queue_t *q, log_entry_t *entry) {
    pthread_mutex_lock(&q->lock);
    while (q->count == 0 && !q->shutdown) {
        pthread_cond_wait(&q->not_empty, &q->lock);
    }
    if (q->count == 0 && q->shutdown) {
        pthread_mutex_unlock(&q->lock);
        return 0; // indicates EOF
    }
    *entry = q->items[q->head];
    q->head = (q->head + 1) % LOG_QUEUE_CAP;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->lock);
    return 1;
}

void *log_consumer_func(void *arg) {
    (void)arg;
    log_entry_t entry;
    mkdir("logs", 0755);
    while (log_dequeue(&log_queue, &entry)) {
        char path[512];
        snprintf(path, sizeof(path), "logs/%s.log", entry.container_id);
        FILE *f = fopen(path, "a");
        if (f) {
            if (!entry.eof) {
                fprintf(f, "[%s] %s", entry.stream, entry.data);
            } else {
                fprintf(f, "[%s] EOF\n", entry.stream);
            }
            fflush(f);
            fclose(f);
        }
    }
    return NULL;
}

void *producer_thread_func(void *arg) {
    producer_arg_t *parg = (producer_arg_t *)arg;
    char buf[LOG_MSG_SIZE - 64]; 
    ssize_t n;
    while ((n = read(parg->fd, buf, sizeof(buf)-1)) > 0) {
        buf[n] = '\0';
        log_entry_t entry;
        memset(&entry, 0, sizeof(entry));
        strncpy(entry.container_id, parg->container_id, sizeof(entry.container_id)-1);
        strncpy(entry.stream, parg->stream, sizeof(entry.stream)-1);
        strncpy(entry.data, buf, sizeof(entry.data)-1);
        entry.eof = 0;
        log_enqueue(parg->queue, &entry);
    }
    log_entry_t eof_entry;
    memset(&eof_entry, 0, sizeof(eof_entry));
    strncpy(eof_entry.container_id, parg->container_id, sizeof(eof_entry.container_id)-1);
    strncpy(eof_entry.stream, parg->stream, sizeof(eof_entry.stream)-1);
    eof_entry.eof = 1;
    log_enqueue(parg->queue, &eof_entry);
    close(parg->fd);
    free(parg);
    return NULL;
}

int child_main(void *arg) {
    child_args_t *args = (child_args_t *)arg;
    
    close(args->stdout_pipe[0]);
    close(args->stderr_pipe[0]);
    dup2(args->stdout_pipe[1], STDOUT_FILENO);
    dup2(args->stderr_pipe[1], STDERR_FILENO);
    close(args->stdout_pipe[1]);
    close(args->stderr_pipe[1]);

    sethostname(args->id, strlen(args->id));

    mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);
    mount(args->rootfs, args->rootfs, "bind", MS_BIND | MS_REC, NULL);
    
    chroot(args->rootfs);
    chdir("/");
    
    mkdir("/proc", 0555);
    mount("proc", "/proc", "proc", 0, NULL);
    
    setpriority(PRIO_PROCESS, 0, args->nice_value);
    
    char *argv[] = { args->command, NULL };
    execv(args->command, argv);
    
    fprintf(stderr, "Failed to exec: %s\n", strerror(errno));
    exit(1);
}

int handle_start(control_request_t *req, control_response_t *resp) {
    struct stat st;
    if (stat(req->rootfs, &st) != 0 || !S_ISDIR(st.st_mode)) {
        resp->status = 1;
        strncpy(resp->message, "Missing rootfs path", sizeof(resp->message)-1);
        return -1;
    }

    pthread_mutex_lock(&containers_lock);
    int slot = -1;
    for (int i=0; i<MAX_CONTAINERS; i++) {
        container_t *c = &supervisor.containers[i];
        if (c->state != STATE_EMPTY && c->state != STATE_FAILED) {
            if (strcmp(c->id, req->id) == 0) {
                if (c->state == STATE_RUNNING || c->state == STATE_STARTING) {
                    resp->status = 1;
                    strncpy(resp->message, "Duplicate running container ID", sizeof(resp->message)-1);
                    pthread_mutex_unlock(&containers_lock);
                    return -1;
                }
            }
            if (strcmp(c->rootfs, req->rootfs) == 0) {
                 if (c->state == STATE_RUNNING || c->state == STATE_STARTING) {
                    resp->status = 1;
                    strncpy(resp->message, "Reused live rootfs path", sizeof(resp->message)-1);
                    pthread_mutex_unlock(&containers_lock);
                    return -1;
                }
            }
        }
        if (c->state == STATE_EMPTY && slot == -1) {
            slot = i;
        }
    }
    
    if (slot == -1) {
        resp->status = 1;
        strncpy(resp->message, "Max containers reached", sizeof(resp->message)-1);
        pthread_mutex_unlock(&containers_lock);
        return -1;
    }
    
    container_t *c = &supervisor.containers[slot];
    memset(c, 0, sizeof(*c));
    strncpy(c->id, req->id, sizeof(c->id)-1);
    strncpy(c->rootfs, req->rootfs, sizeof(c->rootfs)-1);
    strncpy(c->command, req->command, sizeof(c->command)-1);
    c->soft_mib = req->soft_mib > 0 ? req->soft_mib : 40;
    c->hard_mib = req->hard_mib > 0 ? req->hard_mib : 64;
    c->nice_value = req->nice_value;
    c->state = STATE_STARTING;
    c->start_time = time(NULL);
    snprintf(c->log_path, sizeof(c->log_path), "logs/%s.log", c->id);
    pthread_mutex_unlock(&containers_lock);

    child_args_t *cargs = malloc(sizeof(child_args_t));
    memset(cargs, 0, sizeof(*cargs));
    strncpy(cargs->id, req->id, sizeof(cargs->id)-1);
    strncpy(cargs->rootfs, req->rootfs, sizeof(cargs->rootfs)-1);
    strncpy(cargs->command, req->command, sizeof(cargs->command)-1);
    cargs->nice_value = req->nice_value;
    pipe(cargs->stdout_pipe);
    pipe(cargs->stderr_pipe);

    char *stack = malloc(STACK_SIZE);
    char *stack_top = stack + STACK_SIZE;
    
    pid_t pid = clone(child_main, stack_top, CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD, cargs);
    if (pid < 0) {
        resp->status = 1;
        strncpy(resp->message, "Clone failed", sizeof(resp->message)-1);
        pthread_mutex_lock(&containers_lock);
        c->state = STATE_FAILED;
        pthread_mutex_unlock(&containers_lock);
        free(cargs);
        return -1;
    }
    
    close(cargs->stdout_pipe[1]);
    close(cargs->stderr_pipe[1]);
    
    pthread_mutex_lock(&containers_lock);
    c->host_pid = pid;
    c->state = STATE_RUNNING;
    pthread_mutex_unlock(&containers_lock);
    
    if (supervisor.monitor_fd >= 0) {
        struct monitor_req mreq;
        memset(&mreq, 0, sizeof(mreq));
        mreq.pid = pid;
        mreq.soft_limit_mib = c->soft_mib;
        mreq.hard_limit_mib = c->hard_mib;
        strncpy(mreq.id, c->id, sizeof(mreq.id)-1);
        ioctl(supervisor.monitor_fd, MONITOR_IOCTL_REGISTER, &mreq);
    }

    producer_arg_t *pout = malloc(sizeof(producer_arg_t));
    pout->fd = cargs->stdout_pipe[0];
    strncpy(pout->container_id, c->id, sizeof(pout->container_id)-1);
    strcpy(pout->stream, "stdout");
    pout->queue = &log_queue;
    pthread_create(&c->stdout_thread, NULL, producer_thread_func, pout);

    producer_arg_t *perr = malloc(sizeof(producer_arg_t));
    perr->fd = cargs->stderr_pipe[0];
    strncpy(perr->container_id, c->id, sizeof(perr->container_id)-1);
    strcpy(perr->stream, "stderr");
    perr->queue = &log_queue;
    pthread_create(&c->stderr_thread, NULL, producer_thread_func, perr);

    resp->status = 0;
    snprintf(resp->message, sizeof(resp->message), "Container %s started", c->id);
    return slot;
}

void handle_ps(control_request_t *req, control_response_t *resp) {
    (void)req;
    pthread_mutex_lock(&containers_lock);
    resp->status = 0;
    int offset = 0;
    for (int i=0; i<MAX_CONTAINERS; i++) {
        container_t *c = &supervisor.containers[i];
        if (c->state != STATE_EMPTY) {
            const char *st = "UNKNOWN";
            if (c->state==STATE_STARTING) st="STARTING";
            if (c->state==STATE_RUNNING) st="RUNNING";
            if (c->state==STATE_EXITED) st="EXITED";
            if (c->state==STATE_STOPPED) st="STOPPED";
            if (c->state==STATE_HARD_LIMIT_KILLED) st="HARD_LIMIT_KILLED";
            if (c->state==STATE_FAILED) st="FAILED";
            
            offset += snprintf(resp->message + offset, sizeof(resp->message) - offset,
               "ID: %s PID: %d State: %s MemLimits: %d/%d MB\n", 
               c->id, c->host_pid, st, c->soft_mib, c->hard_mib);
        }
    }
    if (offset == 0) {
        snprintf(resp->message, sizeof(resp->message), "No tracked containers");
    }
    pthread_mutex_unlock(&containers_lock);
}

void handle_logs(control_request_t *req, control_response_t *resp) {
    char path[256];
    snprintf(path, sizeof(path), "logs/%s.log", req->id);
    FILE *f = fopen(path, "r");
    if (!f) {
        resp->status = 1;
        snprintf(resp->message, sizeof(resp->message), "No logs for %s", req->id);
        return;
    }
    int len = fread(resp->message, 1, sizeof(resp->message)-1, f);
    resp->message[len] = '\0';
    fclose(f);
    resp->status = 0;
}

void handle_stop(control_request_t *req, control_response_t *resp) {
    pthread_mutex_lock(&containers_lock);
    container_t *target = NULL;
    for (int i=0; i<MAX_CONTAINERS; i++) {
        if (supervisor.containers[i].state == STATE_RUNNING || supervisor.containers[i].state == STATE_STARTING) {
            if (strcmp(supervisor.containers[i].id, req->id) == 0) {
                target = &supervisor.containers[i];
                break;
            }
        }
    }
    
    if (!target) {
        resp->status = 1;
        snprintf(resp->message, sizeof(resp->message), "Container %s not running", req->id);
        pthread_mutex_unlock(&containers_lock);
        return;
    }
    
    target->stop_requested = 1;
    pid_t pid = target->host_pid;
    pthread_mutex_unlock(&containers_lock);
    
    kill(pid, SIGTERM);
    usleep(200000); // Wait 200ms
    
    pthread_mutex_lock(&containers_lock);
    if (target->state == STATE_RUNNING || target->state == STATE_STARTING) {
        kill(pid, SIGKILL);
    }
    pthread_mutex_unlock(&containers_lock);
    
    resp->status = 0;
    snprintf(resp->message, sizeof(resp->message), "Container %s stopped", req->id);
}

void *client_thread(void *arg) {
    int client_fd = *(int *)arg;
    free(arg);
    control_request_t req;
    control_response_t resp;
    memset(&resp, 0, sizeof(resp));

    if (read(client_fd, &req, sizeof(req)) == sizeof(req)) {
        if (req.type == CMD_START) {
            handle_start(&req, &resp);
        } else if (req.type == CMD_RUN) {
            int slot = handle_start(&req, &resp);
            if (slot >= 0) {
                while(1) {
                    pthread_mutex_lock(&containers_lock);
                    container_state_t state = supervisor.containers[slot].state;
                    int exit_code = supervisor.containers[slot].exit_code;
                    pthread_mutex_unlock(&containers_lock);
                    
                    if (state == STATE_STOPPED || state == STATE_EXITED || state == STATE_HARD_LIMIT_KILLED || state == STATE_FAILED) {
                        resp.exit_code = exit_code;
                        resp.status = 0;
                        snprintf(resp.message, sizeof(resp.message), "Container finished (state=%d code=%d)", (int)state, exit_code);
                        break;
                    }
                    usleep(100000);
                }
            }
        } else if (req.type == CMD_PS) {
            handle_ps(&req, &resp);
        } else if (req.type == CMD_LOGS) {
            handle_logs(&req, &resp);
        } else if (req.type == CMD_STOP) {
            handle_stop(&req, &resp);
        }
    }
    
    write(client_fd, &resp, sizeof(resp));
    close(client_fd);
    return NULL;
}

void daemon_mode() {
    memset(&supervisor, 0, sizeof(supervisor));
    log_queue_init(&log_queue);
    
    supervisor.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (supervisor.monitor_fd < 0) {
        fprintf(stderr, "Warning: failed to open /dev/container_monitor\n");
    }

    pthread_create(&log_consumer_thread, NULL, log_consumer_func, NULL);

    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        exit(1);
    }
    
    unlink(CONTROL_SOCK_PATH);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_SOCK_PATH, sizeof(addr.sun_path) - 1);
    
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        exit(1);
    }
    
    listen(server_fd, 10);
    
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    while (1) {
        if (child_exited) {
            child_exited = 0;
            int status;
            pid_t pid;
            while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
                 pthread_mutex_lock(&containers_lock);
                 for (int i=0; i<MAX_CONTAINERS; i++) {
                     if (supervisor.containers[i].host_pid == pid && 
                        (supervisor.containers[i].state == STATE_RUNNING || supervisor.containers[i].state == STATE_STARTING)) {
                         
                         if (WIFEXITED(status)) {
                             supervisor.containers[i].exit_code = WEXITSTATUS(status);
                         } else if (WIFSIGNALED(status)) {
                             supervisor.containers[i].exit_signal = WTERMSIG(status);
                         }
                         if (supervisor.containers[i].stop_requested) {
                             supervisor.containers[i].state = STATE_STOPPED;
                         } else if (supervisor.containers[i].exit_signal == SIGKILL && !supervisor.containers[i].stop_requested) {
                             supervisor.containers[i].state = STATE_HARD_LIMIT_KILLED;
                         } else {
                             supervisor.containers[i].state = STATE_EXITED;
                         }
                         
                         if (supervisor.monitor_fd >= 0) {
                             ioctl(supervisor.monitor_fd, MONITOR_IOCTL_UNREGISTER, &pid);
                         }
                     }
                 }
                 pthread_mutex_unlock(&containers_lock);
            }
        }
        
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }
        
        int *cfd = malloc(sizeof(int));
        *cfd = client_fd;
        pthread_t tid;
        pthread_create(&tid, NULL, client_thread, cfd);
        pthread_detach(tid);
    }
}

static int send_req(control_request_t *req, control_response_t *resp) {
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_SOCK_PATH, sizeof(addr.sun_path) - 1);
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sock);
        return -1;
    }
    write(sock, req, sizeof(*req));
    if (resp) {
        memset(resp, 0, sizeof(*resp));
        read(sock, resp, sizeof(*resp));
    }
    close(sock);
    return 0;
}

volatile sig_atomic_t run_interrupted = 0;
char run_global_id[64];
void run_sig_handler(int sig) {
    (void)sig;
    run_interrupted = 1;
}

int cli_mode(int argc, char *argv[]) {
    control_request_t req;
    memset(&req, 0, sizeof(req));
    
    if (strcmp(argv[1], "ps") == 0) {
        req.type = CMD_PS;
        control_response_t resp;
        if (send_req(&req, &resp) == 0) {
            printf("%s\n", resp.message);
        }
        return 0;
    }
    if (strcmp(argv[1], "logs") == 0) {
        if (argc < 3) return 1;
        req.type = CMD_LOGS;
        strncpy(req.id, argv[2], sizeof(req.id)-1);
        control_response_t resp;
        if (send_req(&req, &resp) == 0) {
            if (resp.status == 0) {
                printf("%s", resp.message);
            } else {
                printf("Error: %s\n", resp.message);
            }
        }
        return 0;
    }
    if (strcmp(argv[1], "stop") == 0) {
        if (argc < 3) return 1;
        req.type = CMD_STOP;
        strncpy(req.id, argv[2], sizeof(req.id)-1);
        control_response_t resp;
        if (send_req(&req, &resp) == 0) {
            printf("%s\n", resp.message);
        }
        return 0;
    }
    
    if (strcmp(argv[1], "start") == 0 || strcmp(argv[1], "run") == 0) {
        if (argc < 5) return 1;
        req.type = strcmp(argv[1], "start") == 0 ? CMD_START : CMD_RUN;
        strncpy(req.id, argv[2], sizeof(req.id)-1);
        strncpy(req.rootfs, argv[3], sizeof(req.rootfs)-1);
        strncpy(req.command, argv[4], sizeof(req.command)-1);
        req.soft_mib = 40;
        req.hard_mib = 64;
        
        for (int i=5; i<argc; i+=2) {
            if (i+1 >= argc) break;
            if (strcmp(argv[i], "--soft-mib") == 0) req.soft_mib = atoi(argv[i+1]);
            else if (strcmp(argv[i], "--hard-mib") == 0) req.hard_mib = atoi(argv[i+1]);
            else if (strcmp(argv[i], "--nice") == 0) req.nice_value = atoi(argv[i+1]);
        }
        
        if (req.type == CMD_START) {
            control_response_t resp;
            if (send_req(&req, &resp) == 0) {
                printf("%s\n", resp.message);
                return resp.status;
            }
        } else {
            int sock = socket(AF_UNIX, SOCK_STREAM, 0);
            struct sockaddr_un addr;
            addr.sun_family = AF_UNIX;
            strncpy(addr.sun_path, CONTROL_SOCK_PATH, sizeof(addr.sun_path) - 1);
            if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
                perror("connect");
                return 1;
            }
            write(sock, &req, sizeof(req));
            
            strncpy(run_global_id, req.id, sizeof(run_global_id)-1);
            signal(SIGINT, run_sig_handler);
            signal(SIGTERM, run_sig_handler);
            
            while(1) {
                if (run_interrupted) {
                    run_interrupted = 0;
                    control_request_t stop_req;
                    memset(&stop_req, 0, sizeof(stop_req));
                    stop_req.type = CMD_STOP;
                    strncpy(stop_req.id, run_global_id, sizeof(stop_req.id)-1);
                    send_req(&stop_req, NULL);
                }
                
                fd_set rfds;
                FD_ZERO(&rfds);
                FD_SET(sock, &rfds);
                struct timeval tv = {0, 100000};
                int ret = select(sock + 1, &rfds, NULL, NULL, &tv);
                if (ret > 0) {
                    control_response_t resp;
                    int r = read(sock, &resp, sizeof(resp));
                    if (r > 0) {
                        printf("%s\n", resp.message);
                        close(sock);
                        return resp.exit_code;
                    } else {
                        break;
                    }
                }
            }
            close(sock);
            return 0;
        }
    }
    return 1;
}

int main(int argc, char *argv[]) {
    if (argc < 2) return 1;
    if (strcmp(argv[1], "supervisor") == 0) {
        daemon_mode();
    } else {
        return cli_mode(argc, argv);
    }
    return 0;
}
