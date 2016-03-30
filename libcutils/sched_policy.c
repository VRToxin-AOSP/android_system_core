/*
** Copyright 2007, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#define LOG_TAG "SchedPolicy"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <cutils/sched_policy.h>
#include <log/log.h>

#define UNUSED __attribute__((__unused__))

/* Re-map SP_DEFAULT to the system default policy, and leave other values unchanged.
 * Call this any place a SchedPolicy is used as an input parameter.
 * Returns the possibly re-mapped policy.
 */
static inline SchedPolicy _policy(SchedPolicy p)
{
   return p == SP_DEFAULT ? SP_SYSTEM_DEFAULT : p;
}

#if defined(__ANDROID__)

#include <pthread.h>
#include <sched.h>
#include <sys/prctl.h>

#define POLICY_DEBUG 0

// This prctl is only available in Android kernels.
#define PR_SET_TIMERSLACK_PID 41

// timer slack value in nS enforced when the thread moves to background
#define TIMER_SLACK_BG 40000000
#define TIMER_SLACK_FG 50000

static pthread_once_t the_once = PTHREAD_ONCE_INIT;
static pthread_once_t sched_once = PTHREAD_ONCE_INIT;
static pthread_once_t cpuset_once = PTHREAD_ONCE_INIT;

static int __sys_supports_schedgroups = -1;
static int __sys_supports_cpusets = -1;
static char proc_name[255] = {0};

// File descriptors open to /dev/cpuctl/../tasks, setup by initialize, or -1 on error.
static int bg_cgroup_fd = -1;
static int fg_cgroup_fd = -1;

// File descriptors open to /dev/cpuset/../tasks, setup by initialize, or -1 on error
static int system_bg_cpuset_fd = -1;
static int bg_cpuset_fd = -1;
static int fg_cpuset_fd = -1;

/* Add tid to the scheduling group defined by the policy */
static int add_tid_to_cgroup(int tid, int fd)
{
    if (fd < 0) {
        SLOGE("%s add_tid_to_cgroup failed; fd=%d\n", proc_name, fd);
        errno = EINVAL;
        return -1;
    }

    // specialized itoa -- works for tid > 0
    char text[22];
    char *end = text + sizeof(text) - 1;
    char *ptr = end;
    *ptr = '\0';
    while (tid > 0) {
        *--ptr = '0' + (tid % 10);
        tid = tid / 10;
    }

    if (write(fd, ptr, end - ptr) < 0) {
        /*
         * If the thread is in the process of exiting,
         * don't flag an error
         */
        if (errno == ESRCH)
                return 0;
        SLOGW("%s add_tid_to_cgroup failed to write '%s' (%s); fd=%d\n",
              proc_name, ptr, strerror(errno), fd);
        errno = EINVAL;
        return -1;
    }

    return 0;
}

static void __initialize(void) {
    int pfd;
    int ptid = gettid();

    sprintf(proc_name, "/proc/%d/cmdline", ptid);

    pfd = open(proc_name, O_RDONLY);
    memset(proc_name, 0, sizeof(proc_name));
    if (pfd > 0) {
        read(pfd, proc_name, 254);
        close(pfd);
    }
}

static void __init_sched(void) {
    char* filename;

    pthread_once(&the_once, __initialize);

    if (!access("/dev/cpuctl/tasks", F_OK)) {
        __sys_supports_schedgroups = 1;

        filename = "/dev/cpuctl/tasks";
        fg_cgroup_fd = open(filename, O_WRONLY | O_CLOEXEC);
        if (fg_cgroup_fd < 0) {
            SLOGE("%s open of %s failed: %s\n", proc_name, filename, strerror(errno));
            __sys_supports_schedgroups = 0;
        }

        filename = "/dev/cpuctl/bg_non_interactive/tasks";
        bg_cgroup_fd = open(filename, O_WRONLY | O_CLOEXEC);
        if (bg_cgroup_fd < 0) {
            SLOGE("%s open of %s failed: %s\n", proc_name, filename, strerror(errno));
            __sys_supports_schedgroups = 0;
        }

        if (!__sys_supports_schedgroups) {
            close(bg_cgroup_fd);
            bg_cgroup_fd = -1;

            close(fg_cgroup_fd);
            fg_cgroup_fd = -1;
        }
    } else {
        __sys_supports_schedgroups = 0;
    }
}

static void __init_cpuset(void) {
    char *filename;

    pthread_once(&the_once, __initialize);

#ifdef USE_CPUSETS
    if (!access("/dev/cpuset/tasks", F_OK)) {
        __sys_supports_cpusets = 1;

        filename = "/dev/cpuset/foreground/tasks";
        fg_cpuset_fd = open(filename, O_WRONLY | O_CLOEXEC);
        if (fg_cpuset_fd < 0) {
            SLOGE("%s open of %s failed %s\n", proc_name, filename, strerror(errno));
            __sys_supports_cpusets = 0;
        }

        filename = "/dev/cpuset/background/tasks";
        bg_cpuset_fd = open(filename, O_WRONLY | O_CLOEXEC);
        if (bg_cpuset_fd < 0) {
            SLOGE("%s open of %s failed %s\n", proc_name, filename, strerror(errno));
            __sys_supports_cpusets = 0;
        }

        filename = "/dev/cpuset/system-background/tasks";
        system_bg_cpuset_fd = open(filename, O_WRONLY | O_CLOEXEC);
        if (system_bg_cpuset_fd < 0) {
            SLOGE("%s open of %s failed %s\n", proc_name, filename, strerror(errno));
            __sys_supports_cpusets = 0;
        }

        if (!__sys_supports_cpusets) {
            close(fg_cpuset_fd);
            fg_cpuset_fd = -1;

            close(bg_cpuset_fd);
            bg_cpuset_fd = -1;

            close(system_bg_cpuset_fd);
            system_bg_cpuset_fd = -1;
        }
    } else {
        __sys_supports_cpusets = 0;
    }
#endif

}

/*
 * Try to get the scheduler group.
 *
 * The data from /proc/<pid>/cgroup looks (something) like:
 *  2:cpu:/bg_non_interactive
 *  1:cpuacct:/
 *
 * We return the part after the "/", which will be an empty string for
 * the default cgroup.  If the string is longer than "bufLen", the string
 * will be truncated.
 */
static int getSchedulerGroup(int tid, char* buf, size_t bufLen)
{
#if defined(__ANDROID__)
    char pathBuf[32];
    char lineBuf[256];
    FILE *fp;

    snprintf(pathBuf, sizeof(pathBuf), "/proc/%d/cgroup", tid);
    if (!(fp = fopen(pathBuf, "r"))) {
        return -1;
    }

    while(fgets(lineBuf, sizeof(lineBuf) -1, fp)) {
        char *next = lineBuf;
        char *subsys;
        char *grp;
        size_t len;

        /* Junk the first field */
        if (!strsep(&next, ":")) {
            goto out_bad_data;
        }

        if (!(subsys = strsep(&next, ":"))) {
            goto out_bad_data;
        }

        if (strcmp(subsys, "cpu")) {
            /* Not the subsys we're looking for */
            continue;
        }

        if (!(grp = strsep(&next, ":"))) {
            goto out_bad_data;
        }
        grp++; /* Drop the leading '/' */
        len = strlen(grp);
        grp[len-1] = '\0'; /* Drop the trailing '\n' */

        if (bufLen <= len) {
            len = bufLen - 1;
        }
        strncpy(buf, grp, len);
        buf[len] = '\0';
        fclose(fp);
        return 0;
    }

    SLOGE("%s Failed to find cpu subsys", proc_name);
    fclose(fp);
    return -1;
 out_bad_data:
    SLOGE("%s Bad cgroup data {%s}", proc_name, lineBuf);
    fclose(fp);
    return -1;
#else
    errno = ENOSYS;
    return -1;
#endif
}

int get_sched_policy(int tid, SchedPolicy *policy)
{
    if (tid == 0) {
        tid = gettid();
    }

    pthread_once(&sched_once, __init_sched);

    if (__sys_supports_schedgroups) {
        char grpBuf[32];
        if (getSchedulerGroup(tid, grpBuf, sizeof(grpBuf)) < 0)
            return -1;
        if (grpBuf[0] == '\0') {
            *policy = SP_FOREGROUND;
        } else if (!strcmp(grpBuf, "bg_non_interactive")) {
            *policy = SP_BACKGROUND;
        } else {
            errno = ERANGE;
            return -1;
        }
    } else {
        int rc = sched_getscheduler(tid);
        if (rc < 0)
            return -1;
        else if (rc == SCHED_NORMAL)
            *policy = SP_FOREGROUND;
        else if (rc == SCHED_BATCH)
            *policy = SP_BACKGROUND;
/* BEGIN Motorola, rknize2, 05/10/2013, IKJBXLINE-9555 */
        else if (rc == SCHED_RR)
            *policy = SP_REALTIME;
/* END Motorola, IKJBXLINE-9555 */
        else {
            errno = ERANGE;
            return -1;
        }
    }
    return 0;
}

int set_cpuset_policy(int tid, SchedPolicy policy)
{
    // in the absence of cpusets, use the old sched policy
#ifndef USE_CPUSETS
    return set_sched_policy(tid, policy);
#else
    if (tid == 0) {
        tid = gettid();
    }

    pthread_once(&cpuset_once, __init_cpuset);

    if (!__sys_supports_cpusets)
        return set_sched_policy(tid, policy);

    policy = _policy(policy);

    int fd;
    switch (policy) {
    case SP_BACKGROUND:
        fd = bg_cpuset_fd;
        break;
    case SP_FOREGROUND:
    case SP_AUDIO_APP:
    case SP_AUDIO_SYS:
        fd = fg_cpuset_fd;
        break;
    case SP_SYSTEM:
        fd = system_bg_cpuset_fd;
        break;
    default:
        fd = -1;
        break;
    }

    if (add_tid_to_cgroup(tid, fd) != 0) {
        if (errno != ESRCH && errno != ENOENT)
            return -errno;
    }

    return 0;
#endif
}

int set_sched_policy(int tid, SchedPolicy policy)
{
    if (tid == 0) {
        tid = gettid();
    }
    policy = _policy(policy);

    pthread_once(&sched_once, __init_sched);

#if POLICY_DEBUG
    char statfile[64];
    char statline[1024];
    char thread_name[255];
    int fd;

    sprintf(statfile, "/proc/%d/stat", tid);
    memset(thread_name, 0, sizeof(thread_name));

    fd = open(statfile, O_RDONLY);
    if (fd >= 0) {
        int rc = read(fd, statline, 1023);
        close(fd);
        statline[rc] = 0;
        char *p = statline;
        char *q;

        for (p = statline; *p != '('; p++);
        p++;
        for (q = p; *q != ')'; q++);

        strncpy(thread_name, p, (q-p));
    }
    switch (policy) {
    case SP_BACKGROUND:
        SLOGD("vvv tid %d (%s)", tid, thread_name);
        break;
    case SP_FOREGROUND:
    case SP_AUDIO_APP:
    case SP_AUDIO_SYS:
        SLOGD("^^^ tid %d (%s)", tid, thread_name);
        break;
    case SP_SYSTEM:
        SLOGD("/// tid %d (%s)", tid, thread_name);
        break;
/* BEGIN Motorola, rknize2, 05/10/2013, IKJBXLINE-9555 */
    case SP_REALTIME:
        SLOGD("!!! tid %d (%s)", tid, thread_name);
        break;
/* END Motorola, IKJBXLINE-9555 */
    default:
        SLOGD("??? tid %d (%s)", tid, thread_name);
        break;
    }
#endif

/* BEGIN Motorola, rknize2, 05/10/2013, IKJBXLINE-9555
 * Schedule groups are not supported for RT processes. */
    if (__sys_supports_schedgroups &&
        policy != SP_REALTIME) {
/* END Motorola, IKJBXLINE-9555 */
        int fd;
        switch (policy) {
        case SP_BACKGROUND:
            fd = bg_cgroup_fd;
            break;
        case SP_FOREGROUND:
        case SP_AUDIO_APP:
        case SP_AUDIO_SYS:
            fd = fg_cgroup_fd;
            break;
        default:
            fd = -1;
            break;
        }


        if (add_tid_to_cgroup(tid, fd) != 0) {
            if (errno != ESRCH && errno != ENOENT)
                return -errno;
        }
    } else {
        struct sched_param param;
/* BEGIN Motorola, rknize2, 05/10/2013, IKJBXLINE-9555
 * Allow the RT policy at the lowest priority. */
        int posix_policy = SCHED_NORMAL;

        param.sched_priority = 0; /* unused for non-RT policies */
        if (policy == SP_BACKGROUND) {
            posix_policy = SCHED_BATCH;
        } else if (policy == SP_REALTIME) {
            posix_policy = SCHED_RR;
            param.sched_priority = 1; /* lowest RT priority */
        }

        if (sched_setscheduler(tid, posix_policy, &param) < 0)
            SLOGE("sched_setscheduler failed: tid %d, errno=%d", tid, errno);
/* END Motorola, IKJBXLINE-9555 */
    }

    prctl(PR_SET_TIMERSLACK_PID,
          policy == SP_BACKGROUND ? TIMER_SLACK_BG : TIMER_SLACK_FG, tid);

    return 0;
}

#else

/* Stubs for non-Android targets. */

int set_sched_policy(int tid UNUSED, SchedPolicy policy UNUSED)
{
    return 0;
}

int get_sched_policy(int tid UNUSED, SchedPolicy *policy)
{
    *policy = SP_SYSTEM_DEFAULT;
    return 0;
}

#endif

const char *get_sched_policy_name(SchedPolicy policy)
{
    policy = _policy(policy);
    static const char * const strings[SP_CNT] = {
       [SP_BACKGROUND] = "bg",
       [SP_FOREGROUND] = "fg",
       [SP_SYSTEM]     = "  ",
       [SP_AUDIO_APP]  = "aa",
       [SP_AUDIO_SYS]  = "as",
       [SP_REALTIME]   = "rt", /* Motorola, w04904, 05/10/2013, IKJBXLINE-9555 */
    };
    if ((policy < SP_CNT) && (strings[policy] != NULL))
        return strings[policy];
    else
        return "error";
}
