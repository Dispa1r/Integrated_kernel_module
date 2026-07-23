/* rfrida-zygisk: Zygisk module for stealth agent.so injection via custom ELF linker.
 * Uses Zygisk API v2, loads agent.so with custom ELF linker (anonymous mmap, no dlopen).
 */

#include "custom_loader.h"
#include "zygisk.hpp"
#include <android/log.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <jni.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#define TAG "rfrida-zygisk"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/* Default config - can be overridden by targets.txt in module directory */
static const char *kAgentName = "agent.so";
static int kDelayMs = 2000;
static std::vector<std::string> g_targets;

/* Read target list from module_dir/targets.txt (one package per line, # comments).
   Falls back to DEFAULT_TARGET if file not found. */
#define DEFAULT_TARGET "com.tencent.rmcn"

static void load_targets(int module_dir_fd) {
    g_targets.clear();

    int fd = openat(module_dir_fd, "targets.txt", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        /* No config file - use default */
        g_targets.push_back(DEFAULT_TARGET);
        LOGI("No targets.txt, using default: %s", DEFAULT_TARGET);
        return;
    }

    /* Read file */
    char buf[4096] = {0};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (n <= 0) {
        g_targets.push_back(DEFAULT_TARGET);
        return;
    }

    /* Parse: one package per line, skip # comments and empty lines */
    std::string content(buf, n);
    size_t pos = 0;
    while (pos < content.size()) {
        size_t nl = content.find('\n', pos);
        if (nl == std::string::npos) nl = content.size();
        std::string line = content.substr(pos, nl - pos);
        pos = nl + 1;
        /* Trim */
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        while (!line.empty() && (line.front() == ' '))
            line.erase(0, 1);
        if (line.empty() || line[0] == '#') continue;
        g_targets.push_back(line);
        LOGI("Target: %s", line.c_str());
    }

    if (g_targets.empty()) {
        g_targets.push_back(DEFAULT_TARGET);
    }
    LOGI("Loaded %zu targets", g_targets.size());
}

static bool is_target(const std::string &pkg) {
    for (auto &t : g_targets) {
        if (pkg == t) return true;
    }
    return false;
}


/* Global state set in preAppSpecialize, used in postAppSpecialize */
static int g_agent_fd = -1;
static bool g_enabled = false;
static std::string g_entry_name = "hello_entry";

/* ================================================================
   Zygisk Module (API v2)
   ================================================================ */
class RfridaModule : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env;
        LOGI("onLoad pid=%d", getpid());
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        const char *nice_name = env->GetStringUTFChars(args->nice_name, nullptr);
        const char *app_data_dir = env->GetStringUTFChars(args->app_data_dir, nullptr);
        LOGI("preAppSpecialize: nice_name=%s", nice_name ? nice_name : "(null)");

        g_enabled = false;
        if (nice_name && nice_name[0]) {
            /* Load target list from module directory on first call */
            if (g_targets.empty()) {
                int dirfd = api->getModuleDir();
                if (dirfd >= 0) load_targets(dirfd);
            }
            bool target = is_target(nice_name);
            LOGI("Target check: %s -> %d", nice_name, target);
            if (target) {
                g_enabled = true;
                int dirfd = api->getModuleDir();
                LOGI("Module dir fd: %d", dirfd);
                /* Load targets on first use */
                if (g_targets.empty()) load_targets(dirfd);
                if (dirfd >= 0) {
                    g_agent_fd = openat(dirfd, kAgentName, O_RDONLY | O_CLOEXEC);
                    if (g_agent_fd >= 0) {
                        LOGI("Agent fd opened: %d", g_agent_fd);
                    } else {
                        LOGE("openat agent.so failed: %s", strerror(errno));
                        g_enabled = false;
                    }
                } else {
                    LOGE("getModuleDir failed");
                    g_enabled = false;
                }
            }
        }

        if (!g_enabled) {
            api->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
        }

        if (nice_name) env->ReleaseStringUTFChars(args->nice_name, nice_name);
        if (app_data_dir) env->ReleaseStringUTFChars(args->app_data_dir, app_data_dir);
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *args) override {
        if (!g_enabled || g_agent_fd < 0) return;

        int fd = g_agent_fd;
        g_agent_fd = -1;

        /* Create socketpair + AgentArgs for hello_entry.
           hello_entry expects a VALID socket fd - passing -1 causes
           .try_clone().expect() panic in Rust agent. */
        std::thread([fd]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(kDelayMs));

            void *handle = custom_dlopen_fd(fd);
            if (!handle) { LOGE("load failed"); close(fd); return; }
            LOGI("Agent loaded at %p", handle);

            typedef void *(*entry_fn)(void *);
            auto entry = (entry_fn)custom_dlsym(handle, "hello_entry");
            if (!entry) { LOGE("hello_entry not found"); close(fd); return; }

            /* Create socketpair: fds[1] goes to agent, fds[0] stored
               for future host connection via abstract socket. */
            int socks[2];
            if (socketpair(AF_UNIX, SOCK_STREAM, 0, socks) < 0) {
                LOGE("socketpair failed: %s", strerror(errno));
                close(fd); return;
            }

            struct AgentArgs {
                uint64_t table;
                int32_t ctrl_fd;
                int32_t agent_memfd;
            };
            static uint8_t g_string_table[256] = {0};
            memcpy(g_string_table, "output_path=/dev/null", 21);

            AgentArgs args;
            args.table = (uint64_t)(uintptr_t)g_string_table;
            args.ctrl_fd = socks[1];       /* agent's end */
            args.agent_memfd = -1;

            LOGI("Calling hello_entry (fd=%d)", args.ctrl_fd);
            /* hello_entry blocks waiting for host commands.
               It never returns until the socket is closed. */
            entry(&args);
            LOGI("hello_entry returned (agent shutdown)");

            close(socks[0]);
            close(fd);
        }).detach();

        LOGI("Injection thread spawned");
    }

    void preServerSpecialize(zygisk::ServerSpecializeArgs *args) override {
        LOGI("preServerSpecialize");
    }

    void postServerSpecialize(const zygisk::ServerSpecializeArgs *args) override {
        LOGI("postServerSpecialize");
    }

private:
    zygisk::Api *api = nullptr;
    JNIEnv *env = nullptr;
};

REGISTER_ZYGISK_MODULE(RfridaModule)