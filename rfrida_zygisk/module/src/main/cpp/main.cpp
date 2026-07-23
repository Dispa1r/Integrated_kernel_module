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
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#define TAG "rfrida-zygisk"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/* Target configuration */
static const char *kAgentName = "agent.so";  /* in module directory */
static const char *kEntryName = "hello_entry";
static int kDelayMs = 2000;

/* Target packages that should be injected */
static const char *kTargetPackages[] = {
    "com.android.systemui",
    nullptr
};

static bool is_target(const std::string &pkg) {
    for (int i = 0; kTargetPackages[i]; i++) {
        if (pkg == kTargetPackages[i]) return true;
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
            bool target = is_target(nice_name);
            LOGI("Target check: %s -> %d", nice_name, target);
            if (target) {
                g_enabled = true;
                int dirfd = api->getModuleDir();
                LOGI("Module dir fd: %d", dirfd);
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

        LOGI("postAppSpecialize: injecting via fd %d", g_agent_fd);

        if (kDelayMs > 0) {
            LOGI("Sleeping %dms", kDelayMs);
            std::this_thread::sleep_for(std::chrono::milliseconds(kDelayMs));
        }

        /* Load agent via custom linker using proc/self/fd */
        void *handle = custom_dlopen_fd(g_agent_fd);
        if (!handle) {
            LOGE("custom_dlopen_fd failed");
            return;
        }
        LOGI("Agent loaded at %p", handle);

        typedef void (*entry_fn)(void *);
        auto entry = (entry_fn)custom_dlsym(handle, kEntryName);
        if (!entry) entry = (entry_fn)custom_dlsym(handle, "JNI_OnLoad");
        if (entry) {
            LOGI("Calling entry point");
            entry(nullptr);
            LOGI("Injection complete!");
        } else {
            LOGE("entry point not found");
        }
        close(g_agent_fd);
        g_agent_fd = -1;
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