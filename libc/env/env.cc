/*
 * OSv-owned environment management (LLVM_LIBC_PLAN.md, Phase 4).
 * Only built with conf_llvm_libc=1; the musl env/ objects own these
 * symbols otherwise (getenv on baremetal is one of the two entrypoints
 * llvm-libc cannot build - it is OS-coupled by nature).
 *
 * Same model as musl: __environ (libc/env/__environ.c) points at a
 * NULL-terminated array of "NAME=value" strings. Strings passed to
 * putenv are owned by the caller; strings created by setenv are
 * malloc'd and leaked on replacement - the kernel sets a handful of
 * variables once at boot (loader.cc), so bookkeeping like musl's
 * __env_rm_add would be dead weight.
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <osv/export.h>

extern "C" char **__environ;

static pthread_mutex_t env_lock = PTHREAD_MUTEX_INITIALIZER;

static char **env_find(const char *name, size_t namelen)
{
    for (char **e = __environ; *e; e++) {
        if (!strncmp(*e, name, namelen) && (*e)[namelen] == '=') {
            return e;
        }
    }
    return nullptr;
}

extern "C" {

OSV_LIBC_API
char *getenv(const char *name)
{
    if (!name || !*name) {
        return nullptr;
    }
    pthread_mutex_lock(&env_lock);
    char **e = env_find(name, strlen(name));
    char *val = e ? *e + strlen(name) + 1 : nullptr;
    pthread_mutex_unlock(&env_lock);
    return val;
}

static int env_insert(char *str, size_t namelen, int overwrite)
{
    pthread_mutex_lock(&env_lock);
    char **e = env_find(str, namelen);
    if (e) {
        if (overwrite) {
            *e = str;
        }
        pthread_mutex_unlock(&env_lock);
        return 0;
    }
    size_t count = 0;
    while (__environ[count]) {
        count++;
    }
    // The initial __environ points at a static empty array; only
    // arrays we allocated here may be realloc'd, so always copy.
    char **bigger = (char **)malloc((count + 2) * sizeof(char *));
    if (!bigger) {
        pthread_mutex_unlock(&env_lock);
        return -1;
    }
    memcpy(bigger, __environ, count * sizeof(char *));
    bigger[count] = str;
    bigger[count + 1] = nullptr;
    static char **owned_environ;
    free(owned_environ);
    owned_environ = bigger;
    __environ = bigger;
    pthread_mutex_unlock(&env_lock);
    return 0;
}

OSV_LIBC_API
int setenv(const char *name, const char *value, int overwrite)
{
    if (!name || !*name || strchr(name, '=')) {
        errno = EINVAL;
        return -1;
    }
    size_t namelen = strlen(name);
    // peek without insertion to honor overwrite=0 cheaply
    pthread_mutex_lock(&env_lock);
    bool exists = env_find(name, namelen) != nullptr;
    pthread_mutex_unlock(&env_lock);
    if (exists && !overwrite) {
        return 0;
    }
    char *str = (char *)malloc(namelen + strlen(value) + 2);
    if (!str) {
        errno = ENOMEM;
        return -1;
    }
    strcpy(str, name);
    str[namelen] = '=';
    strcpy(str + namelen + 1, value);
    if (env_insert(str, namelen, 1) < 0) {
        free(str);
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

OSV_LIBC_API
int putenv(char *str)
{
    char *eq = strchr(str, '=');
    if (!eq || eq == str) {
        errno = EINVAL;
        return -1;
    }
    if (env_insert(str, eq - str, 1) < 0) {
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

OSV_LIBC_API
int unsetenv(const char *name)
{
    if (!name || !*name || strchr(name, '=')) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&env_lock);
    char **e = env_find(name, strlen(name));
    if (e) {
        // shift the tail down over the removed slot
        do {
            e[0] = e[1];
        } while (*e++);
    }
    pthread_mutex_unlock(&env_lock);
    return 0;
}

} // extern "C"
