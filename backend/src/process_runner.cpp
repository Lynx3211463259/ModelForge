#include "modelforge/process_runner.hpp"

#include "modelforge/util.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <sstream>
#include <cstring>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <cerrno>
#include <fstream>
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace mf {

#ifdef _WIN32
namespace {
std::wstring toWide(const std::string& value) {
    if (value.empty()) return L"";
    int length = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (length <= 0) return L"";
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, &result[0], length);
    if (!result.empty() && result.back() == L'\0') result.pop_back();
    return result;
}

std::string toUtf8(const std::wstring& value) {
    if (value.empty()) return "";
    int length = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (length <= 0) return "";
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, &result[0], length, nullptr, nullptr);
    if (!result.empty() && result.back() == '\0') result.pop_back();
    return result;
}

std::wstring buildEnvironment(const std::map<std::string, std::string>& overrides) {
    std::vector<std::pair<std::string, std::string>> entries;
    LPWCH block = GetEnvironmentStringsW();
    if (block) {
        LPWCH current = block;
        while (*current) {
            std::wstring entry(current);
            const auto eq = entry.find(L'=');
            if (eq != std::wstring::npos) {
                std::string key = toUtf8(entry.substr(0, eq));
                if (!key.empty() && key[0] != '=') entries.emplace_back(key, toUtf8(entry.substr(eq + 1)));
            }
            current += entry.size() + 1;
        }
        FreeEnvironmentStringsW(block);
    }
    for (const auto& overridePair : overrides) {
        bool replaced = false;
        for (auto& entry : entries) {
            if (_stricmp(entry.first.c_str(), overridePair.first.c_str()) == 0) {
                entry.second = overridePair.second;
                replaced = true;
                break;
            }
        }
        if (!replaced) entries.push_back(overridePair);
    }
    std::wstring result;
    for (const auto& entry : entries) {
        result += toWide(entry.first + "=" + entry.second);
        result.push_back(L'\0');
    }
    result.push_back(L'\0');
    return result;
}

void readerLoop(HANDLE handle, bool isError, const std::function<void(const std::string&, bool)>& onLine,
                const std::function<void(const char*, size_t, bool)>& onData) {
    std::string pending;
    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(handle, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
        if (onData) {
            onData(buffer, read, isError);
            continue;
        }
        pending.append(buffer, read);
        size_t start = 0;
        for (size_t i = 0; i < pending.size(); ++i) {
            if (pending[i] == '\n' || pending[i] == '\r') {
                if (i > start) {
                    std::string line = pending.substr(start, i - start);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (onLine && !line.empty()) onLine(line, isError);
                }
                start = i + 1;
            }
        }
        pending.erase(0, start);
        if (pending.size() > 128 * 1024) pending.clear();
    }
    if (!pending.empty() && onLine) onLine(pending, isError);
    CloseHandle(handle);
}
} // namespace
#endif

ProcessResult ProcessRunner::run(const ProcessOptions& options) {
    ProcessResult result;
    const auto started = std::chrono::steady_clock::now();
    auto finish = [&]() {
        result.durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        return result;
    };

    if (options.executable.empty()) {
        result.error = "可执行文件路径为空";
        return finish();
    }

#ifdef _WIN32
    SECURITY_ATTRIBUTES attrs{};
    attrs.nLength = sizeof(attrs);
    attrs.bInheritHandle = TRUE;
    HANDLE stdoutRead = nullptr, stdoutWrite = nullptr, stderrRead = nullptr, stderrWrite = nullptr;
    if (!CreatePipe(&stdoutRead, &stdoutWrite, &attrs, 0) || !CreatePipe(&stderrRead, &stderrWrite, &attrs, 0)) {
        result.error = "无法创建进程管道";
        return finish();
    }
    SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderrRead, HANDLE_FLAG_INHERIT, 0);

    std::wstring commandLine = toWide(shellQuote(options.executable, true));
    for (const auto& arg : options.args) commandLine += L" " + toWide(shellQuote(arg, true));

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = stdoutWrite;
    startup.hStdError = stderrWrite;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    std::wstring cwd = options.workingDirectory.empty() ? L"" : options.workingDirectory.wstring();
    std::wstring envBlock;
    LPVOID envPtr = nullptr;
    if (!options.environment.empty()) {
        envBlock = buildEnvironment(options.environment);
        envPtr = &envBlock[0];
    }
    const std::wstring applicationName = options.executable.find_first_of("\\/") != std::string::npos
        ? toWide(options.executable) : L"";
    const BOOL created = CreateProcessW(
        applicationName.empty() ? nullptr : applicationName.c_str(),
        &commandLine[0], nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
        envPtr, cwd.empty() ? nullptr : cwd.c_str(), &startup, &process);
    CloseHandle(stdoutWrite);
    CloseHandle(stderrWrite);
    if (!created) {
        result.error = "无法启动进程，Windows 错误码: " + std::to_string(GetLastError());
        CloseHandle(stdoutRead);
        CloseHandle(stderrRead);
        return finish();
    }
    result.started = true;

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        AssignProcessToJobObject(job, process.hProcess);
    }
    ResumeThread(process.hThread);

    std::thread outThread(readerLoop, stdoutRead, false, options.onLine, options.onData);
    std::thread errThread(readerLoop, stderrRead, true, options.onLine, options.onData);

    bool canceled = false;
    bool timedOut = false;
    const auto waitStart = std::chrono::steady_clock::now();
    // Process-only counters: GetProcessTimes / GetProcessMemoryInfo / GetProcessIoCounters
    // describe the child process (and its children) rather than the whole machine.
    int64_t lastCpuMs = 0;
    int64_t lastSampleMs = 0;
    auto sampleProcess = [&]() {
        if (!options.onSample) return;
        ProcessSample sample;
        FILETIME creationTime{}, exitTime{}, kernelTime{}, userTime{};
        auto toU64 = [](const FILETIME& ft) {
            return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        };
        if (GetProcessTimes(process.hProcess, &creationTime, &exitTime, &kernelTime, &userTime)) {
            sample.cpuTimeMs = static_cast<int64_t>((toU64(kernelTime) + toU64(userTime)) / 10000ULL);
        }
        PROCESS_MEMORY_COUNTERS_EX memory{};
        memory.cb = sizeof(memory);
        if (GetProcessMemoryInfo(process.hProcess, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))) {
            sample.workingSetBytes = static_cast<uint64_t>(memory.WorkingSetSize);
            sample.peakMemoryBytes = static_cast<uint64_t>(memory.PeakWorkingSetSize);
        }
        IO_COUNTERS io{};
        if (GetProcessIoCounters(process.hProcess, &io)) {
            sample.readBytes = io.ReadTransferCount;
            sample.writeBytes = io.WriteTransferCount;
        }
        // A venv python.exe is a launcher that spawns the real interpreter, so the
        // direct child looks almost idle. Job accounting aggregates every process in
        // the job (including exited children) and is the accurate source when present.
        if (job) {
            JOBOBJECT_BASIC_AND_IO_ACCOUNTING_INFORMATION accounting{};
            if (QueryInformationJobObject(job, JobObjectBasicAndIoAccountingInformation,
                                          &accounting, sizeof(accounting), nullptr)) {
                sample.cpuTimeMs = static_cast<int64_t>(
                    (accounting.BasicInfo.TotalUserTime.QuadPart +
                     accounting.BasicInfo.TotalKernelTime.QuadPart) / 10000LL);
                sample.readBytes = accounting.IoInfo.ReadTransferCount;
                sample.writeBytes = accounting.IoInfo.WriteTransferCount;
            }
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            if (QueryInformationJobObject(job, JobObjectExtendedLimitInformation,
                                          &limits, sizeof(limits), nullptr)) {
                const uint64_t peak = std::max<uint64_t>(limits.PeakJobMemoryUsed,
                                                         limits.PeakProcessMemoryUsed);
                sample.peakMemoryBytes = std::max<uint64_t>(sample.peakMemoryBytes, peak);
                sample.workingSetBytes = sample.peakMemoryBytes;
            }
        }
        const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - waitStart).count();
        sample.elapsedMs = now;
        if (lastSampleMs > 0 && now > lastSampleMs && sample.cpuTimeMs >= lastCpuMs) {
            const double windowMs = static_cast<double>(now - lastSampleMs);
            sample.cpuPercent = std::min(100.0 * 64.0,
                                         static_cast<double>(sample.cpuTimeMs - lastCpuMs) / windowMs * 100.0);
        }
        lastCpuMs = sample.cpuTimeMs;
        lastSampleMs = now;
        options.onSample(sample);
    };
    while (true) {
        const DWORD waitResult = WaitForSingleObject(process.hProcess, 200);
        if (waitResult == WAIT_OBJECT_0) break;
        if (options.cancelFlag && options.cancelFlag->load()) {
            canceled = true;
            if (job) TerminateJobObject(job, 1);
            else TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 5000);
            break;
        }
        if (options.timeoutMs > 0 &&
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - waitStart).count() > options.timeoutMs) {
            timedOut = true;
            if (job) TerminateJobObject(job, 2);
            else TerminateProcess(process.hProcess, 2);
            WaitForSingleObject(process.hProcess, 5000);
            break;
        }
        sampleProcess();
        if (options.onTick) options.onTick();
    }
    sampleProcess();
    if (options.onTick) options.onTick();
    outThread.join();
    errThread.join();
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    result.exitCode = static_cast<int>(exitCode);
    result.canceled = canceled;
    result.timedOut = timedOut;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (job) CloseHandle(job);
    return finish();
#else
    int outPipe[2]{}, errPipe[2]{};
    if (pipe(outPipe) != 0 || pipe(errPipe) != 0) {
        result.error = "无法创建进程管道";
        return finish();
    }
    const pid_t pid = fork();
    if (pid < 0) {
        result.error = "无法创建子进程";
        close(outPipe[0]); close(outPipe[1]); close(errPipe[0]); close(errPipe[1]);
        return finish();
    }
    if (pid == 0) {
        setpgid(0, 0);
        if (!options.workingDirectory.empty()) chdir(options.workingDirectory.c_str());
        for (const auto& pair : options.environment) setenv(pair.first.c_str(), pair.second.c_str(), 1);
        dup2(outPipe[1], STDOUT_FILENO);
        dup2(errPipe[1], STDERR_FILENO);
        close(outPipe[0]); close(outPipe[1]); close(errPipe[0]); close(errPipe[1]);
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(options.executable.c_str()));
        for (const auto& arg : options.args) argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);
        if (options.executable.find('/') == std::string::npos) execvp(options.executable.c_str(), argv.data());
        else execv(options.executable.c_str(), argv.data());
        _exit(127);
    }
    close(outPipe[1]);
    close(errPipe[1]);
    result.started = true;

    auto reader = [&](int fd, bool isError) {
        std::string pending;
        char buffer[4096];
        ssize_t count = 0;
        while ((count = read(fd, buffer, sizeof(buffer))) > 0) {
            if (options.onData) {
                options.onData(buffer, static_cast<size_t>(count), isError);
                continue;
            }
            pending.append(buffer, static_cast<size_t>(count));
            size_t start = 0;
            for (size_t i = 0; i < pending.size(); ++i) {
                if (pending[i] == '\n' || pending[i] == '\r') {
                    if (i > start) {
                        std::string line = pending.substr(start, i - start);
                        if (!line.empty() && line.back() == '\r') line.pop_back();
                        if (options.onLine && !line.empty()) options.onLine(line, isError);
                    }
                    start = i + 1;
                }
            }
            pending.erase(0, start);
        }
        close(fd);
    };
    std::thread outThread(reader, outPipe[0], false);
    std::thread errThread(reader, errPipe[0], true);

    bool canceled = false;
    bool timedOut = false;
    int status = 0;
    const auto waitStart = std::chrono::steady_clock::now();
    int64_t lastCpuMs = 0;
    int64_t lastSampleMs = 0;
    auto sampleProcess = [&]() {
        if (!options.onSample) return;
        ProcessSample sample;
        const std::string base = "/proc/" + std::to_string(pid);
        {
            std::ifstream stat(base + "/stat");
            std::string content;
            std::getline(stat, content);
            const auto close = content.rfind(')');
            if (close != std::string::npos) {
                std::istringstream rest(content.substr(close + 2));
                std::vector<std::string> fields;
                std::string field;
                while (rest >> field) fields.push_back(field);
                // Fields after comm start at state (index 0); utime=11, stime=12, rss=21.
                if (fields.size() > 21) {
                    const long ticks = std::atol(fields[11].c_str()) + std::atol(fields[12].c_str());
                    sample.cpuTimeMs = static_cast<int64_t>(ticks * 1000 / 100); // USER_HZ is 100
                    sample.workingSetBytes = static_cast<uint64_t>(std::atoll(fields[21].c_str())) * 4096ULL;
                }
            }
        }
        {
            std::ifstream status2(base + "/status");
            std::string line;
            while (std::getline(status2, line)) {
                if (line.rfind("VmHWM:", 0) == 0) {
                    sample.peakMemoryBytes = static_cast<uint64_t>(std::atoll(line.c_str() + 6)) * 1024ULL;
                }
            }
        }
        {
            std::ifstream io(base + "/io");
            std::string line;
            while (std::getline(io, line)) {
                if (line.rfind("read_bytes:", 0) == 0) sample.readBytes = static_cast<uint64_t>(std::atoll(line.c_str() + 11));
                if (line.rfind("write_bytes:", 0) == 0) sample.writeBytes = static_cast<uint64_t>(std::atoll(line.c_str() + 12));
            }
        }
        const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - waitStart).count();
        sample.elapsedMs = now;
        if (lastSampleMs > 0 && now > lastSampleMs && sample.cpuTimeMs >= lastCpuMs) {
            sample.cpuPercent = static_cast<double>(sample.cpuTimeMs - lastCpuMs) /
                                static_cast<double>(now - lastSampleMs) * 100.0;
        }
        lastCpuMs = sample.cpuTimeMs;
        lastSampleMs = now;
        options.onSample(sample);
    };
    while (true) {
        const pid_t waitResult = waitpid(pid, &status, WNOHANG);
        if (waitResult == pid) break;
        if (options.cancelFlag && options.cancelFlag->load()) {
            canceled = true;
            kill(-pid, SIGTERM);
            for (int i = 0; i < 20; ++i) {
                if (waitpid(pid, &status, WNOHANG) == pid) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            kill(-pid, SIGKILL);
            waitpid(pid, &status, 0);
            break;
        }
        if (options.timeoutMs > 0 &&
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - waitStart).count() > options.timeoutMs) {
            timedOut = true;
            kill(-pid, SIGKILL);
            waitpid(pid, &status, 0);
            break;
        }
        sampleProcess();
        if (options.onTick) options.onTick();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    sampleProcess();
    if (options.onTick) options.onTick();
    outThread.join();
    errThread.join();
    if (WIFEXITED(status)) result.exitCode = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) result.exitCode = 128 + WTERMSIG(status);
    result.canceled = canceled;
    result.timedOut = timedOut;
    return finish();
#endif
}

} // namespace mf
