#include <thread>
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
#include <thread>
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
#include <gflags/gflags.h>

// ... other includes ...

// Flags declared in benchmark.cc
DECLARE_uint64(client_id);
DECLARE_string(client_host);
DECLARE_string(replica_config_paths);
DECLARE_uint64(num_shards);
DECLARE_bool(ping_replicas);
DECLARE_string(net_config_path);
DECLARE_bool(debug_stats);
DECLARE_string(trans_protocol);
DECLARE_string(protocol_mode);
DECLARE_string(strong_consistency);
DECLARE_double(nb_time_alpha);
DECLARE_string(benchmark);
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
DECLARE_string(partitioner);
DECLARE_string(keys_path);
DECLARE_uint64(num_keys);

// New flags to declare
DECLARE_string(key_selector);
DECLARE_string(bench_mode);
DECLARE_double(zipf_coefficient);
DECLARE_double(client_arrival_rate);
DECLARE_double(client_think_time);
DECLARE_double(client_stay_probability);
DECLARE_double(mpl);
DECLARE_double(client_switch_probability);
DECLARE_uint64(num_ops_txn);
DECLARE_int32(warehouse_per_shard);
DECLARE_int32(clients_per_warehouse);
DECLARE_int32(remote_item_milli_p);
DECLARE_int32(tpcc_num_warehouses);
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

std::vector<Client *> clients;
std::vector<BenchmarkClient *> benchClients;
std::vector<std::thread *> threads;
Transport *tport;
Partitioner *part;
KeySelector *keySelector;

namespace py = pybind11;

// Global pointer to BenchmarkClient instance.
std::unique_ptr<BenchmarkClient> benchmarkClient = nullptr;

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

// Simplified initialization function
std::unique_ptr<BenchmarkClient> CreateBenchmarkClient() {
    Debug("Creating benchmark client");

    // Instantiate clients vector similar to benchmark.cc
    std::vector<Client *> clients;
    std::vector<BenchmarkClient *> benchClients;
    std::vector<std::thread *> threads;

    // Note: Transport and Partitioner are abstract classes, so you'll need to provide concrete implementations
    // This is a placeholder and needs to be replaced with actual initialization
    Transport *tport = nullptr; // Concrete transport implementation needed
    Partitioner *part = nullptr; // Concrete partitioner implementation needed

    // Note: UniformKeySelector might need to be replaced with a concrete implementation
    KeySelector *keySelector = nullptr; // Concrete key selector implementation needed

    // Determine bench_mode from FLAGS_bench_mode
    BenchmarkClientMode bench_mode = (FLAGS_bench_mode == "open") ? OPEN : CLOSED;

    // Create MicroClient with exact same parameters as benchmark initialization
    return std::make_unique<micro::MicroClient>(
        keySelector, clients, FLAGS_message_timeout, *tport, 0,
        bench_mode,
        FLAGS_client_switch_probability,
        FLAGS_client_arrival_rate, FLAGS_client_think_time, FLAGS_client_stay_probability,
        FLAGS_mpl,
        FLAGS_exp_duration, FLAGS_warmup_secs, FLAGS_cooldown_secs,
        FLAGS_tput_interval,
        FLAGS_abort_backoff, FLAGS_retry_aborted, FLAGS_max_backoff,
        FLAGS_max_attempts,
        FLAGS_client_fanout,
        FLAGS_client_issue_concurrent
        // FLAGS_transformed_app
    );
}

// AsyncSendRequest - Asynchronous version of SendRequest
std::pair<bool, request_utils::Value> AsyncSendRequest(uint64_t session_id, request_utils::Operation op, int64_t key, const request_utils::Value& newVal, const request_utils::Value& oldVal) {
    // Ensure BenchmarkClient is initialized
    if (!benchmarkClient) {
        benchmarkClient = CreateBenchmarkClient();
    }

    // Call SendAsynchRequest from BenchmarkClient
    std::tuple<bool, request_utils::Value> result = benchmarkClient->SendAsynchRequest(session_id, op, key, newVal, oldVal);
    
    return {std::get<0>(result), std::get<1>(result)};
}

// AsyncGetResponse - Retrieve the result of an asynchronous request
std::pair<bool, request_utils::Value> AsyncGetResponse(uint64_t session_id, uint64_t commandId) {
    // Ensure BenchmarkClient is initialized
    if (!benchmarkClient) {
        benchmarkClient = CreateBenchmarkClient();
    }

    // Call AwaitAsynchResponse from BenchmarkClient
    std::tuple<request_utils::Value, uint64_t> result = benchmarkClient->AwaitAsynchResponse(session_id, commandId);
    
    return {std::get<1>(result) == 0, std::get<0>(result)};
}

// Helper function to convert Value to Python objects
py::object value_to_python(const request_utils::Value& val) {
    switch (val.type) {
        case request_utils::ValueType::STRING:
            return py::cast(val.str);
        case request_utils::ValueType::LIST:
            return py::cast(val.list);
        case request_utils::ValueType::SET:
            return py::cast(val.set);
        case request_utils::ValueType::HASH:
            return py::cast(val.hash);
        default:
            return py::none();
    }
}

// Python-accessible function to call CustomInit() on the BenchmarkClient.
// This function will return the session_id.
uint64_t CustomInitSession() {
    // Ensure BenchmarkClient is initialized
    if (!benchmarkClient) {
        benchmarkClient = CreateBenchmarkClient();
    }

    // Call CustomInit() to get the session_id
    return benchmarkClient->CustomInit();
}

// Python binding module
PYBIND11_MODULE(redisstorepython, m) {
    m.doc() = "Redis Store Python Bindings";

    // Expose Operation enum from request_utils
    py::enum_<request_utils::Operation>(m, "Operation")
        .value("GET", request_utils::Operation::GET)
        .value("PUT", request_utils::Operation::PUT)
        // Add other operations as needed
        ;

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

    // Wrapper for AsyncSendRequest to handle Python types
    m.def("async_send_request", [](uint64_t session_id, request_utils::Operation op, int64_t keys, py::object new_values, py::object old_values) {
        // Convert Python objects to Value
        request_utils::Value newVal = python_to_value(new_values);
        request_utils::Value oldVal = python_to_value(old_values);

        // Call AsyncSendRequest
        return AsyncSendRequest(session_id, op, keys, newVal, oldVal);
    }, py::arg("session_id"), py::arg("op"), py::arg("keys"), py::arg("new_values"), py::arg("old_values") = py::none());

    // Wrapper for AsyncGetResponse to handle Python types
    m.def("async_get_response", &AsyncGetResponse, py::arg("session_id"), py::arg("command_id"));

    // Expose only the CustomInitSession function.
    m.def("custom_init_session", &CustomInitSession,
          "Calls BenchmarkClient::CustomInit() and returns the session id.");
}
