
#include <thread>
#include <sys/eventfd.h>
#include <fcntl.h>
#include <unistd.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/functional.h>
#include "store/benchmark/async/bench_client.h"
#include "store/benchmark/async/micro/micro_client.h"
#include "store/common/backend/redis_store.h"
#include "store/common/frontend/request_utils.h"
#include "store/common/frontend/transaction_utils.h"
#include "gflags/gflags.h"
#include <valgrind/callgrind.h>
#include <algorithm>
#include <atomic>
#include <csignal>
#include <sstream>
#include <vector>
#include "lib/latency.h"
#include "lib/tcptransport.h"
#include "lib/timeval.h"
#include "store/benchmark/async/common/key_selector.h"
#include "store/benchmark/async/common/uniform_key_selector.h"
#include "store/benchmark/async/common/zipf_key_selector.h"
#include "store/benchmark/async/retwis/retwis_client.h"
#include "store/common/partitioner.h"
#include "store/common/stats.h"
#include "store/common/truetime.h"
#include "store/strongstore/client.h"
#include "store/strongstore/networkconfig.h"
#include <iostream>  // for std::cout
#include <cstdint>   // for uint64_t
#include <memory>
#include <string>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include "lib/configuration.h"
#include <typeinfo>

enum protomode_t
{
    PROTO_UNKNOWN,
    PROTO_STRONG,
    PROTO_VR,
    PROTO_IOCL_CT
};

enum benchmode_t
{
    BENCH_UNKNOWN,
    BENCH_RETWIS,
    BENCH_MICRO,
};

enum keysmode_t
{
    KEYS_UNKNOWN,
    KEYS_UNIFORM,
    KEYS_ZIPF
};

enum transmode_t
{
    TRANS_UNKNOWN,
    TRANS_UDP,
    TRANS_TCP,
};

/**
 * System settings.
 */
DECLARE_uint64(client_id);
DECLARE_string(client_host);
// Declare other flags you need similarly
DECLARE_string(replica_config_paths);
DECLARE_uint64(num_shards);
DECLARE_bool(ping_replicas);
DECLARE_string(net_config_path);
DECLARE_bool(debug_stats);

const std::string trans_args[] = {"udp", "tcp"};

const transmode_t transmodes[]{TRANS_UDP, TRANS_TCP};
static bool ValidateTransMode(const char *flagname, const std::string &value)
{
    int n = sizeof(trans_args);
    for (int i = 0; i < n; ++i)
    {
        if (value == trans_args[i])
        {
            return true;
        }
    }
    std::cerr << "Invalid value for --" << flagname << ": " << value
              << std::endl;
    return false;
}
DECLARE_string(trans_protocol);
DECLARE_string(protocol_mode);
DECLARE_string(strong_consistency);
DECLARE_double(nb_time_alpha);
DECLARE_string(benchmark);

const std::string protocol_args[] = {"span-lock", "vr", "iocl_ct"};
const protomode_t protomodes[]{PROTO_STRONG, PROTO_VR, PROTO_IOCL_CT};
const strongstore::Mode strongmodes[]{strongstore::Mode::MODE_SPAN_LOCK};
static bool ValidateProtocolMode(const char *flagname,
                                 const std::string &value)
{
    int n = sizeof(protocol_args);
    for (int i = 0; i < n; ++i)
    {
        if (value == protocol_args[i])
        {
            return true;
        }
    }
    std::cerr << "Invalid value for --" << flagname << ": " << value
              << std::endl;
    return false;
}

const std::string strong_consistency_args[] = {"ss", "rss", "lin"};
const strongstore::Consistency strong_consistency[]{
    strongstore::Consistency::SS,
    strongstore::Consistency::RSS,
    strongstore::Consistency::LIN,
};
static bool ValidateStrongConsistency(const char *flagname,
                                      const std::string &value)
{
    int n = sizeof(strong_consistency_args);
    for (int i = 0; i < n; ++i)
    {
        if (value == strong_consistency_args[i])
        {
            return true;
        }
    }
    std::cerr << "Invalid value for --" << flagname << ": " << value
              << std::endl;
    return false;
}

const std::string benchmark_args[] = {"retwis", "micro"};
const benchmode_t benchmodes[]{BENCH_RETWIS, BENCH_MICRO};
static bool ValidateBenchmark(const char *flagname, const std::string &value)
{
    int n = sizeof(benchmark_args);
    for (int i = 0; i < n; ++i)
    {
        if (value == benchmark_args[i])
        {
            return true;
        }
    }
    std::cerr << "Invalid value for --" << flagname << ": " << value
              << std::endl;
    return false;
}

/**
 * Experiment settings.
 */
DECLARE_uint64(exp_duration);
DECLARE_uint64(warmup_secs);
DECLARE_uint64(cooldown_secs);
DECLARE_uint64(tput_interval);
DECLARE_int64(num_requests);
DECLARE_int32(closest_replica);
DECLARE_string(closest_replicas);
DECLARE_uint64(delay);
DECLARE_uint64(clock_error);
DECLARE_string(stats_file);
DECLARE_uint64(abort_backoff);
DECLARE_bool(retry_aborted);
DECLARE_int64(max_attempts);
DECLARE_uint64(message_timeout);
DECLARE_uint64(max_backoff);
DECLARE_uint64(client_fanout);
DECLARE_bool(client_issue_concurrent);
DECLARE_bool(transformed_app);

const std::string partitioner_args[] = {"default", "warehouse_dist_items",
                                        "warehouse"};
const partitioner_t parts[]{DEFAULT, WAREHOUSE_DIST_ITEMS, WAREHOUSE};
static bool ValidatePartitioner(const char *flagname,
                                const std::string &value)
{
    int n = sizeof(partitioner_args);
    for (int i = 0; i < n; ++i)
    {
        if (value == partitioner_args[i])
        {
            return true;
        }
    }
    std::cerr << "Invalid value for --" << flagname << ": " << value
              << std::endl;
    return false;
}
DECLARE_string(partitioner);
DECLARE_string(keys_path);
DECLARE_uint64(num_keys);

const std::string keys_args[] = {"uniform", "zipf"};
const keysmode_t keysmodes[]{KEYS_UNIFORM, KEYS_ZIPF};
static bool ValidateKeys(const char *flagname, const std::string &value)
{
    int n = sizeof(keys_args);
    for (int i = 0; i < n; ++i)
    {
        if (value == keys_args[i])
        {
            return true;
        }
    }
    std::cerr << "Invalid value for --" << flagname << ": " << value
              << std::endl;
    return false;
}

const BenchmarkClientMode bench_modes[]{OPEN, CLOSED};
const std::string bench_args[] = {"open", "closed"};
static bool ValidateBenchMode(const char *flagname, const std::string &value)
{
    int n = sizeof(bench_args);
    for (int i = 0; i < n; ++i)
    {
        if (value == bench_args[i])
        {
            return true;
        }
    }
    std::cerr << "Invalid value for --" << flagname << ": " << value
              << std::endl;
    return false;
}

// Additional flags that are used in the code but not defined in benchmark.cc
DECLARE_string(key_selector);
DECLARE_string(bench_mode);
DECLARE_double(zipf_coefficient);
DECLARE_double(client_arrival_rate);
DECLARE_double(client_think_time);
DECLARE_double(client_stay_probability);
DECLARE_double(mpl);
DECLARE_double(client_switch_probability);
DECLARE_uint64(num_ops_txn);

// Additional server and client settings
DECLARE_uint64(server_port);
DECLARE_uint64(server_load_time);
DECLARE_bool(server_preload_keys);
DECLARE_bool(client_debug_output);
DECLARE_uint64(client_rand_sleep);
DECLARE_uint64(client_read_percentage);
DECLARE_uint64(client_write_percentage);
DECLARE_uint64(client_conflict_percentage);
DECLARE_uint64(client_rmw_percentage);
DECLARE_uint64(client_zipfian_s);
DECLARE_uint64(client_zipfian_v);
DECLARE_uint64(client_max_processors);
DECLARE_bool(client_random_coordinator);
DECLARE_bool(client_disable_gc);
DECLARE_bool(client_gc_debug_trace);
DECLARE_bool(client_cpuprofile);

// TPCC and other benchmark specific flags
DECLARE_int32(tpcc_num_warehouses);
DECLARE_int32(warehouse_per_shard);
DECLARE_int32(clients_per_warehouse);
DECLARE_int32(remote_item_milli_p);
DECLARE_int32(tpcc_w_id);
DECLARE_int32(tpcc_C_c_id);
DECLARE_int32(tpcc_C_c_last);
DECLARE_int32(tpcc_new_order_ratio);
DECLARE_int32(tpcc_delivery_ratio);
DECLARE_int32(tpcc_stock_level_ratio);
DECLARE_int32(tpcc_payment_ratio);
DECLARE_int32(tpcc_order_status_ratio);
DECLARE_bool(static_w_id);
DECLARE_int32(balance_ratio);
DECLARE_int32(deposit_checking_ratio);
DECLARE_int32(transact_saving_ratio);
DECLARE_int32(amalgamate_ratio);
DECLARE_int32(write_check_ratio);
DECLARE_int32(num_hotspots);
DECLARE_int32(num_customers);
DECLARE_double(hotspot_probability);
DECLARE_int32(timeout);

// Old global variables - no longer used, replaced by static variables in CreateBenchmarkClient
// std::vector<Client *> clients;
// std::vector<BenchmarkClient *> benchClients;
// Transport *tport;
// Partitioner *part;
// KeySelector *keySelector;

namespace py = pybind11;

// Global pointer to BenchmarkClient instance.
std::unique_ptr<BenchmarkClient> benchmarkClient = nullptr;

// Forward declaration
py::object value_to_python(const request_utils::Value& val);

// Helper function to convert Python objects to Value
request_utils::Value python_to_value(const py::object& obj) {
    if (py::isinstance<py::str>(obj)) {
        return request_utils::Value::NewString(obj.cast<std::string>());
    }
    else if (py::isinstance<py::list>(obj)) {
        return request_utils::Value::NewList(obj.cast<std::vector<std::string>>());
    }
    else if (py::isinstance<py::set>(obj)) {
        return request_utils::Value::NewSet(obj.cast<std::unordered_set<std::string>>());
    }
    else if (py::isinstance<py::dict>(obj)) {
        return request_utils::Value::NewHash(obj.cast<std::unordered_map<std::string, std::string>>());
    }
    return request_utils::NIL;
}

// Keep storage so references remain valid
static std::unique_ptr<TCPTransport> s_transport;
static std::unique_ptr<KeySelector> s_keySelector;
static std::vector<Client *> s_clients;
static std::unique_ptr<Partitioner> s_partitioner;

// Helper functions for environment variables
static std::string GetEnvOr(const char *name, const char *def) {
    const char *v = std::getenv(name);
    return v ? std::string(v) : std::string(def);
}

static double GetEnvDouble(const char *name, const char *def) {
    return std::stod(GetEnvOr(name, def));
}

static uint64_t GetEnvU64(const char *name, const char *def) {
    return static_cast<uint64_t>(std::stoull(GetEnvOr(name, def)));
}

static int GetEnvInt(const char *name, const char *def) {
    return std::stoi(GetEnvOr(name, def));
}

static bool GetEnvBool(const char *name, const char *def) {
    std::string s = GetEnvOr(name, def);
    return s == "1" || s == "true" || s == "TRUE";
}

std::unique_ptr<BenchmarkClient> CreateBenchmarkClient() {
    // 1) Transport
    if (!s_transport) {
        s_transport.reset(new TCPTransport(0.0, 0.0, 0, false));
        // std::cerr << "[CreateBenchmarkClient] Transport created (TCP) at " << s_transport.get() << std::endl;
    } else {
        // std::cerr << "[CreateBenchmarkClient] Reusing existing transport at " << s_transport.get() << std::endl;
    }

    // 2) Keys + selector
    if (!s_keySelector) {
        std::vector<std::string> keys;
        uint64_t num_keys = GetEnvU64("IOCL_CLIENT_NUM_KEYS", "1");
        if (num_keys == 0) {
            num_keys = 1;
        }
        std::string key = "0000000000";
        for (uint64_t i = 0; i < num_keys; ++i) {
            keys.emplace_back(key);
        }
        s_keySelector.reset(new UniformKeySelector(keys));
        // std::cout << "[CreateBenchmarkClient] KeySelector created (uniform) with " << keys.size() << " keys at " << s_keySelector.get() << std::endl;
    } 

    // 3) Clients vector (empty or placeholder)
    s_clients.clear();
    if (!s_partitioner) {
        s_partitioner.reset(new DefaultPartitioner());
    }

    // 4) Parse minimal config
    BenchmarkClientMode bench_mode = (GetEnvOr("IOCL_BENCH_MODE", "closed") == "open") ? OPEN : CLOSED;

    uint32_t timeout_ms = static_cast<uint32_t>(GetEnvU64("IOCL_MESSAGE_TIMEOUT", "60000"));
    uint64_t id = GetEnvU64("IOCL_CLIENT_ID", "0") << 4;

    double switch_probability = GetEnvDouble("IOCL_CLIENT_SWITCH_PROBABILITY", "0.0");
    double arrival_rate = GetEnvDouble("IOCL_CLIENT_ARRIVAL_RATE", "1.0");
    double think_time = GetEnvDouble("IOCL_CLIENT_THINK_TIME", "1.0");
    double stay_probability = GetEnvDouble("IOCL_CLIENT_STAY_PROBABILITY", "0.5");
    int mpl = GetEnvInt("IOCL_MPL", "1");

    int expDuration = static_cast<int>(GetEnvU64("IOCL_EXP_DURATION", "30"));
    int warmupSec = static_cast<int>(GetEnvU64("IOCL_WARMUP_SECS", "5"));
    int cooldownSec = static_cast<int>(GetEnvU64("IOCL_COOLDOWN_SECS", "5"));
    int tputInterval = static_cast<int>(GetEnvU64("IOCL_TPUT_INTERVAL", "0"));
    uint32_t abortBackoff = static_cast<uint32_t>(GetEnvU64("IOCL_ABORT_BACKOFF", "1"));
    bool retryAborted = GetEnvBool("IOCL_RETRY_ABORTED", "true");
    uint32_t maxBackoff = static_cast<uint32_t>(GetEnvU64("IOCL_MAX_BACKOFF", "2500"));
    uint32_t maxAttempts = static_cast<uint32_t>(GetEnvU64("IOCL_MAX_ATTEMPTS", "4294967295"));
    uint64_t fanout = GetEnvU64("IOCL_CLIENT_FANOUT", "1");
    bool issueConcurrent = GetEnvBool("IOCL_CLIENT_ISSUE_CONCURRENT", "false");

    // 3a) Build real strongstore clients from config
    std::string replica_paths = GetEnvOr("IOCL_REPLICA_CONFIG_PATHS", "");
    std::string net_config_path = GetEnvOr("IOCL_NET_CONFIG_PATH", "");
    std::string client_host = GetEnvOr("IOCL_CLIENT_HOST", "localhost");
    uint64_t num_shards = GetEnvU64("IOCL_NUM_SHARDS", "1");
    int closest_replica = GetEnvInt("IOCL_CLOSEST_REPLICA", "-1");
    bool debug_stats = GetEnvBool("IOCL_DEBUG_STATS", "false");
    double nb_time_alpha = GetEnvDouble("IOCL_NB_TIME_ALPHA", "1.0");
    std::string consistency_s = GetEnvOr("IOCL_CONSISTENCY", "lin");
    strongstore::Consistency consistency = strongstore::Consistency::LIN;
    if (consistency_s == "rss") consistency = strongstore::Consistency::RSS;
    if (consistency_s == "ss")  consistency = strongstore::Consistency::SS;

    std::string protocol_mode_s = GetEnvOr("IOCL_PROTOCOL_MODE", "vr");
    strongstore::LinearizableProtocol protocol_mode = strongstore::LinearizableProtocol::VR;
    if (protocol_mode_s == "iocl_ct") protocol_mode = strongstore::LinearizableProtocol::PROTO_IOCL_CT;
    if (protocol_mode_s == "vr") protocol_mode = strongstore::LinearizableProtocol::VR;
    if (protocol_mode_s == "span-lock") protocol_mode = strongstore::LinearizableProtocol::PROTO_STRONG;

    // std::cout << "[CreateBenchmarkClient] Config: replicas='" << replica_paths
    //           << "' net='" << net_config_path
    //           << "' host='" << client_host
    //           << "' shards=" << num_shards
    //           << "' closest_replica=" << closest_replica
    //           << "' consistency='" << consistency_s << "'" << std::endl;

    if (!replica_paths.empty() && !net_config_path.empty()) {
        // std::cout << "[CreateBenchmarkClient] Attempting to load configs from paths:" << std::endl;
        // std::cout << "[CreateBenchmarkClient]   Replica paths: '" << replica_paths << "'" << std::endl;
        // std::cout << "[CreateBenchmarkClient]   Network config: '" << net_config_path << "'" << std::endl;
        
        std::ifstream net_config_stream(net_config_path);
        if (net_config_stream) {
            // std::cout << "[CreateBenchmarkClient] Successfully opened network config file" << std::endl;
            
            std::stringstream paths(replica_paths);
            std::string path;
            std::vector<transport::Configuration> replica_configs;
            std::vector<strongstore::NetworkConfiguration> net_configs;
            std::vector<std::string> client_regions;
            
            int idx = 0;
            while (std::getline(paths, path, ',')) {
                // std::cout << "[CreateBenchmarkClient] Processing replica path: '" << path << "'" << std::endl;
                
                std::ifstream replica_config_stream(path);
                if (!replica_config_stream) {
                    // std::cout << "[CreateBenchmarkClient] ERROR: Unable to read replica config '" << path << "'" << std::endl;
                    continue;
                }
                
                // std::cout << "[CreateBenchmarkClient] Successfully opened replica config file" << std::endl;
                
                try {
                    replica_configs.emplace_back(replica_config_stream);
                    // std::cout << "[CreateBenchmarkClient] Created transport::Configuration #" << idx << std::endl;
                    
                    net_config_stream.clear();
                    net_config_stream.seekg(0);
                    
                    net_configs.emplace_back(replica_configs[idx], net_config_stream);
                    // std::cout << "[CreateBenchmarkClient] Created NetworkConfiguration #" << idx << std::endl;
                    
                    client_regions.emplace_back(net_configs[idx].GetRegion(client_host));
                    // std::cout << "[CreateBenchmarkClient] Loaded replica config #" << idx
                    //           << ", region='" << client_regions.back() << "' from '" << path << "'" << std::endl;
                    idx++;
                } catch (const std::exception& e) {
                    std::cout << "[CreateBenchmarkClient] EXCEPTION creating config #" << idx << ": " << e.what() << std::endl;
                } catch (...) {
                    std::cout << "[CreateBenchmarkClient] UNKNOWN EXCEPTION creating config #" << idx << std::endl;
                }
            }
            
            // std::cout << "[CreateBenchmarkClient] Config creation summary:" << std::endl;
            // std::cout << "[CreateBenchmarkClient]   Replica configs: " << replica_configs.size() << std::endl;
            // std::cout << "[CreateBenchmarkClient]   Network configs: " << net_configs.size() << std::endl;
            // std::cout << "[CreateBenchmarkClient]   Client regions: " << client_regions.size() << std::endl;

            if (!replica_configs.empty() && !net_configs.empty()) {
                // std::cout << "[CreateBenchmarkClient] Creating strongstore clients..." << std::endl;
                
                TrueTime tt{static_cast<uint64_t>(GetEnvU64("IOCL_CLOCK_ERROR", "0"))};
                for (std::size_t i = 0; i < replica_configs.size(); ++i) {
                    // std::cout << "[CreateBenchmarkClient] Creating client #" << i << "..." << std::endl;
                    
                    try {
                        auto &shard_config = replica_configs[i];
                        auto &net_config = net_configs[i];
                        auto &region = client_regions[i];
                        Client *c = new strongstore::Client(
                            consistency, protocol_mode, net_config, region, shard_config,
                            GetEnvU64("IOCL_CLIENT_ID", "0"), static_cast<int>(num_shards), closest_replica,
                            s_transport.get(), s_partitioner.get(), tt, debug_stats, nb_time_alpha);
                        
                        s_clients.push_back(c);
                    } catch (const std::exception& e) {
                        std::cout << "[CreateBenchmarkClient] EXCEPTION creating client #" << i << ": " << e.what() << std::endl;
                    } catch (...) {
                        std::cout << "[CreateBenchmarkClient] UNKNOWN EXCEPTION creating client #" << i << std::endl;
                    }
                }
            } else {
                std::cout << "[CreateBenchmarkClient] ERROR: No configs created, cannot create clients" << std::endl;
            }
        } else {
            std::cout << "[CreateBenchmarkClient] ERROR: Failed to open network config file '" << net_config_path << "'" << std::endl;
        }
    } else {
        std::cout << "[CreateBenchmarkClient] WARNING: Missing config paths:" << std::endl;
        std::cout << "[CreateBenchmarkClient]   Replica paths empty: " << (replica_paths.empty() ? "yes" : "no") << std::endl;
        std::cout << "[CreateBenchmarkClient]   Network config empty: " << (net_config_path.empty() ? "yes" : "no") << std::endl;
    }

    if (s_clients.empty()) {
        // As a fallback (should be avoided if real config provided), create a placeholder
        s_clients.push_back(nullptr);
        std::cout << "[CreateBenchmarkClient] No real clients created, using placeholder" << std::endl;
    }

    BenchmarkClient *bench = new micro::MicroClient(
        s_keySelector.get(),
        s_clients,
        timeout_ms,
        *s_transport,
        id,
        bench_mode,
        switch_probability,
        arrival_rate, think_time, stay_probability,
        mpl,
        expDuration, warmupSec, cooldownSec, tputInterval,
        abortBackoff, retryAborted, maxBackoff, maxAttempts,
        fanout,
        issueConcurrent,
        0
    );

    return std::unique_ptr<BenchmarkClient>(bench);
}


// SendRequest - Synchronous version that chains request and response
// std::pair<bool, request_utils::Value> SendRequest(uint64_t session_id, request_utils::Operation op, int64_t key, const request_utils::Value& newVal, const request_utils::Value& oldVal) {
//     std::cout << "[SendRequest] Called with session_id=" << session_id 
//               << ", op=" << static_cast<int>(op) 
//               << ", key=" << key << std::endl;

//     if (!benchmarkClient) {
//         std::cout << "[SendRequest] Creating new benchmark client" << std::endl;
//         benchmarkClient = CreateBenchmarkClient();
//     }
    
//     // Call SendAsynchRequest from BenchmarkClient and get the command ID
//     std::cout << "[SendRequest] Calling SendAsynchRequest..." << std::endl;
//     std::tuple<bool, request_utils::Value> result = benchmarkClient->SendAsynchRequest(session_id, op, key, newVal, oldVal);
    
//     // Extract the command ID from the result (assuming it's the second element)
//     uint64_t commandId = std::get<1>(result).type == request_utils::ValueType::STRING ? 
//                         std::stoull(std::get<1>(result).str) : 0;
    
//     std::cout << "[SendRequest] Got command ID: " << commandId << ", awaiting response..." << std::endl;

//     // Immediately await the response using the command ID
//     std::tuple<request_utils::Value, uint64_t> response = benchmarkClient->AwaitAsynchResponse(session_id, commandId);

//     // bool success = std::get<1>(response) == 0;
//     int efd = std::get<1>(response);
//     std::cout << "[SendRequest] Response received, efd=" << efd << std::endl;
    
//     // Return the response value and success status
//     return {efd, std::get<0>(response)};
// }

// AsyncSendRequest - Asynchronous version of SendRequest
std::pair<bool, request_utils::Value> AsyncSendRequest(uint64_t session_id, request_utils::Operation op, int64_t key, const request_utils::Value& newVal, const request_utils::Value& oldVal) {
    // std::cout << "[AsyncSendRequest] op=" << static_cast<int>(op) << std::endl;
    // std::cout << typeid(session_id).name() << typeid(op).name() << typeid(key).name() << typeid(newVal).name() <<  typeid(oldVal).name() << std::endl;
    
    if (!benchmarkClient) {
        std::cout << "[AsyncSendRequest] Creating new benchmark client" << std::endl;
        benchmarkClient = CreateBenchmarkClient();
    }
    
    std::tuple<bool, request_utils::Value> result = benchmarkClient->SendAsynchOperation(session_id, op, key, newVal, oldVal);
    
    // Print the result using value_to_python
    (void)value_to_python(std::get<1>(result));
    
    return {std::get<0>(result), std::get<1>(result)};
}

// AsyncGetResponse - Retrieve the result of an asynchronous request
std::pair<bool, request_utils::Value> AsyncGetResponse(uint64_t session_id, uint64_t commandId) {
    // std::cout << "[AsyncGetResponse] Called with session_id=" << session_id 
    //           << ", commandId=" << commandId << std::endl;

    if (!benchmarkClient) {
        std::cout << "[ERROR] Creating new benchmark client" << std::endl;
        benchmarkClient = CreateBenchmarkClient();
    }

    // std::cout << "[AsyncGetResponse] Calling AwaitAsynchResponse..." << std::endl;
    std::tuple<request_utils::Value, uint64_t> result = benchmarkClient->AwaitAsynchResponse(session_id, commandId);

    request_utils::Value value = std::get<0>(result);
    uint64_t efd = std::get<1>(result);

    if (efd == static_cast<uint64_t>(-1)) {
        // Response is ready, return the Value object
        // std::cout << "[AsyncGetResponse] Response is ready, returning value." << std::endl;
        return {true, value};
    } else {
        // Response is not ready, return false and a Value containing the efd as a string
        // Wait for the eventfd to be signaled, then close it
        uint64_t val = 0;
        ssize_t read_bytes = read(efd, &val, sizeof(val));
        if (read_bytes != sizeof(val)) {
            std::cout << "[AsyncGetResponse] ERROR: Failed to read from efd " << efd << ", errno=" << errno << " (" << strerror(errno) << ")" << std::endl;
        }
        close(efd); // Release the fd so the OS can assign a new one next time
        // After waiting, try again to get the response
        std::tuple<request_utils::Value, uint64_t> retry_result = benchmarkClient->AwaitAsynchResponse(session_id, commandId);
        request_utils::Value retry_value = std::get<0>(retry_result);
        // Should now be ready
        return {true, retry_value};
    }
}

// Helper function to convert Value to Python objects
py::object value_to_python(const request_utils::Value& val) {
    // std::cout << "[value_to_python] Value type: " << static_cast<int>(val.type) << std::endl;
    switch (val.type) {
        case request_utils::ValueType::STRING:
            // std::cout << "[value_to_python] STRING: " << val.str << std::endl;
            return py::cast(val.str);
        case request_utils::ValueType::LIST:
            // std::cout << "[value_to_python] LIST: ";
            // for (const auto& item : val.list) std::cout << item << ", ";
            // std::cout << std::endl;
            return py::cast(val.list);
        case request_utils::ValueType::SET:
            // std::cout << "[value_to_python] SET: ";
            // for (const auto& item : val.set) std::cout << item << ", ";
            // std::cout << std::endl;
            return py::cast(val.set);
        case request_utils::ValueType::HASH:
            // std::cout << "[value_to_python] HASH: ";
            // for (const auto& kv : val.hash) std::cout << kv.first << ": " << kv.second << ", ";
            // std::cout << std::endl;
            return py::cast(val.hash);
        default:
            std::cout << "[value_to_python] NIL or unknown type" << std::endl;
            return py::none();
    }
}

// Cleanup function to properly manage global resources
void CleanupGlobalResources() {
    // std::cout << "[CleanupGlobalResources] Starting cleanup..." << std::endl;
    
    // Clean up static resources
    for (auto client : s_clients) {
        if (client) {
            std::cout << "[CleanupGlobalResources] Deleting client at " << client << std::endl;
            delete client;
        }
    }
    s_clients.clear();
    
    // Clear static pointers (they will be automatically deleted)
    s_keySelector.reset();
    s_partitioner.reset();
    s_transport.reset();
    
    // Clear the global benchmark client pointer
    benchmarkClient.reset();
    
    std::cout << "[CleanupGlobalResources] Cleanup completed" << std::endl;
}

// Function to start the transport (needed for clients to function)
bool StartTransport() {
    if (!s_transport) {
        std::cout << "[StartTransport] No transport available" << std::endl;
        return false;
    }
    
    std::cout << "[StartTransport] Starting transport..." << std::endl;
    try {
        // Start transport in a separate thread to avoid blocking
        std::thread transport_thread([]() {
            s_transport->Run();
        });
        transport_thread.detach();
        
        std::cout << "[StartTransport] Transport started successfully" << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cout << "[StartTransport] Exception starting transport: " << e.what() << std::endl;
        return false;
    }
}

// // Set string flags
// void SetStringFlag(const std::string& flag_name, const std::string& value) {
//     gflags::SetCommandLineOption(flag_name.c_str(), value.c_str());
// }

// // Set integer flags  
// void SetIntFlag(const std::string& flag_name, int64_t value) {
//     gflags::SetCommandLineOption(flag_name.c_str(), std::to_string(value).c_str());
// }

// // Set boolean flags
// void SetBoolFlag(const std::string& flag_name, bool value) {
//     gflags::SetCommandLineOption(flag_name.c_str(), value ? "true" : "false");
// }

// // Set double flags
// void SetDoubleFlag(const std::string& flag_name, double value) {
//     gflags::SetCommandLineOption(flag_name.c_str(), std::to_string(value).c_str());
// }


// Python-accessible function to call CustomInit() on the BenchmarkClient.
// This function will return the session_id.
uint64_t CustomInitSession() {
    // std::cout << "[CustomInitSession] Called" << std::endl;
    
    if (!benchmarkClient) {
        // std::cout << "[CustomInitSession] Creating new benchmark client" << std::endl;
        benchmarkClient = CreateBenchmarkClient();
        if (!benchmarkClient) {
            // std::cout << "[CustomInitSession] Failed to create benchmark client, returning 0" << std::endl;
            return 0; // Return invalid session ID
        }
        // std::cout << "[CustomInitSession] Benchmark client created successfully" << std::endl;
    } else {
        // std::cerr << "[CustomInitSession] Using existing benchmark client" << std::endl;
    }

    // std::cout << "[CustomInitSession] About to call CustomInit()..." << std::endl;
    // std::cout << "[CustomInitSession] Benchmark client pointer: " << benchmarkClient.get() << std::endl;
    
    try {
        // std::cout << "[CustomInitSession] Calling CustomInit()..." << std::endl;
        // Call CustomInit() to get the session_id
        uint64_t session_id = benchmarkClient->CustomInit();
        // std::cout << "[CustomInitSession] CustomInit completed successfully, session_id=" << session_id << std::endl;
        return session_id;
    } catch (const std::exception& e) {
        std::cerr << "[CustomInitSession] Exception during CustomInit(): " << e.what() << std::endl;
        return 0;
    } catch (...) {
        std::cerr << "[CustomInitSession] Unknown exception during CustomInit()" << std::endl;
        return 0;
    }
}

// Dummy functions for measuring Python-C++ communication overhead

// DummyFn: Returns immediately at C++ side
// Purpose: Measure pure Python-C++ FFI overhead without any I/O
uint64_t DummyFn() {
    return 42;
}

// DummyEFDFn1: Creates an eventfd and returns it immediately
// Purpose: Measure overhead of EFD creation + FFI
int DummyEFDFn1() {
    int efd = eventfd(0, EFD_CLOEXEC);
    if (efd == -1) {
        std::cerr << "[DummyEFDFn1] ERROR: Failed to create eventfd, errno=" << errno << std::endl;
        return -1;
    }
    return efd;
}

// DummyEFDFn2: Takes EFD, spawns thread that sleeps 5s then writes to EFD
// Purpose: Measure select() granularity and responsiveness
// Returns: true if thread spawned successfully, false otherwise
bool DummyEFDFn2(int efd) {
    if (efd < 0) {
        std::cerr << "[DummyEFDFn2] ERROR: Invalid efd=" << efd << std::endl;
        return false;
    }

    // Verify the efd is valid
    int flags = fcntl(efd, F_GETFD);
    if (flags == -1) {
        std::cerr << "[DummyEFDFn2] ERROR: efd " << efd << " is invalid, errno=" << errno << std::endl;
        return false;
    }

    // Spawn a detached thread that will sleep 5 seconds then write to the efd
    std::thread([efd]() {
        struct timespec start, before_sleep, after_sleep, before_write, after_write;
        clock_gettime(CLOCK_MONOTONIC, &start);

        std::cout << "[DummyEFDFn2_Thread] Started at "
                  << (start.tv_sec * 1000000000ULL + start.tv_nsec) << " ns" << std::endl;

        clock_gettime(CLOCK_MONOTONIC, &before_sleep);
        std::this_thread::sleep_for(std::chrono::seconds(5));
        clock_gettime(CLOCK_MONOTONIC, &after_sleep);

        uint64_t sleep_duration_ns = (after_sleep.tv_sec - before_sleep.tv_sec) * 1000000000ULL +
                                     (after_sleep.tv_nsec - before_sleep.tv_nsec);
        std::cout << "[DummyEFDFn2_Thread] Sleep duration: " << sleep_duration_ns << " ns" << std::endl;

        clock_gettime(CLOCK_MONOTONIC, &before_write);
        uint64_t val = 1;
        ssize_t written = write(efd, &val, sizeof(val));
        clock_gettime(CLOCK_MONOTONIC, &after_write);

        if (written != sizeof(val)) {
            std::cerr << "[DummyEFDFn2_Thread] ERROR: Failed to write to efd " << efd
                      << ", written=" << written << ", errno=" << errno << std::endl;
        } else {
            uint64_t write_duration_ns = (after_write.tv_sec - before_write.tv_sec) * 1000000000ULL +
                                         (after_write.tv_nsec - before_write.tv_nsec);
            std::cout << "[DummyEFDFn2_Thread] Write to efd completed in " << write_duration_ns << " ns" << std::endl;
            std::cout << "[DummyEFDFn2_Thread] Total time from start: "
                      << ((after_write.tv_sec - start.tv_sec) * 1000000000ULL +
                          (after_write.tv_nsec - start.tv_nsec)) << " ns" << std::endl;
        }
    }).detach();

    std::cout << "[DummyEFDFn2] Thread spawned successfully for efd=" << efd << std::endl;
    return true;
}

// Python binding module
PYBIND11_MODULE(redisstorepython, m) {
    m.doc() = "Redis Store Python Bindings";

    // Expose Operation enum from request_utils
    py::enum_<request_utils::Operation>(m, "Operation")
        .value("GET", request_utils::Operation::GET)
        .value("PUT", request_utils::Operation::PUT)
        .value("INCR", request_utils::Operation::INCR)
        .value("SET", request_utils::Operation::SET)
        .value("SADD", request_utils::Operation::SADD)
        .value("EXISTS", request_utils::Operation::EXISTS)
        .value("HMSET", request_utils::Operation::HMSET)
        .value("HSET", request_utils::Operation::HSET)
        .value("HMGET", request_utils::Operation::HMGET)
        .value("HGETALL", request_utils::Operation::HGETALL)
        .value("ZADD", request_utils::Operation::ZADD)
        .value("ZINCRBY", request_utils::Operation::ZINCRBY)
        .value("ZSCORE", request_utils::Operation::ZSCORE)
        .value("ZREVRANGE", request_utils::Operation::ZREVRANGE)
        .value("ZRANGE", request_utils::Operation::ZRANGE)
        .export_values();  // Optional: allows using Operation.GET, etc. directly

    // Expose ValueType enum
    py::enum_<request_utils::ValueType>(m, "ValueType")
        .value("STRING", request_utils::ValueType::STRING)
        .value("LIST", request_utils::ValueType::LIST)
        .value("SET", request_utils::ValueType::SET)
        .value("HASH", request_utils::ValueType::HASH);

    // Expose Command struct
    py::class_<request_utils::Command>(m, "Command")
        .def(py::init<>())
        .def_readwrite("op", &request_utils::Command::op)
        .def_readwrite("key", &request_utils::Command::key)
        .def_readwrite("value", &request_utils::Command::value)
        .def_readwrite("oldValue", &request_utils::Command::oldValue);

    // Expose Value class
    py::class_<request_utils::Value>(m, "Value")
        .def(py::init<>())
        .def(py::init<const std::string&>())
        .def(py::init<const std::vector<std::string>&>())
        .def(py::init<const std::unordered_set<std::string>&>())
        .def(py::init<const std::unordered_map<std::string, std::string>&>())
        .def_readwrite("type", &request_utils::Value::type)
        .def_readwrite("str", &request_utils::Value::str)
        .def_readwrite("list", &request_utils::Value::list)
        .def_readwrite("set", &request_utils::Value::set)
        .def_readwrite("hash", &request_utils::Value::hash)
        .def_static("NewString", &request_utils::Value::NewString)
        .def_static("NewList", &request_utils::Value::NewList)
        .def_static("NewSet", &request_utils::Value::NewSet)
        .def_static("NewHash", &request_utils::Value::NewHash)
        .def("is_nil", &request_utils::Value::isNil);

    // Expose BenchmarkClient creation
    m.def("create_benchmark_client", &CreateBenchmarkClient, "Create a BenchmarkClient instance");

    // m.def("set_string_flag", &SetStringFlag, "Set a string gflag value");
    // m.def("set_int_flag", &SetIntFlag, "Set an integer gflag value");
    // m.def("set_bool_flag", &SetBoolFlag, "Set a boolean gflag value");
    // m.def("set_double_flag", &SetDoubleFlag, "Set a double gflag value");

    // Wrapper for SendRequest to handle Python types
    // m.def("send_request", [](uint64_t session_id, request_utils::Operation op, int64_t keys, py::object new_values, py::object old_values) {
    //     // Convert Python objects to Value
    //     request_utils::Value newVal = python_to_value(new_values);
    //     request_utils::Value oldVal = python_to_value(old_values);

    //     // Call SendRequest
    //     return SendRequest(session_id, op, keys, newVal, oldVal);
    // }, py::arg("session_id"), py::arg("op"), py::arg("keys"), py::arg("new_values"), py::arg("old_values") = py::none());

    // Wrapper for AsyncSendRequest to handle Python types
    m.def("async_send_request", [](uint64_t session_id, request_utils::Operation op, uint64_t key, py::object new_values, py::object old_values) {
        request_utils::Value newVal = python_to_value(new_values);
        request_utils::Value oldVal = python_to_value(old_values);
        auto result = AsyncSendRequest(session_id, op, key, newVal, oldVal);
        return result;
    }, py::arg("session_id"), py::arg("op"), py::arg("key"), py::arg("new_values"), py::arg("old_values") = py::none());

    // Wrapper for AsyncGetResponse to handle Python types
    m.def("async_get_response", &AsyncGetResponse, py::arg("session_id"), py::arg("command_id"));

    // Expose only the CustomInitSession function.
    m.def("custom_init_session", &CustomInitSession,
          "Calls BenchmarkClient::CustomInit() and returns the session id.");
    
    // Expose cleanup function
    m.def("cleanup_global_resources", &CleanupGlobalResources,
          "Clean up all global resources (clients, transport, etc.)");
    
    // Expose transport start function
    m.def("start_transport", &StartTransport,
          "Start the transport layer (required for clients to function)");
    
    // Expose value_to_python function
    m.def("value_to_python", &value_to_python,
          "Convert a request_utils::Value to a Python object and print debug info");

    // Expose dummy functions for measuring communication overhead
    m.def("dummy_fn", &DummyFn,
          "Returns immediately at C++ side - measures pure Python-C++ FFI overhead");
    m.def("dummy_efd_fn1", &DummyEFDFn1,
          "Creates an eventfd and returns it immediately - measures EFD creation overhead");
    m.def("dummy_efd_fn2", &DummyEFDFn2,
          "Takes EFD, spawns thread that sleeps 5s then writes - measures select() granularity",
          py::arg("efd"));
}
