#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <new>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr const char *kDefaultProcAiThread = "/proc/set_ai_thread";
constexpr const char *kDefaultStateDir = "/tmp";
constexpr std::string_view kRandcorePrefix = "randcore-";
constexpr const char *kStateBasename = "randcore-compiler";

enum class Cluster {
    X100 = 0,
    A100 = 1,
};

struct StateRecord {
    pid_t pid = 0;
    unsigned long long start_time = 0;
    Cluster cluster = Cluster::X100;
};

struct RouteDecision {
    Cluster desired = Cluster::X100;
    std::size_t x100_count = 0;
    std::size_t a100_count = 0;
    bool tied = false;
};

struct ChildReport {
    int status = 0;
    int errnum = 0;
    int cluster = 0;
};

constexpr int kChildSetupOk = 0;
constexpr int kChildSetupFailed = 1;

class UniqueFd {
  public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) : fd_(fd) {}
    ~UniqueFd() { reset(); }

    UniqueFd(const UniqueFd &) = delete;
    UniqueFd &operator=(const UniqueFd &) = delete;

    UniqueFd(UniqueFd &&other) noexcept : fd_(other.release()) {}

    UniqueFd &operator=(UniqueFd &&other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    int get() const { return fd_; }
    bool valid() const { return fd_ >= 0; }

    int release() {
        int fd = fd_;
        fd_ = -1;
        return fd;
    }

    void reset(int fd = -1) {
        if (fd_ >= 0) {
            close(fd_);
        }
        fd_ = fd;
    }

  private:
    int fd_ = -1;
};

class StateFiles {
  public:
    bool open(const std::string &state_dir) {
        lock_path_ = make_path(state_dir, "lock");
        state_path_ = make_path(state_dir, "state");

        lock_fd_.reset(::open(lock_path_.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600));
        if (!lock_fd_.valid() || !lock_state_file()) {
            return false;
        }

        state_fd_.reset(::open(state_path_.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600));
        return state_fd_.valid();
    }

    int state_fd() const { return state_fd_.get(); }

    void close() {
        state_fd_.reset();
        lock_fd_.reset();
    }

  private:
    static std::string make_path(const std::string &state_dir, const char *suffix) {
        std::string path = state_dir;
        if (!path.empty() && path.back() != '/') {
            path += '/';
        }

        path += kStateBasename;
        path += '-';
        path += std::to_string(static_cast<unsigned long>(getuid()));
        path += '.';
        path += suffix;
        return path;
    }

    bool lock_state_file() const {
        flock lock{};
        lock.l_type = F_WRLCK;
        lock.l_whence = SEEK_SET;

        while (fcntl(lock_fd_.get(), F_SETLKW, &lock) < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }

        return true;
    }

    UniqueFd lock_fd_;
    UniqueFd state_fd_;
    std::string lock_path_;
    std::string state_path_;
};

bool is_empty(const char *value) {
    return value == nullptr || value[0] == '\0';
}

bool parse_bool_env(const char *name) {
    const char *value = getenv(name);

    if (is_empty(value)) {
        return false;
    }

    return std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 ||
           std::strcmp(value, "TRUE") == 0 || std::strcmp(value, "yes") == 0 ||
           std::strcmp(value, "YES") == 0 || std::strcmp(value, "on") == 0 ||
           std::strcmp(value, "ON") == 0;
}

const char *cluster_name(Cluster cluster) {
    return cluster == Cluster::A100 ? "A100" : "X100";
}

const char *cluster_token(Cluster cluster) {
    return cluster == Cluster::A100 ? "a100" : "x100";
}

bool cluster_from_token(const std::string &token, Cluster &cluster) {
    if (token == "x100" || token == "X100") {
        cluster = Cluster::X100;
        return true;
    }
    if (token == "a100" || token == "A100") {
        cluster = Cluster::A100;
        return true;
    }

    return false;
}

std::string base_name(const char *path) {
    const char *slash = std::strrchr(path, '/');
    return slash == nullptr ? path : slash + 1;
}

std::string requested_compiler_name(const char *argv0) {
    std::string name = base_name(argv0);

    if (name.rfind(kRandcorePrefix, 0) != 0) {
        return {};
    }

    name.erase(0, kRandcorePrefix.size());
    return name;
}

std::vector<char *> make_exec_argv(int argc, char **argv, const std::string &compiler_name) {
    std::vector<char *> exec_argv;
    exec_argv.reserve(static_cast<std::size_t>(argc) + 1);
    exec_argv.push_back(const_cast<char *>(compiler_name.c_str()));

    for (int i = 1; i < argc; ++i) {
        exec_argv.push_back(argv[i]);
    }

    exec_argv.push_back(nullptr);
    return exec_argv;
}

int write_all(int fd, const void *data, std::size_t size) {
    const auto *buffer = static_cast<const char *>(data);

    while (size > 0) {
        ssize_t written = write(fd, buffer, size);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (written == 0) {
            errno = EIO;
            return -1;
        }
        buffer += written;
        size -= static_cast<std::size_t>(written);
    }

    return 0;
}

int mark_ai_thread(const std::string &proc_ai_thread) {
    char pid_text[32];
    int length = std::snprintf(pid_text, sizeof(pid_text), "%ld\n", static_cast<long>(getpid()));
    if (length < 0 || static_cast<std::size_t>(length) >= sizeof(pid_text)) {
        errno = EOVERFLOW;
        return -1;
    }

    UniqueFd fd(open(proc_ai_thread.c_str(), O_WRONLY | O_CLOEXEC));
    if (!fd.valid()) {
        return -1;
    }

    return write_all(fd.get(), pid_text, static_cast<std::size_t>(length));
}

bool parse_unsigned_long_long(const std::string &text, unsigned long long &value) {
    char *end = nullptr;
    errno = 0;
    value = std::strtoull(text.c_str(), &end, 10);
    return errno == 0 && end != text.c_str() && *end == '\0';
}

bool parse_long(const std::string &text, long &value) {
    char *end = nullptr;
    errno = 0;
    value = std::strtol(text.c_str(), &end, 10);
    return errno == 0 && end != text.c_str() && *end == '\0';
}

bool read_proc_start_time(pid_t pid, unsigned long long &start_time) {
    char path[64];
    int length = std::snprintf(path, sizeof(path), "/proc/%ld/stat", static_cast<long>(pid));
    if (length < 0 || static_cast<std::size_t>(length) >= sizeof(path)) {
        errno = EOVERFLOW;
        return false;
    }

    UniqueFd fd(open(path, O_RDONLY | O_CLOEXEC));
    if (!fd.valid()) {
        return false;
    }

    char buffer[4096];
    ssize_t bytes_read;
    do {
        bytes_read = read(fd.get(), buffer, sizeof(buffer) - 1);
    } while (bytes_read < 0 && errno == EINTR);

    if (bytes_read <= 0) {
        errno = bytes_read == 0 ? EINVAL : errno;
        return false;
    }

    buffer[bytes_read] = '\0';
    std::string stat(buffer);
    std::size_t rparen = stat.rfind(')');
    if (rparen == std::string::npos) {
        errno = EINVAL;
        return false;
    }

    std::istringstream fields(stat.substr(rparen + 1));
    std::string token;
    for (int field = 3; field <= 22; ++field) {
        if (!(fields >> token)) {
            errno = EINVAL;
            return false;
        }
        if (field == 22) {
            if (!parse_unsigned_long_long(token, start_time)) {
                errno = EINVAL;
                return false;
            }
            return true;
        }
    }

    errno = EINVAL;
    return false;
}

bool record_is_active(const StateRecord &record) {
    unsigned long long start_time = 0;
    return read_proc_start_time(record.pid, start_time) && start_time == record.start_time;
}

class RandcoreState {
  public:
    void parse_line(const std::string &line) {
        if (line.empty() || line[0] == '#') {
            return;
        }

        std::istringstream fields(line);
        std::string first;
        if (!(fields >> first)) {
            return;
        }

        if (first == "next_tie") {
            std::string cluster_text;
            Cluster cluster;
            if (fields >> cluster_text && cluster_from_token(cluster_text, cluster)) {
                next_tie_ = cluster;
            }
            return;
        }

        constexpr std::string_view next_tie_equals = "next_tie=";
        if (first.rfind(next_tie_equals, 0) == 0) {
            Cluster cluster;
            if (cluster_from_token(first.substr(next_tie_equals.size()), cluster)) {
                next_tie_ = cluster;
            }
            return;
        }

        long pid_value = 0;
        unsigned long long start_time = 0;
        std::string start_text;
        std::string cluster_text;
        Cluster cluster;
        if (!parse_long(first, pid_value) || pid_value <= 0 ||
            !(fields >> start_text >> cluster_text) ||
            !parse_unsigned_long_long(start_text, start_time) ||
            !cluster_from_token(cluster_text, cluster)) {
            return;
        }

        append_if_active(StateRecord{static_cast<pid_t>(pid_value), start_time, cluster});
    }

    void append_if_active(const StateRecord &record) {
        if (record_is_active(record)) {
            records_.push_back(record);
        }
    }

    void append(pid_t pid, unsigned long long start_time, Cluster cluster) {
        records_.push_back(StateRecord{pid, start_time, cluster});
    }

    void remove(pid_t pid, unsigned long long start_time) {
        auto kept = std::vector<StateRecord>{};
        kept.reserve(records_.size());

        for (const StateRecord &record : records_) {
            if (record.pid != pid || record.start_time != start_time) {
                kept.push_back(record);
            }
        }

        records_ = std::move(kept);
    }

    RouteDecision choose_route() {
        RouteDecision decision;

        for (const StateRecord &record : records_) {
            if (record.cluster == Cluster::A100) {
                ++decision.a100_count;
            } else {
                ++decision.x100_count;
            }
        }

        if (decision.x100_count < decision.a100_count) {
            decision.desired = Cluster::X100;
            return decision;
        }
        if (decision.a100_count < decision.x100_count) {
            decision.desired = Cluster::A100;
            return decision;
        }

        decision.tied = true;
        decision.desired = next_tie_;
        next_tie_ = decision.desired == Cluster::A100 ? Cluster::X100 : Cluster::A100;
        return decision;
    }

    std::string serialize() const {
        std::string output = "# randcore-compiler state v1\n";
        output += "next_tie ";
        output += cluster_token(next_tie_);
        output += '\n';

        for (const StateRecord &record : records_) {
            output += std::to_string(static_cast<long>(record.pid));
            output += ' ';
            output += std::to_string(record.start_time);
            output += ' ';
            output += cluster_token(record.cluster);
            output += '\n';
        }

        return output;
    }

  private:
    Cluster next_tie_ = Cluster::A100;
    std::vector<StateRecord> records_;
};

bool read_state_fd(int fd, RandcoreState &state) {
    RandcoreState parsed;

    if (lseek(fd, 0, SEEK_SET) < 0) {
        return false;
    }

    std::string content;
    char buffer[4096];
    while (true) {
        ssize_t bytes_read = read(fd, buffer, sizeof(buffer));
        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (bytes_read == 0) {
            break;
        }
        content.append(buffer, static_cast<std::size_t>(bytes_read));
    }

    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        parsed.parse_line(line);
    }

    state = std::move(parsed);
    return true;
}

bool write_state_fd(int fd, const RandcoreState &state) {
    if (lseek(fd, 0, SEEK_SET) < 0 || ftruncate(fd, 0) < 0) {
        return false;
    }

    std::string output = state.serialize();
    return write_all(fd, output.data(), output.size()) == 0;
}

void warn_errno_path(bool quiet, const char *message, const char *path, int errnum) {
    if (quiet) {
        return;
    }

    if (path == nullptr) {
        std::fprintf(stderr, "randcore-compiler: %s: %s\n", message, std::strerror(errnum));
    } else {
        std::fprintf(stderr, "randcore-compiler: %s %s: %s\n", message, path,
                     std::strerror(errnum));
    }
}

void send_child_report(int fd, int status, int errnum, Cluster cluster) {
    ChildReport report{status, errnum, static_cast<int>(cluster)};
    (void)write_all(fd, &report, sizeof(report));
}

bool read_child_report(int fd, ChildReport &report) {
    auto *buffer = reinterpret_cast<char *>(&report);
    std::size_t left = sizeof(report);

    while (left > 0) {
        ssize_t bytes_read = read(fd, buffer, left);
        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (bytes_read == 0) {
            errno = EPIPE;
            return false;
        }
        buffer += bytes_read;
        left -= static_cast<std::size_t>(bytes_read);
    }

    return true;
}

int exec_compiler_now(const std::string &compiler_name, std::vector<char *> &exec_argv) {
    execvp(compiler_name.c_str(), exec_argv.data());

    int saved_errno = errno;
    std::fprintf(stderr, "randcore-compiler: failed to exec %s: %s\n", compiler_name.c_str(),
                 std::strerror(saved_errno));
    return saved_errno == ENOENT ? 127 : 126;
}

void child_exec(Cluster desired, const std::string &proc_ai_thread, bool strict, bool quiet,
                const std::string &compiler_name, std::vector<char *> &exec_argv, int report_fd) {
    Cluster actual = desired;
    int setup_errno = 0;

    if (desired == Cluster::A100 && mark_ai_thread(proc_ai_thread) != 0) {
        setup_errno = errno;
        actual = Cluster::X100;

        if (!quiet) {
            std::fprintf(stderr,
                         "randcore-compiler: failed to mark PID %ld as A100 via %s: %s\n",
                         static_cast<long>(getpid()), proc_ai_thread.c_str(),
                         std::strerror(setup_errno));
        }

        if (strict) {
            send_child_report(report_fd, kChildSetupFailed, setup_errno, actual);
            close(report_fd);
            _exit(1);
        }
    }

    send_child_report(report_fd, kChildSetupOk, setup_errno, actual);
    close(report_fd);
    _exit(exec_compiler_now(compiler_name, exec_argv));
}

int wait_for_child(pid_t pid, bool quiet) {
    int status = 0;

    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) {
            continue;
        }
        warn_errno_path(quiet, "failed to wait for child", nullptr, errno);
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

void cleanup_child_record(const std::string &state_dir, pid_t pid, unsigned long long start_time,
                          bool quiet) {
    StateFiles files;
    if (!files.open(state_dir)) {
        warn_errno_path(quiet, "failed to open state for cleanup", state_dir.c_str(), errno);
        return;
    }

    RandcoreState state;
    if (!read_state_fd(files.state_fd(), state)) {
        warn_errno_path(quiet, "failed to read state for cleanup", state_dir.c_str(), errno);
        return;
    }

    state.remove(pid, start_time);
    if (!write_state_fd(files.state_fd(), state)) {
        warn_errno_path(quiet, "failed to write state for cleanup", state_dir.c_str(), errno);
    }
}

int run_untracked_x100(const std::string &compiler_name, std::vector<char *> &exec_argv,
                       bool log) {
    if (log) {
        std::fprintf(stderr, "randcore-compiler: state unavailable -> X100/default -> %s\n",
                     compiler_name.c_str());
    }

    return exec_compiler_now(compiler_name, exec_argv);
}

int run_balanced(int argc, char **argv, const std::string &compiler_name,
                 const std::string &proc_ai_thread, bool strict, bool quiet, bool log) {
    const char *state_dir_env = getenv("RANDCORE_STATE_DIR");
    std::string state_dir = is_empty(state_dir_env) ? kDefaultStateDir : state_dir_env;
    std::vector<char *> exec_argv = make_exec_argv(argc, argv, compiler_name);

    StateFiles files;
    if (!files.open(state_dir)) {
        int saved_errno = errno;
        warn_errno_path(quiet, "failed to open state directory", state_dir.c_str(), saved_errno);
        return strict ? 1 : run_untracked_x100(compiler_name, exec_argv, log);
    }

    RandcoreState state;
    if (!read_state_fd(files.state_fd(), state)) {
        int saved_errno = errno;
        files.close();
        warn_errno_path(quiet, "failed to read state", state_dir.c_str(), saved_errno);
        return strict ? 1 : run_untracked_x100(compiler_name, exec_argv, log);
    }

    if (!write_state_fd(files.state_fd(), state)) {
        int saved_errno = errno;
        files.close();
        warn_errno_path(quiet, "failed to write state", state_dir.c_str(), saved_errno);
        return strict ? 1 : run_untracked_x100(compiler_name, exec_argv, log);
    }

    RouteDecision decision = state.choose_route();

    int pipe_fds[2];
    if (pipe2(pipe_fds, O_CLOEXEC) != 0) {
        int saved_errno = errno;
        files.close();
        warn_errno_path(quiet, "failed to create child report pipe", nullptr, saved_errno);
        return strict ? 1 : run_untracked_x100(compiler_name, exec_argv, log);
    }
    UniqueFd read_fd(pipe_fds[0]);
    UniqueFd write_fd(pipe_fds[1]);

    pid_t child = fork();
    if (child < 0) {
        int saved_errno = errno;
        files.close();
        warn_errno_path(quiet, "failed to fork child", nullptr, saved_errno);
        return strict ? 1 : run_untracked_x100(compiler_name, exec_argv, log);
    }

    if (child == 0) {
        read_fd.reset();
        files.close();
        child_exec(decision.desired, proc_ai_thread, strict, quiet, compiler_name, exec_argv,
                   write_fd.release());
    }

    write_fd.reset();
    ChildReport report;
    if (!read_child_report(read_fd.get(), report)) {
        int saved_errno = errno;
        read_fd.reset();
        files.close();
        warn_errno_path(quiet, "failed to read child setup report", nullptr, saved_errno);
        return wait_for_child(child, quiet);
    }
    read_fd.reset();

    unsigned long long child_start_time = 0;
    bool recorded_child = false;
    if (report.status == kChildSetupOk) {
        Cluster actual = report.cluster == static_cast<int>(Cluster::A100) ? Cluster::A100
                                                                           : Cluster::X100;

        if (read_proc_start_time(child, child_start_time)) {
            state.append(child, child_start_time, actual);
            recorded_child = true;
        } else if (!quiet) {
            std::fprintf(stderr, "randcore-compiler: failed to record child PID %ld: %s\n",
                         static_cast<long>(child), std::strerror(errno));
        }

        if (log) {
            if (decision.desired == actual) {
                std::fprintf(stderr,
                             "randcore-compiler: X100 count=%zu A100 count=%zu -> %s -> %s\n",
                             decision.x100_count, decision.a100_count, cluster_name(actual),
                             compiler_name.c_str());
            } else {
                std::fprintf(stderr,
                             "randcore-compiler: X100 count=%zu A100 count=%zu -> %s, fallback %s -> %s\n",
                             decision.x100_count, decision.a100_count,
                             cluster_name(decision.desired), cluster_name(actual),
                             compiler_name.c_str());
            }
        }

        if (!write_state_fd(files.state_fd(), state)) {
            warn_errno_path(quiet, "failed to write state", state_dir.c_str(), errno);
            recorded_child = false;
        }
    } else if (log) {
        std::fprintf(stderr,
                     "randcore-compiler: X100 count=%zu A100 count=%zu -> %s setup failed: %s\n",
                     decision.x100_count, decision.a100_count, cluster_name(decision.desired),
                     std::strerror(report.errnum));
    }

    files.close();

    int exit_code = wait_for_child(child, quiet);
    if (recorded_child) {
        cleanup_child_record(state_dir, child, child_start_time, quiet);
    }

    return exit_code;
}

int real_main(int argc, char **argv) {
    if (argc < 1 || argv[0] == nullptr) {
        std::fprintf(stderr, "randcore-compiler: invalid argv[0]\n");
        return 2;
    }

    std::string compiler_name = requested_compiler_name(argv[0]);
    if (compiler_name.empty()) {
        std::fprintf(stderr,
                     "randcore-compiler: invoke as randcore-<compiler>, for example randcore-gcc\n");
        return 2;
    }

    const char *proc_ai_thread_env = getenv("RANDCORE_SET_AI_THREAD");
    std::string proc_ai_thread =
        is_empty(proc_ai_thread_env) ? kDefaultProcAiThread : proc_ai_thread_env;

    return run_balanced(argc, argv, compiler_name, proc_ai_thread,
                        parse_bool_env("RANDCORE_STRICT"), parse_bool_env("RANDCORE_QUIET"),
                        parse_bool_env("RANDCORE_LOG"));
}

} // namespace

int main(int argc, char **argv) {
    try {
        return real_main(argc, argv);
    } catch (const std::bad_alloc &) {
        std::fprintf(stderr, "randcore-compiler: out of memory\n");
        return 1;
    }
}
