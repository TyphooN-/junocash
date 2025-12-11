#include "p2pool_status.h"
#include "util/system.h"
#include "util/time.h"
#include "support/events.h"
#undef HTTP_OK
#include "rpc/protocol.h"

#include <event2/http.h>
#include <event2/buffer.h>
#include <univalue.h>

#include <iostream>

//////////////////////////////////////////////////////////////////////////////
//
// P2PoolStatusMonitor Implementation
//

P2PoolStatusMonitor::P2PoolStatusMonitor()
    : m_lastUpdate(0)
{
}

P2PoolStatusMonitor::~P2PoolStatusMonitor()
{
}

P2PoolStatusMonitor& P2PoolStatusMonitor::GetInstance()
{
    static P2PoolStatusMonitor instance;
    return instance;
}

P2PoolStatus P2PoolStatusMonitor::GetStatus()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    // Return cached status if still fresh
    int64_t now = GetTime();
    if (now - m_lastUpdate < CACHE_TTL_SECONDS) {
        return m_cachedStatus;
    }

    // Fetch fresh status
    m_cachedStatus = FetchStatus();
    m_lastUpdate = now;

    return m_cachedStatus;
}

void P2PoolStatusMonitor::RefreshStatus()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cachedStatus = FetchStatus();
    m_lastUpdate = GetTime();
}

bool P2PoolStatusMonitor::IsReady()
{
    P2PoolStatus status = GetStatus();
    return status.connected;
}

P2PoolStatus P2PoolStatusMonitor::FetchStatus()
{
    P2PoolStatus status;

    try {
        // Get P2Pool URL from config
        std::string p2poolUrl = GetArg("-p2poolurl", "http://127.0.0.1:37889");

        // Try to fetch stats from P2Pool HTTP API
        // P2Pool provides a /stats endpoint that returns JSON

        raii_event_base base = obtain_event_base();
        if (!base) {
            return status; // connected = false
        }

        // Parse URL to extract host and port
        std::string host = "127.0.0.1";
        int port = 37889;

        // Simple URL parsing for http://host:port
        if (p2poolUrl.find("http://") == 0) {
            size_t hostStart = 7; // Length of "http://"
            size_t portStart = p2poolUrl.find(':', hostStart);
            size_t pathStart = p2poolUrl.find('/', hostStart);

            if (portStart != std::string::npos) {
                host = p2poolUrl.substr(hostStart, portStart - hostStart);
                if (pathStart != std::string::npos) {
                    port = std::stoi(p2poolUrl.substr(portStart + 1, pathStart - portStart - 1));
                } else {
                    port = std::stoi(p2poolUrl.substr(portStart + 1));
                }
            } else if (pathStart != std::string::npos) {
                host = p2poolUrl.substr(hostStart, pathStart - hostStart);
            } else {
                host = p2poolUrl.substr(hostStart);
            }
        }

        raii_evhttp_connection evcon = obtain_evhttp_connection_base(base.get(), host.c_str(), port);
        if (!evcon) {
            return status;
        }

        evhttp_connection_set_timeout(evcon.get(), 3); // 3 second timeout

        struct HTTPReply {
            bool completed = false;
            int status_code = 0;
            std::string body;
        } reply;

        auto callback = [](struct evhttp_request *req, void *ctx) {
            auto* reply = static_cast<HTTPReply*>(ctx);
            reply->completed = true;

            if (req == NULL) {
                return;
            }

            reply->status_code = evhttp_request_get_response_code(req);

            struct evbuffer *buf = evhttp_request_get_input_buffer(req);
            if (buf) {
                size_t size = evbuffer_get_length(buf);
                const char *data = (const char*)evbuffer_pullup(buf, size);
                if (data) {
                    reply->body = std::string(data, size);
                }
            }
        };

        raii_evhttp_request req = obtain_evhttp_request(callback, (void*)&reply);
        if (!req) {
            return status;
        }

        evhttp_make_request(evcon.get(), req.get(), EVHTTP_REQ_GET, "/stats");
        req.release(); // Ownership transferred

        // Run event loop
        event_base_dispatch(base.get());

        if (reply.status_code == 200 && !reply.body.empty()) {
            // Parse JSON response
            UniValue jsonResp;
            if (jsonResp.read(reply.body)) {
                status.connected = true;

                // Extract stats from JSON
                // Note: The actual field names depend on P2Pool's API
                // Common fields: connections, shares_found, hashrate, pool_statistics

                if (jsonResp.isObject()) {
                    // Try to get number of connected miners/connections
                    const UniValue& connections = find_value(jsonResp, "connections");
                    if (connections.isNum()) {
                        status.connectedMiners = connections.get_int();
                    } else if (connections.isObject()) {
                        // P2Pool may return connections as an object with "incoming" field
                        const UniValue& incoming = find_value(connections, "incoming");
                        if (incoming.isNum()) {
                            status.connectedMiners = incoming.get_int();
                        }
                    }

                    // Get total shares found
                    const UniValue& shares = find_value(jsonResp, "shares_found");
                    if (shares.isNum()) {
                        status.totalShares = shares.get_int64();
                    }

                    // Get pool hashrate
                    const UniValue& hashrate = find_value(jsonResp, "pool_hashrate");
                    if (hashrate.isNum()) {
                        status.poolHashrate = hashrate.get_real();
                    } else {
                        // Try alternate field name
                        const UniValue& hashrate2 = find_value(jsonResp, "hashrate");
                        if (hashrate2.isNum()) {
                            status.poolHashrate = hashrate2.get_real();
                        }
                    }

                    // Get share difficulty
                    const UniValue& shareDiff = find_value(jsonResp, "current_share_diff");
                    if (shareDiff.isNum()) {
                        status.shareDifficulty = shareDiff.get_int64();
                    } else {
                        const UniValue& sidechainDiff = find_value(jsonResp, "sidechain_difficulty");
                        if (sidechainDiff.isNum()) {
                            status.shareDifficulty = sidechainDiff.get_int64();
                        }
                    }

                    // Get last share timestamp
                    const UniValue& lastShare = find_value(jsonResp, "last_share_timestamp");
                    if (lastShare.isNum()) {
                        status.lastShareTimestamp = lastShare.get_int64();
                    }

                    // Get network difficulty
                    const UniValue& netDiff = find_value(jsonResp, "network_difficulty");
                    if (netDiff.isNum()) {
                        status.networkDifficulty = netDiff.get_int64();
                    } else {
                        const UniValue& mainDiff = find_value(jsonResp, "mainchain_difficulty");
                        if (mainDiff.isNum()) {
                            status.networkDifficulty = mainDiff.get_int64();
                        }
                    }

                    // Get effort percentage
                    const UniValue& effort = find_value(jsonResp, "pool_effort");
                    if (effort.isNum()) {
                        status.effortPercent = effort.get_real();
                    }

                    // Try to get stratum connection count if connections field doesn't work
                    if (status.connectedMiners == 0) {
                        const UniValue& stratum = find_value(jsonResp, "stratum");
                        if (stratum.isObject()) {
                            const UniValue& miners = find_value(stratum, "connections");
                            if (miners.isNum()) {
                                status.connectedMiners = miners.get_int();
                            }
                        }
                    }
                }
            }
        }

    } catch (const std::exception& e) {
        LogPrint("rpc", "P2Pool status fetch error: %s\n", e.what());
    }

    return status;
}
