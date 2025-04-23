// Copyright 2025 PingCAP, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <common/types.h>
#include <unistd.h>

#include <cstdlib>

namespace DB
{

/**
 * @brief Control the start and stop of profiling when used with Linux perf.
 * For example, in benchmarks, you don't want perf to include your bootstrap stage stacks.
 *
 * Usage with perf:
 *
 *  mkfifo ctl_fd.fifo
 *  exec {ctl_fd}<>ctl_fd.fifo
 *  PERF_CTL_FD=$ctl_fd perf record  --control fd:${ctl_fd} -F 99 -g --delay=-1 -- dbms/bench_dbms --benchmark_filter="...." --benchmark_min_time=30.0
 *  exec {ctl_fd}>&-
 *  unlink ctl_fd.fifo
 *  perf script -i perf.data | ./FlameGraph/stackcollapse-perf.pl > perf.fold
 *
 */
class PerfControl
{
public:
    static void enable()
    {
        static auto control_fd = get_fd();
        if (control_fd > 0)
        {
            write(control_fd, "enable\n", 7);
            fsync(control_fd);
        }
    }

    static void disable()
    {
        static auto control_fd = get_fd();
        if (control_fd > 0)
        {
            write(control_fd, "disable\n", 8);
            fsync(control_fd);
        }
    }

private:
    static Int64 get_fd()
    {
        auto * env_fd = getenv("PERF_CTL_FD");
        if (env_fd)
            return std::strtol(env_fd, nullptr, 10);
        return 0;
    }
};

} // namespace DB
