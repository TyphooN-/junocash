#ifndef P2POOL_CLIENT_H
#define P2POOL_CLIENT_H

#include <string>
#include <univalue.h>
#include <vector>
#include <optional>

class P2PoolClient {
public:
    P2PoolClient(const std::string& url, const std::string& address);
    
    struct BlockTemplate {
        std::string header_hex;
        std::string seed_hash;
        uint64_t difficulty;
        uint64_t height;
        std::string target;
    };

    enum class ShareStatus {
        Accepted,
        Rejected,
        Stale,
        Error
    };

    struct ShareResult {
        ShareStatus status;
        std::string message;
    };

    // Fetches a new block template from the P2Pool node
    std::optional<BlockTemplate> GetBlockTemplate();

    // Submits a found share to the P2Pool node
    ShareResult SubmitShare(const std::string& header_hex);

private:
    std::string m_url;
    std::string m_address;
    std::string m_host;
    int m_port;
    std::string m_path;

    UniValue CallMethod(const std::string& method, const UniValue& params);
    void ParseUrl();
};

#endif
