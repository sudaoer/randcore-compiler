#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <limits.h>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

constexpr const char *kDefaultProcAiThread = "/proc/set_ai_thread";
constexpr const char *kDefaultMatchNames = "*gcc,*g++,gcc,g++,cc,c++,clang,clang++";
constexpr int kDefaultScanIntervalMs = 100;

enum class Cluster {
    X100 = 0,
    A100 = 1,
};

struct ProcessInfo {
    pid_t pid = 0;
    pid_t ppid = 0;
    unsigned long long start_time = 0;
};

struct ManagedProcess {
    pid_t pid = 0;
    unsigned long long start_time = 0;
    Cluster cluster = Cluster::X100;
};

struct Counts {
    std::size_t x100 = 0;
    std::size_t a100 = 0;
};

struct MarkAiResult {
    std::size_t attempted = 0;
    std::size_t marked = 0;
    std::size_t skipped_exited = 0;
    std::size_t failed = 0;
    pid_t first_failed_pid = 0;
    int first_errno = 0;
    bool root_marked = false;
    bool root_exited = false;
    bool root_failed = false;
};

struct Config {
    std::vector<pid_t> parent_pids;
    std::vector<std::string> match_patterns;
    std::string proc_ai_thread = kDefaultProcAiThread;
    int scan_interval_ms = kDefaultScanIntervalMs;
    bool strict = false;
    bool log = false;
    bool quiet = false;
    bool dry_run = false;
    bool once = false;
};

volatile sig_atomic_t g_stop_requested = 0;

void handle_signal(int) {
    g_stop_requested = 1;
}

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

bool parse_long(const std::string &text, long &value) {
    char *end = nullptr;
    errno = 0;
    value = std::strtol(text.c_str(), &end, 10);
    return errno == 0 && end != text.c_str() && *end == '\0';
}

bool parse_unsigned_long_long(const std::string &text, unsigned long long &value) {
    char *end = nullptr;
    errno = 0;
    value = std::strtoull(text.c_str(), &end, 10);
    return errno == 0 && end != text.c_str() && *end == '\0';
}

const char *cluster_name(Cluster cluster) {
    return cluster == Cluster::A100 ? "A100" : "X100";
}

std::string trim_copy(std::string text) {
    auto is_not_space = [](unsigned char ch) { return !std::isspace(ch); };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), is_not_space));
    text.erase(std::find_if(text.rbegin(), text.rend(), is_not_space).base(), text.end());
    return text;
}

std::vector<std::string> split_list(std::string_view text) {
    std::vector<std::string> tokens;
    std::string token;

    auto flush = [&]() {
        token = trim_copy(token);
        if (!token.empty()) {
            tokens.push_back(token);
        }
        token.clear();
    };

    for (char ch : text) {
        if (ch == ',' || ch == ';' || std::isspace(static_cast<unsigned char>(ch))) {
            flush();
        } else {
            token.push_back(ch);
        }
    }
    flush();

    return tokens;
}

std::vector<pid_t> parse_pid_list(const char *value) {
    std::vector<pid_t> pids;
    if (is_empty(value)) {
        return pids;
    }

    for (const std::string &token : split_list(value)) {
        long parsed = 0;
        if (!parse_long(token, parsed) || parsed <= 0) {
            std::fprintf(stderr, "randcore-child-balancer: ignoring invalid PID '%s'\n",
                         token.c_str());
            continue;
        }
        pids.push_back(static_cast<pid_t>(parsed));
    }

    std::sort(pids.begin(), pids.end());
    pids.erase(std::unique(pids.begin(), pids.end()), pids.end());
    return pids;
}

int parse_interval_ms(const char *value) {
    if (is_empty(value)) {
        return kDefaultScanIntervalMs;
    }

    long parsed = 0;
    if (!parse_long(value, parsed) || parsed < 10 || parsed > 60000) {
        std::fprintf(stderr,
                     "randcore-child-balancer: invalid RANDCORE_SCAN_INTERVAL_MS='%s', using %d\n",
                     value, kDefaultScanIntervalMs);
        return kDefaultScanIntervalMs;
    }

    return static_cast<int>(parsed);
}

std::string basename_of(const std::string &path) {
    if (path.empty()) {
        return {};
    }

    std::size_t end = path.find_last_not_of('/');
    if (end == std::string::npos) {
        return {};
    }

    std::size_t slash = path.find_last_of('/', end);
    if (slash == std::string::npos) {
        return path.substr(0, end + 1);
    }
    return path.substr(slash + 1, end - slash);
}

bool read_file_limited(const std::string &path, std::string &content, std::size_t max_bytes) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }

    content.clear();
    char buffer[4096];
    while (content.size() < max_bytes) {
        std::size_t wanted = std::min(sizeof(buffer), max_bytes - content.size());
        ssize_t bytes_read = read(fd, buffer, wanted);
        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            return false;
        }
        if (bytes_read == 0) {
            break;
        }
        content.append(buffer, static_cast<std::size_t>(bytes_read));
    }

    close(fd);
    return true;
}

bool read_proc_stat(pid_t pid, ProcessInfo &info) {
    char path[64];
    int length = std::snprintf(path, sizeof(path), "/proc/%ld/stat", static_cast<long>(pid));
    if (length < 0 || static_cast<std::size_t>(length) >= sizeof(path)) {
        errno = EOVERFLOW;
        return false;
    }

    std::string stat;
    if (!read_file_limited(path, stat, 4096)) {
        return false;
    }

    std::size_t rparen = stat.rfind(')');
    if (rparen == std::string::npos) {
        errno = EINVAL;
        return false;
    }

    std::istringstream fields(stat.substr(rparen + 1));
    std::string token;
    pid_t ppid = 0;
    unsigned long long start_time = 0;

    for (int field = 3; field <= 22; ++field) {
        if (!(fields >> token)) {
            errno = EINVAL;
            return false;
        }

        if (field == 4) {
            long parsed_ppid = 0;
            if (!parse_long(token, parsed_ppid)) {
                errno = EINVAL;
                return false;
            }
            ppid = static_cast<pid_t>(parsed_ppid);
        } else if (field == 22) {
            if (!parse_unsigned_long_long(token, start_time)) {
                errno = EINVAL;
                return false;
            }
        }
    }

    info = ProcessInfo{pid, ppid, start_time};
    return true;
}

bool parse_pid_dir_name(const char *name, pid_t &pid) {
    if (name == nullptr || name[0] == '\0') {
        return false;
    }
    for (const char *ch = name; *ch != '\0'; ++ch) {
        if (!std::isdigit(static_cast<unsigned char>(*ch))) {
            return false;
        }
    }

    long parsed = 0;
    if (!parse_long(name, parsed) || parsed <= 0) {
        return false;
    }

    pid = static_cast<pid_t>(parsed);
    return true;
}

std::unordered_map<pid_t, ProcessInfo> snapshot_processes() {
    std::unordered_map<pid_t, ProcessInfo> processes;
    DIR *proc = opendir("/proc");
    if (proc == nullptr) {
        std::fprintf(stderr, "randcore-child-balancer: failed to open /proc: %s\n",
                     std::strerror(errno));
        return processes;
    }

    while (dirent *entry = readdir(proc)) {
        pid_t pid = 0;
        if (!parse_pid_dir_name(entry->d_name, pid)) {
            continue;
        }

        ProcessInfo info;
        if (read_proc_stat(pid, info)) {
            processes.emplace(pid, info);
        }
    }

    closedir(proc);
    return processes;
}

std::unordered_map<pid_t, std::vector<pid_t>>
build_children(const std::unordered_map<pid_t, ProcessInfo> &processes) {
    std::unordered_map<pid_t, std::vector<pid_t>> children;
    for (const auto &entry : processes) {
        children[entry.second.ppid].push_back(entry.second.pid);
    }

    for (auto &entry : children) {
        std::sort(entry.second.begin(), entry.second.end());
    }
    return children;
}

void add_unique(std::vector<std::string> &names, const std::string &name) {
    if (name.empty()) {
        return;
    }
    if (std::find(names.begin(), names.end(), name) == names.end()) {
        names.push_back(name);
    }
}

std::vector<std::string> process_names(pid_t pid) {
    std::vector<std::string> names;
    char path[64];

    int length = std::snprintf(path, sizeof(path), "/proc/%ld/exe", static_cast<long>(pid));
    if (length > 0 && static_cast<std::size_t>(length) < sizeof(path)) {
        char target[PATH_MAX + 1];
        ssize_t read_len = readlink(path, target, PATH_MAX);
        if (read_len > 0) {
            target[read_len] = '\0';
            add_unique(names, basename_of(target));
        }
    }

    length = std::snprintf(path, sizeof(path), "/proc/%ld/cmdline", static_cast<long>(pid));
    if (length > 0 && static_cast<std::size_t>(length) < sizeof(path)) {
        std::string cmdline;
        if (read_file_limited(path, cmdline, 4096) && !cmdline.empty()) {
            std::size_t nul = cmdline.find('\0');
            std::string argv0 = cmdline.substr(0, nul);
            add_unique(names, basename_of(argv0.empty() ? std::string{} : argv0));
        }
    }

    length = std::snprintf(path, sizeof(path), "/proc/%ld/comm", static_cast<long>(pid));
    if (length > 0 && static_cast<std::size_t>(length) < sizeof(path)) {
        std::string comm;
        if (read_file_limited(path, comm, 256)) {
            add_unique(names, trim_copy(comm));
        }
    }

    return names;
}

std::string find_matching_name(pid_t pid, const std::vector<std::string> &patterns) {
    std::vector<std::string> names = process_names(pid);
    for (const std::string &name : names) {
        for (const std::string &pattern : patterns) {
            if (fnmatch(pattern.c_str(), name.c_str(), 0) == 0) {
                return name;
            }
        }
    }
    return {};
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

bool mark_ai_thread(pid_t pid, const std::string &proc_ai_thread) {
    char pid_text[32];
    int length = std::snprintf(pid_text, sizeof(pid_text), "%ld\n", static_cast<long>(pid));
    if (length < 0 || static_cast<std::size_t>(length) >= sizeof(pid_text)) {
        errno = EOVERFLOW;
        return false;
    }

    int fd = open(proc_ai_thread.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }

    bool ok = write_all(fd, pid_text, static_cast<std::size_t>(length)) == 0;
    int saved_errno = errno;
    close(fd);
    errno = saved_errno;
    return ok;
}

bool process_still_matches(pid_t pid, unsigned long long start_time) {
    ProcessInfo current;
    return read_proc_stat(pid, current) && current.start_time == start_time;
}

bool parse_cpu_number(const std::string &text, int &value) {
    long parsed = 0;
    if (!parse_long(text, parsed) || parsed < 0 || parsed > 1024) {
        return false;
    }
    value = static_cast<int>(parsed);
    return true;
}

void classify_cpu(int cpu, bool &has_x100, bool &has_a100, bool &has_other) {
    if (cpu >= 0 && cpu <= 7) {
        has_x100 = true;
    } else if (cpu >= 8 && cpu <= 15) {
        has_a100 = true;
    } else {
        has_other = true;
    }
}

bool detect_cluster_from_cpu_list(const std::string &cpu_list, Cluster &cluster) {
    bool has_x100 = false;
    bool has_a100 = false;
    bool has_other = false;

    for (const std::string &part : split_list(cpu_list)) {
        std::size_t dash = part.find('-');
        if (dash == std::string::npos) {
            int cpu = 0;
            if (!parse_cpu_number(part, cpu)) {
                return false;
            }
            classify_cpu(cpu, has_x100, has_a100, has_other);
            continue;
        }

        int first = 0;
        int last = 0;
        if (!parse_cpu_number(part.substr(0, dash), first) ||
            !parse_cpu_number(part.substr(dash + 1), last) || first > last) {
            return false;
        }
        for (int cpu = first; cpu <= last; ++cpu) {
            classify_cpu(cpu, has_x100, has_a100, has_other);
        }
    }

    if (has_a100 && !has_x100 && !has_other) {
        cluster = Cluster::A100;
        return true;
    }
    if (has_x100 && !has_a100 && !has_other) {
        cluster = Cluster::X100;
        return true;
    }
    return false;
}

bool detect_current_cluster(pid_t pid, Cluster &cluster) {
    char path[64];
    int length = std::snprintf(path, sizeof(path), "/proc/%ld/status", static_cast<long>(pid));
    if (length < 0 || static_cast<std::size_t>(length) >= sizeof(path)) {
        return false;
    }

    std::string status;
    if (!read_file_limited(path, status, 65536)) {
        return false;
    }

    std::istringstream lines(status);
    std::string line;
    while (std::getline(lines, line)) {
        constexpr std::string_view prefix = "Cpus_allowed_list:";
        if (line.rfind(prefix, 0) == 0) {
            return detect_cluster_from_cpu_list(trim_copy(line.substr(prefix.size())), cluster);
        }
    }

    return false;
}

void sleep_ms(int milliseconds) {
    timespec remaining{};
    remaining.tv_sec = milliseconds / 1000;
    remaining.tv_nsec = static_cast<long>(milliseconds % 1000) * 1000000L;

    while (!g_stop_requested && nanosleep(&remaining, &remaining) < 0) {
        if (errno != EINTR) {
            return;
        }
    }
}

class ChildBalancer {
  public:
    explicit ChildBalancer(Config config) : config_(std::move(config)) {}

    int run() {
        if (config_.parent_pids.empty() && !config_.quiet) {
            std::fprintf(stderr,
                         "randcore-child-balancer: RANDCORE_PARENT_PIDS is empty; nothing to scan\n");
        }

        while (!g_stop_requested) {
            if (!scan_once()) {
                return 1;
            }
            if (config_.once) {
                break;
            }
            sleep_ms(config_.scan_interval_ms);
        }

        return 0;
    }

  private:
    bool scan_once() {
        auto processes = snapshot_processes();
        auto children = build_children(processes);
        cleanup(processes);

        std::unordered_set<pid_t> visited;
        for (pid_t root : config_.parent_pids) {
            auto child_it = children.find(root);
            if (child_it == children.end()) {
                if (config_.log) {
                    std::fprintf(stderr,
                                 "randcore-child-balancer: parent PID %ld has no visible children\n",
                                 static_cast<long>(root));
                }
                continue;
            }

            for (pid_t child : child_it->second) {
                if (!scan_subtree(child, processes, children, visited)) {
                    return false;
                }
            }
        }

        return true;
    }

    void cleanup(const std::unordered_map<pid_t, ProcessInfo> &processes) {
        for (auto it = managed_.begin(); it != managed_.end();) {
            auto proc_it = processes.find(it->first);
            if (proc_it == processes.end() ||
                proc_it->second.start_time != it->second.start_time) {
                it = managed_.erase(it);
            } else {
                ++it;
            }
        }
    }

    Counts counts() const {
        Counts result;
        for (const auto &entry : managed_) {
            if (entry.second.cluster == Cluster::A100) {
                ++result.a100;
            } else {
                ++result.x100;
            }
        }
        return result;
    }

    Cluster choose_route(const Counts &current, bool &tied) {
        tied = false;
        if (current.x100 < current.a100) {
            return Cluster::X100;
        }
        if (current.a100 < current.x100) {
            return Cluster::A100;
        }

        tied = true;
        Cluster selected = next_tie_;
        next_tie_ = selected == Cluster::A100 ? Cluster::X100 : Cluster::A100;
        return selected;
    }

    bool scan_subtree(pid_t pid, const std::unordered_map<pid_t, ProcessInfo> &processes,
                      const std::unordered_map<pid_t, std::vector<pid_t>> &children,
                      std::unordered_set<pid_t> &visited) {
        if (!visited.insert(pid).second) {
            return true;
        }

        auto proc_it = processes.find(pid);
        if (proc_it == processes.end()) {
            return true;
        }

        auto managed_it = managed_.find(pid);
        if (managed_it != managed_.end() &&
            managed_it->second.start_time == proc_it->second.start_time) {
            return true;
        }

        std::string matched_name = find_matching_name(pid, config_.match_patterns);
        if (!matched_name.empty()) {
            return manage_process(proc_it->second, matched_name, processes, children);
        }

        auto child_it = children.find(pid);
        if (child_it == children.end()) {
            return true;
        }

        for (pid_t child : child_it->second) {
            if (!scan_subtree(child, processes, children, visited)) {
                return false;
            }
        }
        return true;
    }

    std::vector<ProcessInfo>
    collect_subtree(const ProcessInfo &root,
                    const std::unordered_map<pid_t, ProcessInfo> &processes,
                    const std::unordered_map<pid_t, std::vector<pid_t>> &children) const {
        std::vector<ProcessInfo> result;
        std::vector<pid_t> stack;
        std::unordered_set<pid_t> seen;

        stack.push_back(root.pid);
        while (!stack.empty()) {
            pid_t pid = stack.back();
            stack.pop_back();

            if (!seen.insert(pid).second) {
                continue;
            }

            auto proc_it = processes.find(pid);
            if (proc_it == processes.end()) {
                continue;
            }
            result.push_back(proc_it->second);

            auto child_it = children.find(pid);
            if (child_it == children.end()) {
                continue;
            }
            for (auto it = child_it->second.rbegin(); it != child_it->second.rend(); ++it) {
                stack.push_back(*it);
            }
        }

        return result;
    }

    MarkAiResult
    mark_ai_subtree(const ProcessInfo &root,
                    const std::unordered_map<pid_t, ProcessInfo> &processes,
                    const std::unordered_map<pid_t, std::vector<pid_t>> &children) const {
        MarkAiResult result;
        for (const ProcessInfo &process : collect_subtree(root, processes, children)) {
            ++result.attempted;
            if (mark_ai_thread(process.pid, config_.proc_ai_thread)) {
                ++result.marked;
                if (process.pid == root.pid) {
                    result.root_marked = true;
                }
                continue;
            }

            int saved_errno = errno;
            if (!process_still_matches(process.pid, process.start_time)) {
                ++result.skipped_exited;
                if (process.pid == root.pid) {
                    result.root_exited = true;
                }
                continue;
            }

            ++result.failed;
            if (result.first_failed_pid == 0) {
                result.first_failed_pid = process.pid;
                result.first_errno = saved_errno;
            }
            if (process.pid == root.pid) {
                result.root_failed = true;
            }
        }

        return result;
    }

    bool manage_process(
        const ProcessInfo &process, const std::string &matched_name,
        const std::unordered_map<pid_t, ProcessInfo> &processes,
        const std::unordered_map<pid_t, std::vector<pid_t>> &children) {
        Counts before = counts();
        bool tied = false;
        Cluster desired = choose_route(before, tied);
        Cluster actual = desired;
        MarkAiResult mark_result;

        if (desired == Cluster::A100) {
            if (config_.dry_run) {
                mark_result.attempted = collect_subtree(process, processes, children).size();
                mark_result.marked = mark_result.attempted;
                mark_result.root_marked = true;
            } else {
                mark_result = mark_ai_subtree(process, processes, children);
            }

            if (!mark_result.root_marked) {
                if (mark_result.root_exited) {
                    return true;
                }

                if (!config_.quiet) {
                    std::fprintf(stderr,
                                 "randcore-child-balancer: failed to mark PID %ld (%s) subtree as A100 via %s: %s\n",
                                 static_cast<long>(process.pid), matched_name.c_str(),
                                 config_.proc_ai_thread.c_str(),
                                 std::strerror(mark_result.first_errno));
                }
                if (config_.strict) {
                    return false;
                }
                actual = Cluster::X100;
            } else if (mark_result.failed > 0) {
                if (!config_.quiet) {
                    std::fprintf(stderr,
                                 "randcore-child-balancer: failed to mark %zu PID(s) under PID %ld (%s) as A100 via %s; first failed pid=%ld: %s\n",
                                 mark_result.failed, static_cast<long>(process.pid),
                                 matched_name.c_str(), config_.proc_ai_thread.c_str(),
                                 static_cast<long>(mark_result.first_failed_pid),
                                 std::strerror(mark_result.first_errno));
                }
                if (config_.strict) {
                    return false;
                }
            }
        } else {
            Cluster detected = Cluster::X100;
            if (detect_current_cluster(process.pid, detected) && detected == Cluster::A100) {
                actual = Cluster::A100;
            }
        }

        managed_[process.pid] = ManagedProcess{process.pid, process.start_time, actual};

        if (config_.log) {
            const char *tie_text = tied ? " tie" : "";
            const char *dry_run_text = config_.dry_run ? " dry-run" : "";
            if (desired == actual) {
                std::fprintf(stderr,
                             "randcore-child-balancer:%s%s X100 count=%zu A100 count=%zu -> %s pid=%ld name=%s subtree=%zu\n",
                             tie_text, dry_run_text, before.x100, before.a100,
                             cluster_name(actual), static_cast<long>(process.pid),
                             matched_name.c_str(), mark_result.attempted);
            } else {
                std::fprintf(stderr,
                             "randcore-child-balancer:%s%s X100 count=%zu A100 count=%zu -> %s, actual %s pid=%ld name=%s subtree=%zu\n",
                             tie_text, dry_run_text, before.x100, before.a100,
                             cluster_name(desired), cluster_name(actual),
                             static_cast<long>(process.pid), matched_name.c_str(),
                             mark_result.attempted);
            }
        }

        return true;
    }

    Config config_;
    std::unordered_map<pid_t, ManagedProcess> managed_;
    Cluster next_tie_ = Cluster::A100;
};

void print_help() {
    std::puts(
        "Usage: randcore-child-balancer [--once] [--dry-run] [--help]\n"
        "\n"
        "Environment:\n"
        "  RANDCORE_PARENT_PIDS       Comma/space separated root PIDs to scan below.\n"
        "  RANDCORE_MATCH_NAMES       Comma/space separated fnmatch patterns.\n"
        "  RANDCORE_SCAN_INTERVAL_MS  Scan interval, default 100.\n"
        "  RANDCORE_SET_AI_THREAD     HMP proc path, default /proc/set_ai_thread.\n"
        "  RANDCORE_LOG=1             Print routing decisions.\n"
        "  RANDCORE_STRICT=1          Exit if A100 marking fails.\n"
        "  RANDCORE_QUIET=1           Suppress warnings.\n"
        "  RANDCORE_DRY_RUN=1         Do not write /proc/set_ai_thread.\n"
        "  RANDCORE_ONESHOT=1         Scan once and exit.");
}

Config load_config(int argc, char **argv) {
    Config config;

    config.parent_pids = parse_pid_list(getenv("RANDCORE_PARENT_PIDS"));

    const char *match_env = getenv("RANDCORE_MATCH_NAMES");
    config.match_patterns = split_list(is_empty(match_env) ? kDefaultMatchNames : match_env);
    if (config.match_patterns.empty()) {
        config.match_patterns = split_list(kDefaultMatchNames);
    }

    const char *proc_ai_thread_env = getenv("RANDCORE_SET_AI_THREAD");
    if (!is_empty(proc_ai_thread_env)) {
        config.proc_ai_thread = proc_ai_thread_env;
    }

    config.scan_interval_ms = parse_interval_ms(getenv("RANDCORE_SCAN_INTERVAL_MS"));
    config.strict = parse_bool_env("RANDCORE_STRICT");
    config.log = parse_bool_env("RANDCORE_LOG");
    config.quiet = parse_bool_env("RANDCORE_QUIET");
    config.dry_run = parse_bool_env("RANDCORE_DRY_RUN");
    config.once = parse_bool_env("RANDCORE_ONESHOT");

    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg == "--once") {
            config.once = true;
        } else if (arg == "--dry-run") {
            config.dry_run = true;
        } else if (arg == "--help" || arg == "-h") {
            print_help();
            std::exit(0);
        } else {
            std::fprintf(stderr, "randcore-child-balancer: unknown argument '%s'\n", argv[i]);
            std::exit(2);
        }
    }

    return config;
}

int real_main(int argc, char **argv) {
    Config config = load_config(argc, argv);

    struct sigaction action {};
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);

    ChildBalancer balancer(std::move(config));
    return balancer.run();
}

} // namespace

int main(int argc, char **argv) {
    try {
        return real_main(argc, argv);
    } catch (const std::bad_alloc &) {
        std::fprintf(stderr, "randcore-child-balancer: out of memory\n");
        return 1;
    }
}
