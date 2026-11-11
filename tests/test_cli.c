#include "skbd/runtime.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static int fails;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", what);
        fails++;
    }
}

static void wipe(const char *dir) {
    char path[256];

    snprintf(path, sizeof path, "%s/file", dir);
    unlink(path);
    snprintf(path, sizeof path, "%s/sock", dir);
    unlink(path);
    snprintf(path, sizeof path, "%s/skbd.lock", dir);
    unlink(path);
    snprintf(path, sizeof path, "%s/extra.lock", dir);
    unlink(path);
    rmdir(dir);
}

static int scratch(char *dir, size_t cap, const char *name) {
    int wrote;

    wrote = snprintf(dir, cap, "/tmp/skbd-%s-%ld", name, (long)getpid());
    if (wrote < 0 || (size_t)wrote >= cap) {
        return -1;
    }
    wipe(dir);
    if (mkdir(dir, 0700) != 0) {
        return -1;
    }
    if (chmod(dir, 0700) != 0) {
        return -1;
    }
    return 0;
}

/* port: help_lists_the_flags */
static void test_help_lists_the_flags(void) {
    const char *flags[] = {"--help", "-h", "--version", "--verbose", "--config", "--default"};
    size_t i;
    SkbdParsed parsed;
    char err[128];
    char *help_argv[] = {"skbd", "-h"};
    char *long_argv[] = {"skbd", "--help"};
    char *version_argv[] = {"skbd", "--version"};

    for (i = 0; i < sizeof flags / sizeof flags[0]; i++) {
        check(strstr(skbd_help, flags[i]) != NULL, flags[i]);
    }
    err[0] = '\0';
    check(skbd_parse_args(2, help_argv, &parsed, err, sizeof err) == 0, "parse -h");
    check(parsed.kind == SKBD_PARSED_HELP, "kind -h");
    skbd_parsed_free(&parsed);
    check(skbd_parse_args(2, long_argv, &parsed, err, sizeof err) == 0, "parse --help");
    check(parsed.kind == SKBD_PARSED_HELP, "kind --help");
    skbd_parsed_free(&parsed);
    check(skbd_parse_args(2, version_argv, &parsed, err, sizeof err) == 0, "parse --version");
    check(parsed.kind == SKBD_PARSED_VERSION, "kind --version");
    skbd_parsed_free(&parsed);
}

/* port: unknown_flag_and_missing_value_are_errors */
static void test_unknown_flag_and_missing_value_are_errors(void) {
    SkbdParsed parsed;
    char err[128];
    char *unknown[] = {"skbd", "--nope"};
    char *config[] = {"skbd", "--config"};
    char *fallback[] = {"skbd", "--default"};

    err[0] = '\0';
    check(skbd_parse_args(2, unknown, &parsed, err, sizeof err) != 0, "unknown fails");
    check(strstr(err, "unknown argument: --nope") != NULL, err);
    err[0] = '\0';
    check(skbd_parse_args(2, config, &parsed, err, sizeof err) != 0, "config fails");
    check(strstr(err, "--config needs a value") != NULL, err);
    err[0] = '\0';
    check(skbd_parse_args(2, fallback, &parsed, err, sizeof err) != 0, "default fails");
    check(strstr(err, "--default needs a value") != NULL, err);
}

/* port: config_and_default_flags_are_kept */
static void test_config_and_default_flags_are_kept(void) {
    SkbdParsed parsed;
    char err[128];
    char *argv[] = {"skbd", "--verbose", "--config", "dir/config.toml", "--default", "ua"};

    err[0] = '\0';
    check(skbd_parse_args(6, argv, &parsed, err, sizeof err) == 0, err);
    check(parsed.kind == SKBD_PARSED_RUN, "run");
    check(parsed.options.verbose, "verbose");
    check(parsed.options.config_path != NULL && strcmp(parsed.options.config_path, "dir/config.toml") == 0, "config");
    check(parsed.options.default_layout != NULL && strcmp(parsed.options.default_layout, "ua") == 0, "default");
    skbd_parsed_free(&parsed);
}

/* port: empty_swaysock_is_an_error_at_the_cli_and_idle_under_systemd */
static void test_empty_swaysock_is_an_error_at_the_cli_and_idle_under_systemd(void) {
    SkbdStopKind stop;
    char path[128];
    char err[128];

    path[0] = '\0';
    err[0] = '\0';
    stop = SKBD_STOP_OK;
    check(skbd_classify_swaysock(SKBD_SOCK_EMPTY, NULL, 0, path, sizeof path, &stop, err, sizeof err) != 0, "empty cli");
    check(stop == SKBD_STOP_FAIL, "empty cli stop");
    check(strcmp(err, "SWAYSOCK is empty") == 0, err);
    err[0] = '\0';
    check(skbd_classify_swaysock(SKBD_SOCK_EMPTY, NULL, 1, path, sizeof path, &stop, err, sizeof err) != 0, "empty service");
    check(stop == SKBD_STOP_IDLE, "empty idle");
    check(strcmp(err, "SWAYSOCK is empty") == 0, err);
    err[0] = '\0';
    check(skbd_classify_swaysock(SKBD_SOCK_MISSING, NULL, 0, path, sizeof path, &stop, err, sizeof err) != 0, "missing cli");
    check(stop == SKBD_STOP_FAIL, "missing cli stop");
    check(strcmp(err, "SWAYSOCK is not set") == 0, err);
    err[0] = '\0';
    check(skbd_classify_swaysock(SKBD_SOCK_MISSING, NULL, 1, path, sizeof path, &stop, err, sizeof err) != 0, "missing service");
    check(stop == SKBD_STOP_IDLE, "missing idle");
    check(strcmp(err, "SWAYSOCK is not set") == 0, err);
    err[0] = '\0';
    check(skbd_classify_swaysock(SKBD_SOCK_NOT_UTF8, NULL, 1, path, sizeof path, &stop, err, sizeof err) != 0, "utf8");
    check(stop == SKBD_STOP_FAIL, "utf8 stop");
    check(strcmp(err, "SWAYSOCK is not valid UTF-8") == 0, err);
    err[0] = '\0';
    check(skbd_classify_swaysock(SKBD_SOCK_PATH, "/run/user/1000/sway-ipc.sock", 0, path, sizeof path, &stop, err, sizeof err) == 0, err);
    check(strcmp(path, "/run/user/1000/sway-ipc.sock") == 0, path);
}

/* port: socket_path_must_exist_and_be_a_socket */
static void test_socket_path_must_exist_and_be_a_socket(void) {
    char dir[128];
    char missing[160];
    char file[160];
    char sock[160];
    char err[256];
    FILE *handle;
    int fd;
    struct sockaddr_un addr;

    if (scratch(dir, sizeof dir, "socket") != 0) {
        check(0, "scratch socket");
        return;
    }
    snprintf(missing, sizeof missing, "%s/missing", dir);
    snprintf(file, sizeof file, "%s/file", dir);
    snprintf(sock, sizeof sock, "%s/sock", dir);
    err[0] = '\0';
    check(skbd_ensure_socket(missing, err, sizeof err) != 0, "missing fails");
    check(strstr(err, "does not exist") != NULL, err);
    handle = fopen(file, "w");
    check(handle != NULL, "write file");
    if (handle != NULL) {
        fputs("nope", handle);
        fclose(handle);
    }
    err[0] = '\0';
    check(skbd_ensure_socket(file, err, sizeof err) != 0, "file fails");
    check(strstr(err, "not a socket") != NULL, err);
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    check(fd >= 0, "socket");
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    check(strlen(sock) < sizeof addr.sun_path, "sock path fits");
    if (strlen(sock) < sizeof addr.sun_path) {
        memcpy(addr.sun_path, sock, strlen(sock) + 1);
        check(bind(fd, (struct sockaddr *)&addr, sizeof addr) == 0, "bind");
    }
    err[0] = '\0';
    check(skbd_ensure_socket(sock, err, sizeof err) == 0, err);
    check(chmod(sock, 0775) == 0, "chmod sock");
    err[0] = '\0';
    check(skbd_ensure_socket(sock, err, sizeof err) == 0, err);
    check(chmod(dir, 0755) == 0, "chmod parent");
    err[0] = '\0';
    check(skbd_ensure_socket(sock, err, sizeof err) != 0, "parent mode");
    check(strstr(err, "0700") != NULL, err);
    if (fd >= 0) {
        close(fd);
    }
    wipe(dir);
}

/* port: root_is_refused */
static void test_root_is_refused(void) {
    char err[160];

    err[0] = '\0';
    check(skbd_refuse_root(0, err, sizeof err) != 0, "root");
    check(strcmp(err, "refusing to run as root. The unit is a systemd user service") == 0, err);
    check(skbd_refuse_root(1000, err, sizeof err) == 0, "user");
}

/* port: runtime_dir_must_be_set_absolute_private_and_owned */
static void test_runtime_dir_must_be_set_absolute_private_and_owned(void) {
    char err[256];
    char dir[128];
    uid_t mine;

    err[0] = '\0';
    check(skbd_require_runtime_dir(NULL, err, sizeof err) != 0, "unset");
    check(strcmp(err, "XDG_RUNTIME_DIR is unset") == 0, err);
    err[0] = '\0';
    check(skbd_require_runtime_dir("", err, sizeof err) != 0, "empty");
    check(strcmp(err, "XDG_RUNTIME_DIR is empty") == 0, err);
    err[0] = '\0';
    check(skbd_require_runtime_dir("relative", err, sizeof err) != 0, "relative");
    check(strcmp(err, "XDG_RUNTIME_DIR is not absolute") == 0, err);
    if (scratch(dir, sizeof dir, "runtime") != 0) {
        check(0, "scratch runtime");
        return;
    }
    mine = geteuid();
    err[0] = '\0';
    check(skbd_check_runtime_dir(dir, mine, err, sizeof err) == 0, err);
    err[0] = '\0';
    check(skbd_check_runtime_dir(dir, (uid_t)(mine + 1), err, sizeof err) != 0, "owner");
    check(strstr(err, "not owned") != NULL, err);
    check(chmod(dir, 0755) == 0, "chmod");
    err[0] = '\0';
    check(skbd_check_runtime_dir(dir, mine, err, sizeof err) != 0, "mode");
    check(strstr(err, "0700") != NULL, err);
    wipe(dir);
}

/* port: second_lock_reports_already_running */
static void test_second_lock_reports_already_running(void) {
    char dir[128];
    char err[256];
    SkbdLock first;
    SkbdLock second;

    first.fd = -1;
    second.fd = -1;
    if (scratch(dir, sizeof dir, "lock") != 0) {
        check(0, "scratch lock");
        return;
    }
    err[0] = '\0';
    check(skbd_lock_runtime_dir(dir, &first, err, sizeof err) == 0, err);
    err[0] = '\0';
    check(skbd_lock_runtime_dir(dir, &second, err, sizeof err) != 0, "second");
    check(strcmp(err, "skbd: already running") == 0, err);
    check(second.fd < 0, "second fd");
    skbd_lock_close(&first);
    err[0] = '\0';
    check(skbd_lock_runtime_dir(dir, &second, err, sizeof err) == 0, err);
    skbd_lock_close(&second);
    wipe(dir);
}

/* port: lock_symlink_is_refused */
static void test_lock_symlink_is_refused(void) {
    char dir[128];
    char path[160];
    char err[256];
    SkbdLock lock;

    lock.fd = -1;
    if (scratch(dir, sizeof dir, "locklink") != 0) {
        check(0, "scratch locklink");
        return;
    }
    snprintf(path, sizeof path, "%s/skbd.lock", dir);
    check(symlink("elsewhere", path) == 0, "symlink");
    err[0] = '\0';
    check(skbd_lock_runtime_dir(dir, &lock, err, sizeof err) != 0, "symlink lock");
    check(strstr(err, "cannot open") != NULL, err);
    check(lock.fd < 0, "lock fd");
    skbd_lock_close(&lock);
    wipe(dir);
}

/* Rust lock_file_is_ours accepts mode 0644 and nlink greater than 1. */
static void test_lock_accepts_shared_mode_and_extra_link(void) {
    char dir[128];
    char path[160];
    char extra[160];
    char err[256];
    SkbdLock lock;
    int fd;
    int wrote;

    lock.fd = -1;
    if (scratch(dir, sizeof dir, "lockmode") != 0) {
        check(0, "scratch lockmode");
        return;
    }
    wrote = snprintf(path, sizeof path, "%s/skbd.lock", dir);
    check(wrote > 0 && (size_t)wrote < sizeof path, "lock path");
    wrote = snprintf(extra, sizeof extra, "%s/extra.lock", dir);
    check(wrote > 0 && (size_t)wrote < sizeof extra, "extra path");
    fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0644);
    check(fd >= 0, "create lock");
    if (fd >= 0) {
        close(fd);
    }
    check(chmod(path, 0644) == 0, "chmod lock");
    check(link(path, extra) == 0, "link lock");
    err[0] = '\0';
    check(skbd_lock_runtime_dir(dir, &lock, err, sizeof err) == 0, err);
    skbd_lock_close(&lock);
    unlink(extra);
    wipe(dir);
}

int main(void) {
    test_help_lists_the_flags();
    test_unknown_flag_and_missing_value_are_errors();
    test_config_and_default_flags_are_kept();
    test_empty_swaysock_is_an_error_at_the_cli_and_idle_under_systemd();
    test_socket_path_must_exist_and_be_a_socket();
    test_root_is_refused();
    test_runtime_dir_must_be_set_absolute_private_and_owned();
    test_second_lock_reports_already_running();
    test_lock_symlink_is_refused();
    test_lock_accepts_shared_mode_and_extra_link();
    return fails == 0 ? 0 : 1;
}
