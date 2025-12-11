#ifndef P2POOL_STATUS_H
#define P2POOL_STATUS_H

#include <string>
#include <atomic>
#include <mutex>
#include "p2pool_client.h"

/**
 * P2Pool status information
 */
struct P2PoolStatus {
    bool connected;
    int connectedMiners;
    uint64_t totalShares;
    double poolHashrate;

    // Progress tracking fields
    uint64_t shareDifficulty;      // Current share difficulty
    int64_t lastShareTimestamp;     // Unix timestamp of last share found
    uint64_t networkDifficulty;     // Current network difficulty
    double effortPercent;           // Pool effort percentage (0-100+)

    P2PoolStatus()
        : connected(false), connectedMiners(0), totalShares(0), poolHashrate(0.0),
          shareDifficulty(0), lastShareTimestamp(0), networkDifficulty(0), effortPercent(0.0) {}
};

/**
 * Singleton monitor for P2Pool status
 * Polls the P2Pool HTTP API for statistics with caching
 */
class P2PoolStatusMonitor {
public:
    static P2PoolStatusMonitor& GetInstance();

    // Get current status (cached for 5 seconds)
    P2PoolStatus GetStatus();

    // Check if P2Pool is ready to accept work
    bool IsReady();

    // Force refresh of status (bypasses cache)
    void RefreshStatus();

    // Singleton: delete copy/move constructors
    P2PoolStatusMonitor(const P2PoolStatusMonitor&) = delete;
    P2PoolStatusMonitor& operator=(const P2PoolStatusMonitor&) = delete;

private:
    P2PoolStatusMonitor();
    ~P2PoolStatusMonitor();

    // Fetch fresh status from P2Pool
    P2PoolStatus FetchStatus();

    // State
    P2PoolStatus m_cachedStatus;
    int64_t m_lastUpdate;
    mutable std::mutex m_mutex;

    // Cache TTL in seconds
    static const int CACHE_TTL_SECONDS = 5;
};

#endif // P2POOL_STATUS_H
