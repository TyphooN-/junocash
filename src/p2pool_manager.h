#ifndef P2POOL_MANAGER_H
#define P2POOL_MANAGER_H

#include <string>
#include <atomic>
#include <mutex>
#include <boost/thread.hpp>

#ifdef WIN32
#include <windows.h>
typedef DWORD pid_t;
#else
#include <sys/types.h>
#include <unistd.h>
#endif

/**
 * Configuration for launching a P2Pool process
 */
struct P2PoolConfig {
    std::string binaryPath;
    std::string walletAddress;
    std::string host;
    int rpcPort;
    bool lightMode;
    std::string rpcUser;
    std::string rpcPassword;

    P2PoolConfig() : host("127.0.0.1"), rpcPort(8232), lightMode(false) {}
};

/**
 * Singleton manager for P2Pool process lifecycle
 * Handles starting, stopping, monitoring, and auto-restarting the P2Pool daemon
 */
class P2PoolProcessManager {
public:
    static P2PoolProcessManager& GetInstance();

    // Process lifecycle
    bool Start(const P2PoolConfig& config);
    void Stop();
    bool Restart();

    // Status queries
    bool IsRunning() const;
    pid_t GetPID() const;
    int64_t GetUptime() const;
    int GetRestartAttempts() const;
    bool IsHealthy() const;

    // Singleton: delete copy/move constructors
    P2PoolProcessManager(const P2PoolProcessManager&) = delete;
    P2PoolProcessManager& operator=(const P2PoolProcessManager&) = delete;

private:
    P2PoolProcessManager();
    ~P2PoolProcessManager();

    // Process management
    bool SpawnProcess(const std::string& binaryPath, const std::vector<std::string>& args);
    bool KillProcess();
    bool IsProcessAlive(pid_t pid) const;
    bool CheckHttpHealth() const;
    void MonitorThread();
    std::vector<std::string> BuildP2PoolArgs(const P2PoolConfig& config) const;

    // State
    pid_t m_pid;
    int64_t m_startTime;
    std::atomic<bool> m_running;
    std::atomic<bool> m_stopMonitoring;
    boost::thread* m_monitorThread;
    mutable std::mutex m_mutex;

    // Auto-restart state
    int m_restartAttempts;
    int m_httpFailures;
    P2PoolConfig m_lastConfig;

    // Constants
    static const int MAX_RESTART_ATTEMPTS = 5;
    static const int HEALTH_CHECK_INTERVAL_MS = 5000;
    static const int MAX_HTTP_FAILURES = 3;
    static const int GRACEFUL_SHUTDOWN_WAIT_MS = 5000;

#ifdef WIN32
    HANDLE m_processHandle;
#endif
};

// Helper function to get P2Pool binary path
std::string GetP2PoolBinaryPath();

#endif // P2POOL_MANAGER_H
