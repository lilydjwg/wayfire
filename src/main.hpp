#ifndef MAIN_HPP
#define MAIN_HPP

#include <sys/resource.h>

extern struct wf_runtime_config
{
    bool no_damage_track = false;
    bool damage_debug    = false;
} runtime_config;

void restore_nofile_limit(void);

static struct rlimit original_nofile_rlimit;

#endif /* end of include guard: MAIN_HPP */
