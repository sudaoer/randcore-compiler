#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define DEFAULT_PROC_AI_THREAD "/proc/set_ai_thread"
#define DEFAULT_STATE_DIR "/tmp"
#define RANDCORE_PREFIX "randcore-"
#define STATE_BASENAME "randcore-compiler"

enum randcore_cluster {
    CLUSTER_X100 = 0,
    CLUSTER_A100 = 1,
};

struct state_record {
    pid_t pid;
    unsigned long long start_time;
    enum randcore_cluster cluster;
};

struct randcore_state {
    enum randcore_cluster next_tie;
    struct state_record *records;
    size_t count;
    size_t capacity;
};

struct state_files {
    int lock_fd;
    int state_fd;
    char *lock_path;
    char *state_path;
};

struct route_decision {
    enum randcore_cluster desired;
    size_t x100_count;
    size_t a100_count;
    bool tied;
};

struct child_report {
    int status;
    int errnum;
    int cluster;
};

#define CHILD_SETUP_OK 0
#define CHILD_SETUP_FAILED 1

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash == NULL ? path : slash + 1;
}

static bool is_empty(const char *value) {
    return value == NULL || value[0] == '\0';
}

static bool parse_bool_env(const char *name) {
    const char *value = getenv(name);

    if (is_empty(value)) {
        return false;
    }

    return strcmp(value, "1") == 0 || strcmp(value, "true") == 0 || strcmp(value, "TRUE") == 0 ||
           strcmp(value, "yes") == 0 || strcmp(value, "YES") == 0 || strcmp(value, "on") == 0 ||
           strcmp(value, "ON") == 0;
}

static const char *cluster_name(enum randcore_cluster cluster) {
    return cluster == CLUSTER_A100 ? "A100" : "X100";
}

static const char *cluster_token(enum randcore_cluster cluster) {
    return cluster == CLUSTER_A100 ? "a100" : "x100";
}

static int cluster_from_token(const char *token, enum randcore_cluster *cluster) {
    if (strcmp(token, "x100") == 0 || strcmp(token, "X100") == 0) {
        *cluster = CLUSTER_X100;
        return 0;
    }
    if (strcmp(token, "a100") == 0 || strcmp(token, "A100") == 0) {
        *cluster = CLUSTER_A100;
        return 0;
    }

    return -1;
}

static bool streq(const char *lhs, const char *rhs) {
    return strcmp(lhs, rhs) == 0;
}

static bool requested_compiler_is_gcc(const char *compiler) {
    return streq(compiler, "gcc");
}

static bool requested_compiler_is_gxx(const char *compiler) {
    return streq(compiler, "g++");
}

static const char *requested_compiler_name(const char *argv0) {
    const char *name = base_name(argv0);
    size_t prefix_len = strlen(RANDCORE_PREFIX);

    if (strncmp(name, RANDCORE_PREFIX, prefix_len) != 0) {
        return NULL;
    }

    name += prefix_len;
    return name[0] == '\0' ? NULL : name;
}

static const char *real_compiler_path(const char *requested_compiler) {
    const char *env = getenv("RANDCORE_REAL_COMPILER");

    if (!is_empty(env)) {
        return env;
    }

    if (requested_compiler_is_gcc(requested_compiler)) {
        env = getenv("RANDCORE_GCC");
        if (!is_empty(env)) {
            return env;
        }
    }

    if (requested_compiler_is_gxx(requested_compiler)) {
        env = getenv("RANDCORE_GXX");
        if (!is_empty(env)) {
            return env;
        }
    }

    return requested_compiler;
}

static char **make_exec_argv(int argc, char **argv, const char *compiler_path) {
    char **exec_argv = calloc((size_t)argc + 1, sizeof(*exec_argv));
    int i;

    if (exec_argv == NULL) {
        return NULL;
    }

    exec_argv[0] = (char *)base_name(compiler_path);
    for (i = 1; i < argc; i++) {
        exec_argv[i] = argv[i];
    }
    exec_argv[argc] = NULL;

    return exec_argv;
}

static int write_all(int fd, const char *buffer, size_t size) {
    while (size > 0) {
        ssize_t nwritten = write(fd, buffer, size);
        if (nwritten < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (nwritten == 0) {
            errno = EIO;
            return -1;
        }
        buffer += nwritten;
        size -= (size_t)nwritten;
    }

    return 0;
}

static int mark_ai_thread(const char *proc_ai_thread) {
    char pid_text[32];
    int fd;
    int ret;

    ret = snprintf(pid_text, sizeof(pid_text), "%ld\n", (long)getpid());
    if (ret < 0 || (size_t)ret >= sizeof(pid_text)) {
        errno = EOVERFLOW;
        return -1;
    }

    fd = open(proc_ai_thread, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }

    ret = write_all(fd, pid_text, (size_t)ret);
    if (close(fd) != 0 && ret == 0) {
        ret = -1;
    }

    return ret;
}

static void state_init(struct randcore_state *state) {
    state->next_tie = CLUSTER_A100;
    state->records = NULL;
    state->count = 0;
    state->capacity = 0;
}

static void state_free(struct randcore_state *state) {
    free(state->records);
    state_init(state);
}

static int state_append(struct randcore_state *state, pid_t pid, unsigned long long start_time,
                        enum randcore_cluster cluster) {
    struct state_record *records;
    size_t new_capacity;

    if (state->count == state->capacity) {
        new_capacity = state->capacity == 0 ? 16 : state->capacity * 2;
        records = realloc(state->records, new_capacity * sizeof(*records));
        if (records == NULL) {
            return -1;
        }
        state->records = records;
        state->capacity = new_capacity;
    }

    state->records[state->count].pid = pid;
    state->records[state->count].start_time = start_time;
    state->records[state->count].cluster = cluster;
    state->count++;
    return 0;
}

static void state_remove(struct randcore_state *state, pid_t pid, unsigned long long start_time) {
    size_t i = 0;

    while (i < state->count) {
        const struct state_record *record = &state->records[i];
        if (record->pid == pid && record->start_time == start_time) {
            state->records[i] = state->records[state->count - 1];
            state->count--;
            continue;
        }
        i++;
    }
}

static int read_proc_start_time(pid_t pid, unsigned long long *start_time) {
    char path[64];
    char buffer[4096];
    char *rparen;
    char *cursor;
    int fd;
    int ret;
    ssize_t nread;

    ret = snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
    if (ret < 0 || (size_t)ret >= sizeof(path)) {
        errno = EOVERFLOW;
        return -1;
    }

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }

    do {
        nread = read(fd, buffer, sizeof(buffer) - 1);
    } while (nread < 0 && errno == EINTR);

    ret = errno;
    if (close(fd) != 0 && nread >= 0) {
        return -1;
    }
    if (nread <= 0) {
        errno = nread == 0 ? EINVAL : ret;
        return -1;
    }

    buffer[nread] = '\0';
    rparen = strrchr(buffer, ')');
    if (rparen == NULL) {
        errno = EINVAL;
        return -1;
    }

    cursor = rparen + 1;
    for (int field = 3; field <= 22; field++) {
        char *end;

        while (*cursor == ' ' || *cursor == '\t') {
            cursor++;
        }
        if (*cursor == '\0') {
            errno = EINVAL;
            return -1;
        }

        end = cursor;
        while (*end != '\0' && *end != ' ' && *end != '\t' && *end != '\n' && *end != '\r') {
            end++;
        }

        if (field == 22) {
            char saved = *end;
            char *parse_end = NULL;
            unsigned long long value;
            bool parsed;

            *end = '\0';
            errno = 0;
            value = strtoull(cursor, &parse_end, 10);
            parsed = errno == 0 && parse_end != cursor && parse_end == end;
            *end = saved;
            if (!parsed) {
                errno = EINVAL;
                return -1;
            }
            *start_time = value;
            return 0;
        }

        cursor = end;
    }

    errno = EINVAL;
    return -1;
}

static bool record_is_active(const struct state_record *record) {
    unsigned long long start_time = 0;

    return read_proc_start_time(record->pid, &start_time) == 0 && start_time == record->start_time;
}

static int parse_state_line(struct randcore_state *state, const char *line) {
    char cluster_text[16];
    enum randcore_cluster cluster;
    long pid_value;
    unsigned long long start_time;

    if (line[0] == '#' || line[0] == '\n' || line[0] == '\0') {
        return 0;
    }

    if (sscanf(line, "next_tie %15s", cluster_text) == 1 ||
        sscanf(line, "next_tie=%15s", cluster_text) == 1) {
        if (cluster_from_token(cluster_text, &cluster) == 0) {
            state->next_tie = cluster;
        }
        return 0;
    }

    if (sscanf(line, "%ld %llu %15s", &pid_value, &start_time, cluster_text) == 3 &&
        pid_value > 0 && cluster_from_token(cluster_text, &cluster) == 0) {
        struct state_record record;

        record.pid = (pid_t)pid_value;
        record.start_time = start_time;
        record.cluster = cluster;
        if (record_is_active(&record)) {
            return state_append(state, record.pid, record.start_time, record.cluster);
        }
    }

    return 0;
}

static int read_state_fd(int fd, struct randcore_state *state) {
    char line[256];
    int read_fd;
    int saved_errno = 0;
    FILE *file;

    state_init(state);

    if (lseek(fd, 0, SEEK_SET) < 0) {
        return -1;
    }

    read_fd = dup(fd);
    if (read_fd < 0) {
        return -1;
    }

    file = fdopen(read_fd, "r");
    if (file == NULL) {
        saved_errno = errno;
        close(read_fd);
        errno = saved_errno;
        return -1;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        if (parse_state_line(state, line) != 0) {
            saved_errno = errno;
            break;
        }
    }

    if (saved_errno == 0 && ferror(file)) {
        saved_errno = errno;
    }
    if (fclose(file) != 0 && saved_errno == 0) {
        saved_errno = errno;
    }
    if (saved_errno != 0) {
        state_free(state);
        errno = saved_errno;
        return -1;
    }

    return 0;
}

static int write_state_fd(int fd, const struct randcore_state *state) {
    char line[128];
    int ret;

    if (lseek(fd, 0, SEEK_SET) < 0) {
        return -1;
    }
    if (ftruncate(fd, 0) < 0) {
        return -1;
    }

    if (write_all(fd, "# randcore-compiler state v1\n", strlen("# randcore-compiler state v1\n")) != 0) {
        return -1;
    }

    ret = snprintf(line, sizeof(line), "next_tie %s\n", cluster_token(state->next_tie));
    if (ret < 0 || (size_t)ret >= sizeof(line) || write_all(fd, line, (size_t)ret) != 0) {
        if (ret >= 0 && (size_t)ret >= sizeof(line)) {
            errno = EOVERFLOW;
        }
        return -1;
    }

    for (size_t i = 0; i < state->count; i++) {
        const struct state_record *record = &state->records[i];

        ret = snprintf(line, sizeof(line), "%ld %llu %s\n", (long)record->pid,
                       record->start_time, cluster_token(record->cluster));
        if (ret < 0 || (size_t)ret >= sizeof(line) || write_all(fd, line, (size_t)ret) != 0) {
            if (ret >= 0 && (size_t)ret >= sizeof(line)) {
                errno = EOVERFLOW;
            }
            return -1;
        }
    }

    return 0;
}

static char *make_state_path(const char *state_dir, const char *suffix) {
    const char *separator;
    unsigned long uid = (unsigned long)getuid();
    int needed;
    char *path;

    separator = state_dir[strlen(state_dir) - 1] == '/' ? "" : "/";
    needed = snprintf(NULL, 0, "%s%s%s-%lu.%s", state_dir, separator, STATE_BASENAME, uid, suffix);
    if (needed < 0) {
        return NULL;
    }

    path = malloc((size_t)needed + 1);
    if (path == NULL) {
        return NULL;
    }

    if (snprintf(path, (size_t)needed + 1, "%s%s%s-%lu.%s", state_dir, separator,
                 STATE_BASENAME, uid, suffix) != needed) {
        free(path);
        errno = EOVERFLOW;
        return NULL;
    }

    return path;
}

static void state_files_init(struct state_files *files) {
    files->lock_fd = -1;
    files->state_fd = -1;
    files->lock_path = NULL;
    files->state_path = NULL;
}

static void close_state_files(struct state_files *files) {
    if (files->state_fd >= 0) {
        close(files->state_fd);
    }
    if (files->lock_fd >= 0) {
        close(files->lock_fd);
    }
    free(files->state_path);
    free(files->lock_path);
    state_files_init(files);
}

static int lock_state_file(int fd) {
    struct flock lock;

    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;

    while (fcntl(fd, F_SETLKW, &lock) < 0) {
        if (errno == EINTR) {
            continue;
        }
        return -1;
    }

    return 0;
}

static int open_state_files(const char *state_dir, struct state_files *files) {
    state_files_init(files);

    files->lock_path = make_state_path(state_dir, "lock");
    files->state_path = make_state_path(state_dir, "state");
    if (files->lock_path == NULL || files->state_path == NULL) {
        close_state_files(files);
        return -1;
    }

    files->lock_fd = open(files->lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (files->lock_fd < 0) {
        close_state_files(files);
        return -1;
    }
    if (lock_state_file(files->lock_fd) != 0) {
        close_state_files(files);
        return -1;
    }

    files->state_fd = open(files->state_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (files->state_fd < 0) {
        close_state_files(files);
        return -1;
    }

    return 0;
}

static void warn_errno_path(bool quiet, const char *message, const char *path, int errnum) {
    if (quiet) {
        return;
    }

    if (path == NULL) {
        fprintf(stderr, "randcore-compiler: %s: %s\n", message, strerror(errnum));
    } else {
        fprintf(stderr, "randcore-compiler: %s %s: %s\n", message, path, strerror(errnum));
    }
}

static struct route_decision choose_route(struct randcore_state *state) {
    struct route_decision decision;

    memset(&decision, 0, sizeof(decision));

    for (size_t i = 0; i < state->count; i++) {
        if (state->records[i].cluster == CLUSTER_A100) {
            decision.a100_count++;
        } else {
            decision.x100_count++;
        }
    }

    if (decision.x100_count < decision.a100_count) {
        decision.desired = CLUSTER_X100;
        return decision;
    }
    if (decision.a100_count < decision.x100_count) {
        decision.desired = CLUSTER_A100;
        return decision;
    }

    decision.tied = true;
    decision.desired = state->next_tie;
    state->next_tie = decision.desired == CLUSTER_A100 ? CLUSTER_X100 : CLUSTER_A100;
    return decision;
}

static void send_child_report(int fd, int status, int errnum, enum randcore_cluster cluster) {
    struct child_report report;

    report.status = status;
    report.errnum = errnum;
    report.cluster = (int)cluster;
    (void)write_all(fd, (const char *)&report, sizeof(report));
}

static int read_child_report(int fd, struct child_report *report) {
    char *buffer = (char *)report;
    size_t left = sizeof(*report);

    while (left > 0) {
        ssize_t nread = read(fd, buffer, left);
        if (nread < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (nread == 0) {
            errno = EPIPE;
            return -1;
        }
        buffer += nread;
        left -= (size_t)nread;
    }

    return 0;
}

static int exec_compiler_now(const char *compiler_path, char **exec_argv) {
    int saved_errno;

    if (strchr(compiler_path, '/') != NULL) {
        execv(compiler_path, exec_argv);
    } else {
        execvp(compiler_path, exec_argv);
    }

    saved_errno = errno;
    fprintf(stderr, "randcore-compiler: failed to exec %s: %s\n", compiler_path,
            strerror(saved_errno));
    return saved_errno == ENOENT ? 127 : 126;
}

static void child_exec(enum randcore_cluster desired, const char *proc_ai_thread, bool strict,
                       bool quiet, const char *compiler_path, char **exec_argv, int report_fd) {
    enum randcore_cluster actual = desired;
    int setup_errno = 0;

    if (desired == CLUSTER_A100 && mark_ai_thread(proc_ai_thread) != 0) {
        setup_errno = errno;
        actual = CLUSTER_X100;

        if (!quiet) {
            fprintf(stderr, "randcore-compiler: failed to mark PID %ld as A100 via %s: %s\n",
                    (long)getpid(), proc_ai_thread, strerror(setup_errno));
        }

        if (strict) {
            send_child_report(report_fd, CHILD_SETUP_FAILED, setup_errno, actual);
            close(report_fd);
            _exit(1);
        }
    }

    send_child_report(report_fd, CHILD_SETUP_OK, setup_errno, actual);
    close(report_fd);
    _exit(exec_compiler_now(compiler_path, exec_argv));
}

static int wait_for_child(pid_t pid, bool quiet) {
    int status;

    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) {
            continue;
        }
        warn_errno_path(quiet, "failed to wait for child", NULL, errno);
        return 1;
    }

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }

    return 1;
}

static void cleanup_child_record(const char *state_dir, pid_t pid, unsigned long long start_time,
                                 bool quiet) {
    struct state_files files;
    struct randcore_state state;
    int saved_errno;

    if (open_state_files(state_dir, &files) != 0) {
        warn_errno_path(quiet, "failed to open state for cleanup", state_dir, errno);
        return;
    }

    if (read_state_fd(files.state_fd, &state) != 0) {
        saved_errno = errno;
        close_state_files(&files);
        warn_errno_path(quiet, "failed to read state for cleanup", state_dir, saved_errno);
        return;
    }

    state_remove(&state, pid, start_time);
    if (write_state_fd(files.state_fd, &state) != 0) {
        warn_errno_path(quiet, "failed to write state for cleanup", state_dir, errno);
    }

    state_free(&state);
    close_state_files(&files);
}

static int run_untracked_x100(const char *compiler_path, char **exec_argv, bool log) {
    if (log) {
        fprintf(stderr, "randcore-compiler: state unavailable -> X100/default -> %s\n",
                compiler_path);
    }

    return exec_compiler_now(compiler_path, exec_argv);
}

static int run_balanced(int argc, char **argv, const char *compiler_path,
                        const char *proc_ai_thread, bool strict, bool quiet, bool log) {
    const char *state_dir = getenv("RANDCORE_STATE_DIR");
    struct state_files files;
    struct randcore_state state;
    struct route_decision decision;
    struct child_report report;
    char **exec_argv;
    int pipe_fds[2];
    int saved_errno;
    pid_t child;
    unsigned long long child_start_time = 0;
    bool recorded_child = false;
    int exit_code;

    if (is_empty(state_dir)) {
        state_dir = DEFAULT_STATE_DIR;
    }

    exec_argv = make_exec_argv(argc, argv, compiler_path);
    if (exec_argv == NULL) {
        fprintf(stderr, "randcore-compiler: out of memory\n");
        return 1;
    }

    if (open_state_files(state_dir, &files) != 0) {
        saved_errno = errno;
        warn_errno_path(quiet, "failed to open state directory", state_dir, saved_errno);
        if (strict) {
            free(exec_argv);
            return 1;
        }
        return run_untracked_x100(compiler_path, exec_argv, log);
    }

    if (read_state_fd(files.state_fd, &state) != 0) {
        saved_errno = errno;
        close_state_files(&files);
        warn_errno_path(quiet, "failed to read state", state_dir, saved_errno);
        if (strict) {
            free(exec_argv);
            return 1;
        }
        return run_untracked_x100(compiler_path, exec_argv, log);
    }

    if (write_state_fd(files.state_fd, &state) != 0) {
        saved_errno = errno;
        state_free(&state);
        close_state_files(&files);
        warn_errno_path(quiet, "failed to write state", state_dir, saved_errno);
        if (strict) {
            free(exec_argv);
            return 1;
        }
        return run_untracked_x100(compiler_path, exec_argv, log);
    }

    decision = choose_route(&state);

    if (pipe2(pipe_fds, O_CLOEXEC) != 0) {
        saved_errno = errno;
        state_free(&state);
        close_state_files(&files);
        warn_errno_path(quiet, "failed to create child report pipe", NULL, saved_errno);
        if (strict) {
            free(exec_argv);
            return 1;
        }
        return run_untracked_x100(compiler_path, exec_argv, log);
    }

    child = fork();
    if (child < 0) {
        saved_errno = errno;
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        state_free(&state);
        close_state_files(&files);
        warn_errno_path(quiet, "failed to fork child", NULL, saved_errno);
        if (strict) {
            free(exec_argv);
            return 1;
        }
        return run_untracked_x100(compiler_path, exec_argv, log);
    }

    if (child == 0) {
        close(pipe_fds[0]);
        close_state_files(&files);
        child_exec(decision.desired, proc_ai_thread, strict, quiet, compiler_path, exec_argv,
                   pipe_fds[1]);
    }

    close(pipe_fds[1]);
    if (read_child_report(pipe_fds[0], &report) != 0) {
        saved_errno = errno;
        close(pipe_fds[0]);
        state_free(&state);
        close_state_files(&files);
        warn_errno_path(quiet, "failed to read child setup report", NULL, saved_errno);
        exit_code = wait_for_child(child, quiet);
        free(exec_argv);
        return exit_code;
    }
    close(pipe_fds[0]);

    if (report.status == CHILD_SETUP_OK) {
        enum randcore_cluster actual =
            report.cluster == (int)CLUSTER_A100 ? CLUSTER_A100 : CLUSTER_X100;

        if (read_proc_start_time(child, &child_start_time) == 0 &&
            state_append(&state, child, child_start_time, actual) == 0) {
            recorded_child = true;
        } else if (!quiet) {
            fprintf(stderr, "randcore-compiler: failed to record child PID %ld: %s\n",
                    (long)child, strerror(errno));
        }

        if (log) {
            if (decision.desired == actual) {
                fprintf(stderr, "randcore-compiler: X100 count=%zu A100 count=%zu -> %s -> %s\n",
                        decision.x100_count, decision.a100_count, cluster_name(actual),
                        compiler_path);
            } else {
                fprintf(stderr,
                        "randcore-compiler: X100 count=%zu A100 count=%zu -> %s, fallback %s -> %s\n",
                        decision.x100_count, decision.a100_count, cluster_name(decision.desired),
                        cluster_name(actual), compiler_path);
            }
        }

        if (write_state_fd(files.state_fd, &state) != 0) {
            warn_errno_path(quiet, "failed to write state", state_dir, errno);
            recorded_child = false;
        }
    } else {
        if (log) {
            fprintf(stderr, "randcore-compiler: X100 count=%zu A100 count=%zu -> %s setup failed: %s\n",
                    decision.x100_count, decision.a100_count, cluster_name(decision.desired),
                    strerror(report.errnum));
        }
    }

    state_free(&state);
    close_state_files(&files);

    exit_code = wait_for_child(child, quiet);
    if (recorded_child) {
        cleanup_child_record(state_dir, child, child_start_time, quiet);
    }

    free(exec_argv);
    return exit_code;
}

int main(int argc, char **argv) {
    const char *requested_compiler;
    const char *compiler_path;
    const char *proc_ai_thread = getenv("RANDCORE_SET_AI_THREAD");
    const bool strict = parse_bool_env("RANDCORE_STRICT");
    const bool quiet = parse_bool_env("RANDCORE_QUIET");
    const bool log = parse_bool_env("RANDCORE_LOG");

    if (argc < 1 || argv[0] == NULL) {
        fprintf(stderr, "randcore-compiler: invalid argv[0]\n");
        return 2;
    }

    requested_compiler = requested_compiler_name(argv[0]);
    if (requested_compiler == NULL) {
        fprintf(stderr, "randcore-compiler: invoke as randcore-<compiler>, for example randcore-gcc\n");
        return 2;
    }

    compiler_path = real_compiler_path(requested_compiler);

    if (is_empty(proc_ai_thread)) {
        proc_ai_thread = DEFAULT_PROC_AI_THREAD;
    }

    return run_balanced(argc, argv, compiler_path, proc_ai_thread, strict, quiet, log);
}
