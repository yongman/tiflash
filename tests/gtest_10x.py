# Copyright 2025 PingCAP, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Copyright 2013 Google Inc. All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# A 10x faster gtest runner.

import io
import dataclasses  # Python 3.7
import asyncio  # Python 3.4
import asyncio.subprocess
import asyncio.exceptions
import optparse
import sys
import os
import heapq
import shutil
import enum
import time
import re
import traceback

# fmt: off
class C:  # Colors
    RESET     = "\033[0m"
    OK        = "\033[92m\033[1m"
    FAIL      = "\033[91m\033[1m"
    WARN      = "\033[93m\033[1m"
    TIME_OK   = "\033[90m"
    TIME_WARN = "\033[93m\033[1m"
class Out:
    OK      =   f"{C.OK}OK:      {C.RESET}"
    FAILED  = f"{C.FAIL}FAILED:  {C.RESET}"
    SKIPPED = f"{C.WARN}SKIPPED: {C.RESET}"
# fmt: on


@dataclasses.dataclass
class Options:
    values: optparse.Values = None
    binaries: list[str] = dataclasses.field(default_factory=list)
    additional_args: list[str] = dataclasses.field(default_factory=list)
    skip_tests: set[str] = dataclasses.field(default_factory=set)
    suppress_tests: set[str] = dataclasses.field(default_factory=set)


@dataclasses.dataclass(order=True)
class TestCase:
    binary: str
    test_suite: str
    full_test_name: str


@dataclasses.dataclass(order=True)
class TestGroup:
    binary: str
    tests: list[str]


@dataclasses.dataclass
class TestStatus:
    @enum.unique
    class Result(enum.Enum):
        NOT_RUN = 0
        PASS = 1
        FAIL = 2
        FAIL_TIMEOUT = 3
        FAIL_NOT_RUN = 4

    result: Result = Result.NOT_RUN
    start_at: float = 0
    end_at: float = 0


class TestStatusRepo(dict[str, dict[str, TestStatus]]):
    pass


class ProcWatcher:
    """
    Collect result from the execution of a single test binary.
    """

    # fmt: off
    r_run         = re.compile(r"^\[ RUN      \] (.+)$")
    r_failed      = re.compile(r"^\[  FAILED  \] (.+) \(\d+ ms\)$")
    r_ok          = re.compile(r"^\[       OK \] (.+) \(\d+ ms\)$")
    # fmt: on

    max_log_per_test = (
        2000  # All logs are buffered in memory. Limit max lines to avoids OOM.
    )

    def __init__(
        self,
        sync_log_writer: io.TextIOWrapper,
        status_repo: TestStatusRepo,
        binary: str,
        test_names: list[str],
        options: Options,
        print_prefix: str = "",
    ):
        self.proc = None
        self.sync_log_writer = sync_log_writer
        self.log_writer_tasks = []
        self.test_names = test_names
        self.options = options
        self.prefix = print_prefix

        # All status are updated into the global status_repo
        if binary not in status_repo:
            status_repo[binary] = {}
        self.status = status_repo[binary]
        for test_name in test_names:
            self.status[test_name] = TestStatus()

        self.current_logs = []
        self.current_test_name = ""
        self.has_timed_out = False
        self.test_timeout_task = None

    async def handle_proc(self, proc: asyncio.subprocess.Process):
        assert self.proc is None
        self.proc = proc
        is_user_interrupt = False
        tasks = [
            asyncio.create_task(self._handle_stdout(proc.stdout)),
            asyncio.create_task(self._handle_stderr(proc.stderr)),
        ]
        try:
            await asyncio.gather(*tasks)
            tasks.clear()
            # We only wait file write finish after the test execution.
            # If we wait write inside test execution, we will not be able to
            # handle real-time stdout and stderr streaming in time and lose
            # their order.
            await asyncio.gather(*self.log_writer_tasks)
            self.log_writer_tasks.clear()
        except asyncio.CancelledError:
            is_user_interrupt = True
            raise
        except Exception:
            # If any exception happens we cancel the current process
            # because this is not expected
            print(
                f"{self.prefix}{C.FAIL}Meet Python exception when running test, test will be aborted:{C.RESET}"
            )
            traceback.print_exception(sys.exception(), chain=True, colorize=True)
        finally:
            if self.test_timeout_task:
                self.test_timeout_task.cancel()
            for task in tasks:
                if not task.done():
                    task.cancel()
            for task in self.log_writer_tasks:
                if not task.done():
                    task.cancel()
            if not proc.returncode:
                try:
                    proc.kill()
                except ProcessLookupError:
                    pass
            await proc.wait()
            if not is_user_interrupt:
                if proc.returncode != 0:
                    self._on_proc_exit_fail(proc.returncode)
                else:
                    self._on_proc_exit_ok()

    async def _handle_stdout(self, stream: asyncio.StreamReader):
        while not stream.at_eof():
            data = await stream.readline()
            line = data.decode("utf-8", errors="replace").rstrip()
            if len(self.current_logs) < self.max_log_per_test:
                self.current_logs.append(line)
            try:
                if r := self.r_run.match(line):
                    self._on_test_run(r.group(1))
                elif r := self.r_ok.match(line):
                    self._on_test_pass(r.group(1))
                elif r := self.r_failed.match(line):
                    self._on_test_fail(r.group(1))
            except KeyError as exc:
                raise RuntimeError(
                    f"Unable to find test from stdout line: {line}"
                ) from exc

    async def _read_line(self, stream: asyncio.StreamReader):
        # Alternative to stream.readline that will not throw BufferFull error
        try:
            return await stream.readuntil(b"\n")
        except asyncio.exceptions.IncompleteReadError as e:
            return e.partial
        except asyncio.exceptions.LimitOverrunError as e:
            return await stream.read(e.consumed)

    async def _handle_stderr(self, stream: asyncio.StreamReader):
        while not stream.at_eof():
            data = await self._read_line(stream)  # Allow partial read
            line = data.decode("utf-8", errors="replace").rstrip()
            self.current_logs.append(line)
            if len(self.current_logs) > self.max_log_per_test:
                # Discard remaining logs. We still need to keep reading
                # to avoid blocking the stderr pipe.
                while not stream.at_eof():
                    _ = await stream.read(1024)
                return

    def _reset_timeout(self):
        if self.test_timeout_task:
            self.test_timeout_task.cancel()
        self.test_timeout_task = asyncio.create_task(self._test_timeout_trigger())

    # Must be called in another thread to avoid blocking the async loop
    def _sync_write_log(self, log_lines: list[str]):
        additional_log = ""
        if len(log_lines) >= self.max_log_per_test:
            additional_log = f"Some logs are truncated (exceeds max log lines: {self.max_log_per_test}).\n"
        self.sync_log_writer.write("\n".join(log_lines) + "\n" + additional_log + "\n")

    def _elapsed(self, s: TestStatus) -> str:
        if s.start_at == 0 or s.end_at == 0:
            return ""
        ms = int((s.end_at - s.start_at) * 1000)
        color = C.TIME_OK if ms < 1000 else C.TIME_WARN
        return f" {color}{ms}ms{C.RESET}"

    def _on_test_run(self, test_name: str):
        s = self.status[test_name]
        s.result = TestStatus.Result.NOT_RUN
        s.start_at = time.time()
        self.current_test_name = test_name
        self._reset_timeout()

    def _on_test_run_finished(self, test_name: str):
        s = self.status[test_name]
        s.end_at = time.time()
        self.current_logs = []
        self.current_test_name = ""
        # Sometimes process may stuck after test finished, so we keep a timeout tracker even
        # a test is finished
        self._reset_timeout()

    def _on_test_pass(self, test_name: str):
        s = self.status[test_name]
        s.result = TestStatus.Result.PASS
        self._on_test_run_finished(test_name)
        print(f"{self.prefix}{Out.OK}{test_name}{self._elapsed(s)}")

    def _on_test_fail(self, test_name: str):
        s = self.status[test_name]
        s.result = (
            TestStatus.Result.FAIL
            if not self.has_timed_out
            else TestStatus.Result.FAIL_TIMEOUT
        )
        self.log_writer_tasks.append(
            asyncio.create_task(
                asyncio.to_thread(self._sync_write_log, self.current_logs)
            )
        )
        self._on_test_run_finished(test_name)

        p_timeout = f" {C.FAIL}(timedout){C.RESET}" if self.has_timed_out else ""
        p_suppressed = (
            f" {C.WARN}(fail suppressed){C.RESET}"
            if test_name in self.options.suppress_tests
            else ""
        )
        print(
            f"{self.prefix}{Out.FAILED}{test_name}{self._elapsed(s)}{p_timeout}{p_suppressed}"
        )

    def _on_test_fail_not_run(self, test_name: str):
        s = self.status[test_name]
        s.result = TestStatus.Result.FAIL_NOT_RUN
        print(f"{self.prefix}{Out.FAILED}{test_name} {C.WARN}(not run yet){C.RESET}")

    def _on_proc_exit_fail(self, exit_code: int):
        if self.current_test_name:
            self._on_test_fail(self.current_test_name)
        for test_name in self.test_names:
            if self.status[test_name].result == TestStatus.Result.NOT_RUN:
                self._on_test_fail_not_run(test_name)
        print(
            f"{self.prefix}{C.WARN}Test process exited with code {exit_code}.{C.RESET}"
        )

    def _on_proc_exit_ok(self):
        for test_name in self.test_names:
            assert self.status[test_name].result != TestStatus.Result.NOT_RUN

    async def _test_timeout_trigger(self):
        await asyncio.sleep(self.options.values.timeout)
        self.has_timed_out = True
        try:
            self.proc.kill()
        except ProcessLookupError:
            pass


def default_options_parser() -> optparse.OptionParser:
    # fmt: off
    parser = optparse.OptionParser(usage="usage: %prog [options] binary [binary ...] -- [additional args]")
    parser.add_option("--working_dir", type="string", default="", help="specify the CWD for running the test and where to output log files")
    parser.add_option("-w", "--workers", type="int", default=os.cpu_count(), help="number of workers to spawn")
    parser.add_option("--shard_count", type="int", default=1, help="total number of shards (for sharding test execution between multiple machines)")
    parser.add_option("--shard_index", type="int", default=0, help="zero-indexed number identifying this shard (for sharding test execution between multiple machines)")
    parser.add_option("--skip_list", type="string", default="", help="skip running these tests in the file")
    parser.add_option("--suppress_list", type="string", default="", help="suppress failures caused by these tests in the file")
    parser.add_option("--timeout", type="int", default=60, help="interrupt current test after specified timeout (in seconds)")
    # fmt: on
    return parser


async def find_tests(options: Options) -> list[TestCase]:
    tests_n = 0  # Include discarded tests
    tests = []
    for test_binary in options.binaries:
        command = [test_binary] + options.additional_args + ["--gtest_list_tests"]
        proc = await asyncio.create_subprocess_exec(
            *command,
            limit=512 * 1024,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.DEVNULL,
            cwd=options.values.working_dir,
        )

        test_group = ""
        while not proc.stdout.at_eof():
            data = await proc.stdout.readline()
            line = data.decode("utf-8").rstrip()
            if not line.strip():
                continue
            if line[0] != " ":
                test_group = line.split("#")[0].strip()
                continue
            line = line.split("#")[0].strip()
            if not line:
                continue

            test_name = test_group + line
            tests_n += 1

            if "DISABLED_" in test_name:
                continue
            if (tests_n - options.values.shard_index) % options.values.shard_count != 0:
                continue
            if test_name in options.skip_tests:
                print(f"        {Out.SKIPPED}{test_name}")
                continue

            tests.append(TestCase(test_binary, test_group, test_name))

    return tests


async def execute_tests(
    sync_log_writer: io.TextIOWrapper,
    status_repo: TestStatusRepo,
    tests: list[TestCase],
    options: Options,
):
    # Group tests of the same test suite together, avoid running them in parallel
    # to avoid race conditions (--serialize_test_cases)
    test_groups = {}
    for test in tests:
        key = (test.binary, test.test_suite)
        if key not in test_groups:
            test_groups[key] = TestGroup(test.binary, [])
        test_groups[key].tests.append(test.full_test_name)

    # Reorder test groups by the number of tests in each group, so that the
    # distribution of tests to workers is more even. (If we find large number of tests
    # at the very end, then it cause uneven. So we need to first process these large tests)
    test_groups = dict(
        sorted(test_groups.items(), key=lambda x: len(x[1].tests), reverse=True)
    )

    # Distribute all tests to n workers in a pre-calculated way.
    # If we have 5 workers, we will generate 5*10 tasks, because although test case numbers
    # are balanced, test time is not balanced. In this way, if a worker finish tests early
    # it will take over more tests to work with.
    worker_factor = 10
    h: list[tuple[int, list[TestGroup]]] = []
    for _ in range(options.values.workers * worker_factor):
        heapq.heappush(h, (0, []))
    for test_group in test_groups.values():
        item = heapq.heappop(h)
        item = (item[0] + len(test_group.tests), item[1] + [test_group])
        heapq.heappush(h, item)

    concurrency_guard = asyncio.Semaphore(options.values.workers)
    tasks = []
    tests_to_run = 0
    for _, item in enumerate(h):
        if item[0] == 0:  # No test in this group
            continue
        # Group tests in the same binary together
        groups_by_binary: dict[str, TestGroup] = {}
        for test_group in item[1]:
            if test_group.binary not in groups_by_binary:
                groups_by_binary[test_group.binary] = TestGroup(test_group.binary, [])
            groups_by_binary[test_group.binary].tests.extend(test_group.tests)
            tests_to_run += len(test_group.tests)
        for test_group in groups_by_binary.values():
            tasks.append(
                spawn_test_proc(
                    concurrency_guard,
                    sync_log_writer,
                    status_repo,
                    len(tasks),
                    test_group,
                    options,
                )
            )

    print(
        f"Discovered {tests_to_run} tests to run, distributed into {len(tasks)} groups with concurrency={options.values.workers}."
    )
    print()

    await asyncio.gather(*tasks)


async def spawn_test_proc(
    concurrency_controller: asyncio.Semaphore,
    sync_log_writer: io.TextIOWrapper,
    status_repo: TestStatusRepo,
    task_idx: int,
    test_group: TestGroup,
    options: Options,
):
    async with concurrency_controller:
        watcher = ProcWatcher(
            sync_log_writer,
            status_repo,
            test_group.binary,
            test_group.tests,
            options,
            print_prefix=f"#{task_idx}".ljust(8),
        )
        command = [test_group.binary] + options.additional_args
        command.append("--gtest_filter=" + ":".join(test_group.tests))
        proc = await asyncio.create_subprocess_exec(
            *command,
            stderr=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.PIPE,
            cwd=options.values.working_dir,
        )
        await watcher.handle_proc(proc)


def sync_read_test_list_file(file_path: str) -> set[str]:
    l = set()
    with open(file_path, "r", encoding="utf-8") as f:
        while line := f.readline():
            # Remove anything after #
            line = line.split("#")[0].strip()
            if line:
                l.add(line)
    return l


async def main():
    options = Options()

    # Remove additional arguments (anything after --)
    for i, arg in enumerate(sys.argv):
        if arg == "--":
            options.additional_args = sys.argv[i + 1 :]
            sys.argv = sys.argv[:i]
            break

    parser = default_options_parser()
    options.values, options.binaries = parser.parse_args()

    if not options.binaries:
        parser.print_usage()
        sys.exit(1)

    unique_binaries = set(os.path.basename(binary) for binary in options.binaries)
    assert len(unique_binaries) == len(
        options.binaries
    ), "All test binaries must have an unique basename."

    # Resolve into absolute path because we will change the working directory when
    # running the tests.
    for binary in options.binaries:
        assert os.path.exists(binary), f"Binary not found: {binary}"
    options.binaries = [os.path.abspath(binary) for binary in options.binaries]

    if options.values.working_dir == "":
        options.values.working_dir = "./gtest_10x_workdir"
        # We will not cleanup the WD if it is specified by user,
        # because user may specify WD that may not want clean up, e.g. `./`
        if os.path.exists(options.values.working_dir):
            shutil.rmtree(options.values.working_dir)
    os.makedirs(options.values.working_dir, exist_ok=True)

    if options.values.skip_list:
        options.skip_tests = sync_read_test_list_file(options.values.skip_list)
    if options.values.suppress_list:
        options.suppress_tests = sync_read_test_list_file(options.values.suppress_list)
        if options.suppress_tests:
            print(
                f"{C.WARN}Warning: --suppress_list is specified, failures from these tests will not be counted:{C.RESET}"
            )
            for test in options.suppress_tests:
                print(f"  {C.WARN}{test}{C.RESET}")
            print()

    error_log_file = os.path.join(options.values.working_dir, "test_errors.log")
    print(f"Test case failures will be log to: {error_log_file}")

    try:
        status_repo = TestStatusRepo()
        tests = await find_tests(options)
        with open(
            error_log_file,
            "w",
            encoding="utf-8",
        ) as log_writer:
            await execute_tests(log_writer, status_repo, tests, options)

        # Print summary
        results_summary: dict[TestStatus.Result, int] = {
            TestStatus.Result.PASS: 0,
            TestStatus.Result.FAIL: 0,
            TestStatus.Result.FAIL_TIMEOUT: 0,
            TestStatus.Result.FAIL_NOT_RUN: 0,
        }
        failed_tests_suppressed = 0

        for status_by_binary in status_repo.values():
            for test_name, status in status_by_binary.items():
                assert status.result != TestStatus.Result.NOT_RUN
                if (
                    status.result != TestStatus.Result.PASS
                    and test_name in options.suppress_tests
                ):
                    failed_tests_suppressed += 1
                else:
                    results_summary[status.result] += 1

        print("\n\nAll tests finished.")

        pass_tests = results_summary[TestStatus.Result.PASS]
        failed_tests = (
            results_summary[TestStatus.Result.FAIL]
            + results_summary[TestStatus.Result.FAIL_NOT_RUN]
            + results_summary[TestStatus.Result.FAIL_TIMEOUT]
        )

        print(
            f"{C.OK}{pass_tests}{C.RESET} tests passed, "
            f"{C.FAIL}{failed_tests}{C.RESET} tests failed, "
            f"{C.FAIL}{failed_tests_suppressed}{C.RESET} test failures are suppressed."
        )
        if results_summary[TestStatus.Result.FAIL_NOT_RUN] > 0:
            print(
                f"Note: {results_summary[TestStatus.Result.FAIL_NOT_RUN]} of failed tests were not run due to previous test failures."
            )
        if failed_tests > 0:
            print(f"\n{C.FAIL}TEST FAILED.{C.RESET}")
            print(f"Failure details: {error_log_file}")
            return 1

        print(f"\n{C.OK}TESTS PASSED.{C.RESET}")
        return 0

    except asyncio.CancelledError:
        print(f"{C.FAIL}Interrupted by user.{C.RESET}")
        return 1


if __name__ == "__main__":
    # Python 3.9: built-in collection typing
    assert sys.version_info >= (3, 9), "Python 3.9 or later is required."
    exit(asyncio.run(main()))
