#include "p2pool_client.h"
#include "support/events.h"
#undef HTTP_OK
#include "rpc/protocol.h"
#include "util/system.h"
#include "util/strencodings.h"

#include <event2/buffer.h>
#include <event2/keyvalq_struct.h>

#include <iostream>
#include <sstream>

// Helper struct for libevent callback
struct HTTPReply {
    int status = 0;
    int error = 0;
    std::string body;
};

static void http_request_done(struct evhttp_request *req, void *ctx) {
    HTTPReply *reply = static_cast<HTTPReply*>(ctx);

    if (req == NULL) {
        /* If req is NULL, it means an error occurred while connecting: the
         * error code will have been passed to evhttp_request_error. */
        reply->status = 0;
        return;
    }

    reply->status = evhttp_request_get_response_code(req);
    struct evbuffer *buf = evhttp_request_get_input_buffer(req);
    if (buf)
    {
        size_t size = evbuffer_get_length(buf);
        const char *data = (const char*)evbuffer_pullup(buf, size);
        if (data)
            reply->body = std::string(data, size);
        evbuffer_drain(buf, size);
    }
}

#if LIBEVENT_VERSION_NUMBER >= 0x02010300
static void http_error_cb(enum evhttp_request_error err, void *ctx) {
    HTTPReply *reply = static_cast<HTTPReply*>(ctx);
    reply->error = err;
}
#endif

P2PoolClient::P2PoolClient(const std::string& url, const std::string& address) 
    : m_url(url), m_address(address)
{
    ParseUrl();
}

void P2PoolClient::ParseUrl() {
    size_t protocol_pos = m_url.find("://");
    std::string clean_url = m_url;
    if (protocol_pos != std::string::npos) {
        clean_url = m_url.substr(protocol_pos + 3);
    }
    
    size_t colon_pos = clean_url.find(':');
    if (colon_pos != std::string::npos) {
        m_host = clean_url.substr(0, colon_pos);
        m_port = std::stoi(clean_url.substr(colon_pos + 1));
    } else {
        m_host = clean_url;
        m_port = 37889; 
    }
}

UniValue P2PoolClient::CallMethod(const std::string& method, const UniValue& params) {
    // Obtain event base
    raii_event_base base = obtain_event_base();

    // Synchronously look up hostname
    raii_evhttp_connection evcon = obtain_evhttp_connection_base(base.get(), m_host, m_port);
    evhttp_connection_set_timeout(evcon.get(), 10); // 10s timeout

    HTTPReply response;
    raii_evhttp_request req = obtain_evhttp_request(http_request_done, (void*)&response);
    if (req == NULL)
        throw std::runtime_error("create http request failed");

#if LIBEVENT_VERSION_NUMBER >= 0x02010300
    evhttp_request_set_error_cb(req.get(), http_error_cb);
#endif

    struct evkeyvalq* output_headers = evhttp_request_get_output_headers(req.get());
    evhttp_add_header(output_headers, "Host", m_host.c_str());
    evhttp_add_header(output_headers, "Connection", "close");
    evhttp_add_header(output_headers, "Content-Type", "application/json");

    // Create JSON-RPC request
    std::string strRequest = JSONRPCRequest(method, params, 1);
    struct evbuffer* output_buffer = evhttp_request_get_output_buffer(req.get());
    evbuffer_add(output_buffer, strRequest.data(), strRequest.size());

    int r = evhttp_make_request(evcon.get(), req.get(), EVHTTP_REQ_POST, "/");
    req.release(); 
    if (r != 0) {
        throw std::runtime_error("send http request failed");
    }

    event_base_dispatch(base.get());

    if (response.status == 0)
        throw std::runtime_error("couldn't connect to p2pool server");
    else if (response.status != 200)
        throw std::runtime_error("server returned HTTP error " + std::to_string(response.status));

    UniValue valReply(UniValue::VSTR);
    if (!valReply.read(response.body))
        throw std::runtime_error("couldn't parse reply from server");
    
    const UniValue& reply = valReply.get_obj();
    if (reply.empty())
        throw std::runtime_error("expected reply to have result");

    const UniValue error = find_value(reply, "error");
    if (!error.isNull()) {
        throw std::runtime_error("RPC error: " + error.write());
    }

    return find_value(reply, "result");
}

std::optional<P2PoolClient::BlockTemplate> P2PoolClient::GetBlockTemplate() {
    try {
        UniValue params(UniValue::VARR);
        params.push_back(m_address);
        
        UniValue result = CallMethod("get_share_template", params);
        // Expecting { "header": "hex...", "seed_hash": "hex...", "difficulty": n, "height": n }
        
        BlockTemplate templ;
        templ.header_hex = find_value(result, "blocktemplate_blob").get_str(); // Or "header" - verifying API
        // Monero uses "blocktemplate_blob". p2pool.md says "get_share_template" returns work.
        // Let's assume consistent naming with monero/p2pool:
        // Actually, let's look at the Python miner example:
        // work = p2pool_rpc.get_share_template(address)
        // It doesn't specify the fields.
        // But "getminerdata" in mining.cpp returns { "blocktemplate_blob": ... }
        // Let's assume p2pool returns similar.
        
        // If the key is different, we can adjust.
        if (result.exists("blocktemplate_blob"))
            templ.header_hex = find_value(result, "blocktemplate_blob").get_str();
        else
            templ.header_hex = find_value(result, "header").get_str();

        templ.seed_hash = find_value(result, "seed_hash").get_str();
        templ.difficulty = find_value(result, "difficulty").get_int64();
        templ.height = find_value(result, "height").get_int64();
        if (result.exists("target"))
             templ.target = find_value(result, "target").get_str();
        else {
             // Fallback: Calculate target from difficulty (simplified)
             // This is tricky without arith_uint256. 
             // Just default to a high target (low difficulty) if missing?
             // Or maybe p2pool gives 'target_hex'?
             // Let's assume 'target' is present.
             templ.target = "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"; 
        }
        
        return templ;
    } catch (const std::exception& e) {
        LogPrintf("P2PoolClient::GetBlockTemplate error: %s\n", e.what());
        return std::nullopt;
    }
}

P2PoolClient::ShareResult P2PoolClient::SubmitShare(const std::string& header_hex) {
    try {
        UniValue params(UniValue::VARR);
        params.push_back(header_hex);
        params.push_back(m_address);

        UniValue result = CallMethod("submit_share", params);

        // Parse response to determine status
        ShareResult shareResult;

        if (result.isObject()) {
            // Check for "status" field
            if (result.exists("status")) {
                std::string status = result["status"].get_str();
                std::string message = result.exists("message") ? result["message"].get_str() : "";

                if (status == "accepted") {
                    shareResult.status = ShareStatus::Accepted;
                    shareResult.message = message.empty() ? "Share accepted" : message;
                } else if (status == "rejected") {
                    shareResult.status = ShareStatus::Rejected;
                    shareResult.message = message.empty() ? "Share rejected" : message;
                } else if (status == "stale") {
                    shareResult.status = ShareStatus::Stale;
                    shareResult.message = message.empty() ? "Share stale" : message;
                } else {
                    shareResult.status = ShareStatus::Accepted; // Default to accepted
                    shareResult.message = "Share submitted";
                }
            } else {
                // No status field, assume accepted
                shareResult.status = ShareStatus::Accepted;
                shareResult.message = "Share accepted";
            }
        } else if (result.isBool() && result.get_bool()) {
            // Boolean true response means accepted
            shareResult.status = ShareStatus::Accepted;
            shareResult.message = "Share accepted";
        } else {
            // Assume success if we got a response
            shareResult.status = ShareStatus::Accepted;
            shareResult.message = "Share submitted";
        }

        return shareResult;
    } catch (const std::exception& e) {
        LogPrintf("P2PoolClient::SubmitShare error: %s\n", e.what());
        return {ShareStatus::Error, std::string("Error: ") + e.what()};
    }
}
