#include "p2pool_manager.h"
#include "util/system.h"
#include "util/time.h"
#include "support/events.h"
#include "chainparams.h"

#include <event2/http.h>
#include <event2/buffer.h>

#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <boost/filesystem.hpp>

#ifndef WIN32
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>
#else
#include <tlhelp32.h>
#endif

//////////////////////////////////////////////////////////////////////////////
//
// P2PoolProcessManager Implementation
//

// Define static constants
const int P2PoolProcessManager::MAX_RESTART_ATTEMPTS;
const int P2PoolProcessManager::HEALTH_CHECK_INTERVAL_MS;
const int P2PoolProcessManager::MAX_HTTP_FAILURES;
const int P2PoolProcessManager::GRACEFUL_SHUTDOWN_WAIT_MS;

P2PoolProcessManager::P2PoolProcessManager()
    : m_pid(0), m_startTime(0), m_running(false), m_stopMonitoring(false),
      m_monitorThread(nullptr), m_restartAttempts(0), m_httpFailures(0)
#ifdef WIN32
      , m_processHandle(NULL)
#endif
{
}

P2PoolProcessManager::~P2PoolProcessManager()
{
    Stop();
}

P2PoolProcessManager& P2PoolProcessManager::GetInstance()
{
    static P2PoolProcessManager instance;
    return instance;
}

std::string GetP2PoolBinaryPath()
{
    std::string customPath = GetArg("-p2poolbinary", "");
    if (!customPath.empty()) {
        return customPath;
    }

    // Default: look in data directory first, then same directory as daemon
    boost::filesystem::path dataDir = GetDataDir();
    boost::filesystem::path binaryPath = dataDir / "junocash-p2pool";

#ifdef WIN32
    binaryPath = dataDir / "junocash-p2pool.exe";
#endif

    if (boost::filesystem::exists(binaryPath)) {
        return binaryPath.string();
    }

    // Try same directory as junocashd executable
    boost::filesystem::path programDir = boost::filesystem::current_path();
    binaryPath = programDir / "junocash-p2pool";

#ifdef WIN32
    binaryPath = programDir / "junocash-p2pool.exe";
#endif

    return binaryPath.string();
}

std::vector<std::string> P2PoolProcessManager::BuildP2PoolArgs(const P2PoolConfig& config) const
{
    std::vector<std::string> args;

    // Connection to junocashd
    args.push_back("--host");
    args.push_back(config.host);

    args.push_back("--rpc-port");
    args.push_back(std::to_string(config.rpcPort));

    // RPC credentials
    if (!config.rpcUser.empty()) {
        args.push_back("--rpc-login");
        args.push_back(config.rpcUser + ":" + config.rpcPassword);
    }

    // Wallet address
    args.push_back("--wallet");
    args.push_back(config.walletAddress);

    // Stratum server (bind to all interfaces so external miners can connect)
    args.push_back("--stratum");
    args.push_back("0.0.0.0:37889");

    // Light mode if requested
    if (config.lightMode) {
        args.push_back("--light-mode");
    }

    // Disable RandomX since junocashd provides it via calc_pow RPC
    // (P2Pool is built with -DWITH_RANDOMX=OFF)

    return args;
}

bool P2PoolProcessManager::Start(const P2PoolConfig& config)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_running) {
        LogPrintf("P2Pool: Already running (PID %d)\n", m_pid);
        return true;
    }

    // Validate config
    if (config.binaryPath.empty()) {
        LogPrintf("P2Pool: Error - binary path not configured\n");
        return false;
    }

    if (!boost::filesystem::exists(config.binaryPath)) {
        LogPrintf("P2Pool: Error - binary not found at %s\n", config.binaryPath);
        return false;
    }

    if (config.walletAddress.empty()) {
        LogPrintf("P2Pool: Error - wallet address required\n");
        return false;
    }

    // Build command-line arguments
    std::vector<std::string> args = BuildP2PoolArgs(config);

    // Spawn the process
    if (!SpawnProcess(config.binaryPath, args)) {
        LogPrintf("P2Pool: Failed to spawn process\n");
        return false;
    }

    m_startTime = GetTime();
    m_lastConfig = config;
    m_httpFailures = 0;

    LogPrintf("P2Pool: Started (PID %d)\n", m_pid);

    // Start monitoring thread
    m_stopMonitoring = false;
    m_monitorThread = new boost::thread(boost::bind(&P2PoolProcessManager::MonitorThread, this));

    return true;
}

void P2PoolProcessManager::Stop()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_running) {
        return;
    }

    LogPrintf("P2Pool: Stopping (PID %d)...\n", m_pid);

    // Stop monitoring thread
    m_stopMonitoring = true;
    if (m_monitorThread) {
        m_monitorThread->interrupt();
        m_monitorThread->join();
        delete m_monitorThread;
        m_monitorThread = nullptr;
    }

    // Kill the process
    KillProcess();

    m_running = false;
    m_pid = 0;
    m_startTime = 0;
    m_restartAttempts = 0;

    LogPrintf("P2Pool: Stopped\n");
}

bool P2PoolProcessManager::Restart()
{
    LogPrintf("P2Pool: Restarting...\n");

    // Increment restart counter
    m_restartAttempts++;

    if (m_restartAttempts > MAX_RESTART_ATTEMPTS) {
        LogPrintf("P2Pool: Max restart attempts (%d) reached, giving up\n", MAX_RESTART_ATTEMPTS);
        m_running = false;
        return false;
    }

    // Exponential backoff: 1s, 2s, 4s, 8s, 16s (max)
    int backoffMs = std::min(1000 * (1 << (m_restartAttempts - 1)), 16000);
    LogPrintf("P2Pool: Waiting %d ms before restart (attempt %d/%d)\n",
              backoffMs, m_restartAttempts, MAX_RESTART_ATTEMPTS);
    MilliSleep(backoffMs);

    // Kill existing process if still alive
    if (m_pid != 0) {
        KillProcess();
        m_pid = 0;
    }

    // Try to start again with last config
    std::vector<std::string> args = BuildP2PoolArgs(m_lastConfig);
    if (!SpawnProcess(m_lastConfig.binaryPath, args)) {
        LogPrintf("P2Pool: Restart failed\n");
        m_running = false;
        return false;
    }

    m_startTime = GetTime();
    m_httpFailures = 0;
    LogPrintf("P2Pool: Restarted successfully (PID %d)\n", m_pid);

    return true;
}

bool P2PoolProcessManager::IsRunning() const
{
    return m_running.load();
}

pid_t P2PoolProcessManager::GetPID() const
{
    return m_pid;
}

int64_t P2PoolProcessManager::GetUptime() const
{
    if (!m_running || m_startTime == 0) {
        return 0;
    }
    return GetTime() - m_startTime;
}

int P2PoolProcessManager::GetRestartAttempts() const
{
    return m_restartAttempts;
}

bool P2PoolProcessManager::IsHealthy() const
{
    return m_running && (m_httpFailures < MAX_HTTP_FAILURES);
}

//////////////////////////////////////////////////////////////////////////////
//
// Platform-specific process spawning
//

#ifndef WIN32

bool P2PoolProcessManager::SpawnProcess(const std::string& binaryPath, const std::vector<std::string>& args)
{
    pid_t pid = fork();

    if (pid < 0) {
        LogPrintf("P2Pool: fork() failed: %s\n", strerror(errno));
        return false;
    }

    if (pid == 0) {
        // Child process

        // Redirect stdout and stderr to log file
        boost::filesystem::path logPath = GetDataDir() / "p2pool.log";
        int fd = open(logPath.string().c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }

        // Close stdin
        close(STDIN_FILENO);

        // Build argv for execv
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(binaryPath.c_str()));
        for (const auto& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);

        // Execute P2Pool
        execv(binaryPath.c_str(), argv.data());

        // If execv returns, it failed
        std::cerr << "P2Pool: execv() failed: " << strerror(errno) << std::endl;
        _exit(1);
    }

    // Parent process
    m_pid = pid;
    m_running = true;

    return true;
}

bool P2PoolProcessManager::KillProcess()
{
    if (m_pid <= 0) {
        return true;
    }

    LogPrintf("P2Pool: Sending SIGTERM to PID %d\n", m_pid);

    // Try graceful shutdown with SIGTERM
    if (kill(m_pid, SIGTERM) == 0) {
        // Wait up to GRACEFUL_SHUTDOWN_WAIT_MS for process to exit
        int waited = 0;
        while (waited < GRACEFUL_SHUTDOWN_WAIT_MS) {
            if (!IsProcessAlive(m_pid)) {
                LogPrintf("P2Pool: Process exited gracefully\n");
                return true;
            }
            MilliSleep(100);
            waited += 100;
        }

        // Still alive, send SIGKILL
        LogPrintf("P2Pool: Process did not exit, sending SIGKILL\n");
        kill(m_pid, SIGKILL);

        // Wait for SIGKILL to take effect
        MilliSleep(500);
        waitpid(m_pid, nullptr, WNOHANG);
    }

    return true;
}

bool P2PoolProcessManager::IsProcessAlive(pid_t pid) const
{
    if (pid <= 0) {
        return false;
    }

    // Send signal 0 to check if process exists
    return (kill(pid, 0) == 0);
}

#else // WIN32

bool P2PoolProcessManager::SpawnProcess(const std::string& binaryPath, const std::vector<std::string>& args)
{
    // Build command line
    std::string cmdLine = "\"" + binaryPath + "\"";
    for (const auto& arg : args) {
        cmdLine += " \"" + arg + "\"";
    }

    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;

    // Create log file
    boost::filesystem::path logPath = GetDataDir() / "p2pool.log";
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    HANDLE hLogFile = CreateFileA(
        logPath.string().c_str(),
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &sa,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (hLogFile != INVALID_HANDLE_VALUE) {
        si.hStdOutput = hLogFile;
        si.hStdError = hLogFile;
        si.hStdInput = NULL;
    }

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    // Create the process
    BOOL success = CreateProcessA(
        NULL,                   // Application name
        const_cast<char*>(cmdLine.c_str()), // Command line
        NULL,                   // Process handle not inheritable
        NULL,                   // Thread handle not inheritable
        TRUE,                   // Inherit handles
        0,                      // No creation flags
        NULL,                   // Use parent's environment
        NULL,                   // Use parent's starting directory
        &si,                    // STARTUPINFO
        &pi                     // PROCESS_INFORMATION
    );

    if (hLogFile != INVALID_HANDLE_VALUE) {
        CloseHandle(hLogFile);
    }

    if (!success) {
        LogPrintf("P2Pool: CreateProcess() failed: %d\n", GetLastError());
        return false;
    }

    m_pid = pi.dwProcessId;
    m_processHandle = pi.hProcess;
    CloseHandle(pi.hThread);
    m_running = true;

    return true;
}

bool P2PoolProcessManager::KillProcess()
{
    if (m_processHandle == NULL) {
        return true;
    }

    LogPrintf("P2Pool: Terminating process (PID %d)\n", m_pid);

    // Try graceful shutdown first (send WM_CLOSE)
    // Note: P2Pool may not have a window, so this might not work
    // We'll go straight to TerminateProcess after a brief wait

    // Wait a bit to see if process exits on its own
    if (WaitForSingleObject(m_processHandle, GRACEFUL_SHUTDOWN_WAIT_MS) == WAIT_OBJECT_0) {
        LogPrintf("P2Pool: Process exited gracefully\n");
        CloseHandle(m_processHandle);
        m_processHandle = NULL;
        return true;
    }

    // Force termination
    LogPrintf("P2Pool: Force terminating process\n");
    TerminateProcess(m_processHandle, 1);
    WaitForSingleObject(m_processHandle, 1000);
    CloseHandle(m_processHandle);
    m_processHandle = NULL;

    return true;
}

bool P2PoolProcessManager::IsProcessAlive(pid_t pid) const
{
    if (m_processHandle == NULL) {
        return false;
    }

    DWORD exitCode;
    if (GetExitCodeProcess(m_processHandle, &exitCode)) {
        return (exitCode == STILL_ACTIVE);
    }

    return false;
}

#endif // WIN32

//////////////////////////////////////////////////////////////////////////////
//
// Health monitoring
//

bool P2PoolProcessManager::CheckHttpHealth() const
{
    // Simple HTTP GET to P2Pool stats endpoint
    // Use libevent for HTTP request

    raii_event_base base = obtain_event_base();
    if (!base) {
        return false;
    }

    raii_evhttp_connection evcon = obtain_evhttp_connection_base(base.get(), "127.0.0.1", 37889);
    if (!evcon) {
        return false;
    }

    evhttp_connection_set_timeout(evcon.get(), 3); // 3 second timeout

    struct HttpReply {
        bool completed;
        bool success;
    };
    HttpReply reply = {false, false};

    auto callback = [](struct evhttp_request *req, void *ctx) {
        HttpReply* reply = static_cast<HttpReply*>(ctx);
        reply->completed = true;

        if (req == NULL) {
            reply->success = false;
            return;
        }

        int status = evhttp_request_get_response_code(req);
        reply->success = (status == 200);
    };

    raii_evhttp_request req = obtain_evhttp_request(callback, (void*)&reply);
    if (!req) {
        return false;
    }

    evhttp_make_request(evcon.get(), req.get(), EVHTTP_REQ_GET, "/stats");
    req.release(); // Ownership transferred to connection

    // Event loop
    event_base_dispatch(base.get());

    return reply.success;
}

void P2PoolProcessManager::MonitorThread()
{
    LogPrintf("P2Pool: Monitor thread started\n");

    while (!m_stopMonitoring) {
        try {
            // Check if process is still alive
            if (!IsProcessAlive(m_pid)) {
                LogPrintf("P2Pool: Process died unexpectedly, attempting restart\n");
                if (!Restart()) {
                    LogPrintf("P2Pool: Unable to restart, stopping monitor\n");
                    break;
                }
                continue;
            }

            // Check HTTP health
            if (!CheckHttpHealth()) {
                m_httpFailures++;
                LogPrintf("P2Pool: HTTP health check failed (%d/%d)\n",
                         m_httpFailures, MAX_HTTP_FAILURES);

                if (m_httpFailures >= MAX_HTTP_FAILURES) {
                    LogPrintf("P2Pool: Too many HTTP failures, restarting\n");
                    if (!Restart()) {
                        LogPrintf("P2Pool: Unable to restart, stopping monitor\n");
                        break;
                    }
                }
            } else {
                // Health check passed, reset counters
                if (m_httpFailures > 0) {
                    LogPrintf("P2Pool: HTTP health check passed, resetting failure count\n");
                }
                m_httpFailures = 0;
                m_restartAttempts = 0; // Reset restart counter on successful health check
            }

            // Sleep until next check
            MilliSleep(HEALTH_CHECK_INTERVAL_MS);

        } catch (const boost::thread_interrupted&) {
            LogPrintf("P2Pool: Monitor thread interrupted\n");
            break;
        } catch (const std::exception& e) {
            LogPrintf("P2Pool: Monitor thread exception: %s\n", e.what());
            MilliSleep(HEALTH_CHECK_INTERVAL_MS);
        }
    }

    LogPrintf("P2Pool: Monitor thread stopped\n");
}
