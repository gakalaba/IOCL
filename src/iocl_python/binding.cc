#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/functional.h>
#include "store/benchmark/async/bench_client.h"
#include "store/benchmark/async/micro/micro_client.h"
#include "store/common/backend/redis_store.h"

namespace py = pybind11;

// Global pointer to BenchmarkClient instance.
std::unique_ptr<BenchmarkClient> benchmarkClient = nullptr;

// Helper function to convert Python objects to Value
Value python_to_value(const py::object& obj) {
    if (py::isinstance<py::str>(obj)) {
        return Value::NewString(obj.cast<std::string>());
    }
    else if (py::isinstance<py::list>(obj)) {
        return Value::NewList(obj.cast<std::vector<std::string>>());
    }
    else if (py::isinstance<py::set>(obj)) {
        return Value::NewSet(obj.cast<std::unordered_set<std::string>>());
    }
    else if (py::isinstance<py::dict>(obj)) {
        return Value::NewHash(obj.cast<std::unordered_map<std::string, std::string>>());
    }
    return NIL;
}

// Simplified initialization function
std::unique_ptr<BenchmarkClient> CreateBenchmarkClient() {
    // Placeholder implementation - replace with actual initialization
    KeySelector keySelector = new UniformKeySelector(keys);
    std::vector<std::shared_ptr<Client>> clients = /* your client initialization */;
    Transport* tport = /* your transport initialization */;
    
    return std::make_unique<micro::MicroClient>(
        keySelector,
        clients,
        FLAGS_message_timeout,
        *tport,
        seed,
        bench_mode,
        FLAGS_client_switch_probability,
        FLAGS_client_arrival_rate,
        FLAGS_client_think_time,
        FLAGS_client_stay_probability,
        FLAGS_mpl,
        FLAGS_exp_duration,
        FLAGS_warmup_secs,
        FLAGS_cooldown_secs,
        FLAGS_tput_interval,
        FLAGS_abort_backoff,
        FLAGS_retry_aborted,
        FLAGS_max_backoff,
        FLAGS_max_attempts,
        FLAGS_client_fanout,
        FLAGS_client_issue_concurrent
    );
}

// AsyncSendRequest - Asynchronous version of SendRequest
std::pair<bool, int> AsyncSendRequest(uint64_t session_id, Operation op, int64_t key, const Value& newVal, const Value& oldVal) {
    try {
        auto result = benchmarkClient->SendAsynchRequest(session_id, op, key, newVal, oldVal);
        bool success = std::get<0>(result);
        if (!success) {
            return {false, -1};
        }
        
        Value cmdIdValue = std::get<1>(result);
        int requestId = std::stoi(cmdIdValue.toString());
        
        return {true, requestId};
    } catch (const std::exception& e) {
        std::cerr << "Error in AsyncSendRequest: " << e.what() << std::endl;
        return {false, -1};
    }
}


// AsyncGetResponse - Retrieve the result of an asynchronous request
std::pair<bool, Value> AsyncGetResponse(uint64_t session_id, uint64_t commandId) {
    try {
        std::tuple<Value, uint64_t> result_tuple = benchmarkClient->AwaitAsynchResponse(session_id, commandId);
        Value result = std::get<0>(result_tuple);

        return {true, result};
    } catch (const std::exception& e) {
        std::cerr << "Error in AsyncGetResponse: " << e.what() << std::endl;
        return {false, NIL};
    }
}


// Helper function to convert Value to Python objects
py::object value_to_python(const Value& val) {
    switch (val.type) {
        case ValueType::STRING:
            return py::cast(val.str);
        case ValueType::LIST:
            return py::cast(val.list);
        case ValueType::SET:
            return py::cast(val.set);
        case ValueType::HASH:
            return py::cast(val.hash);
        default:
            return py::none();
    }
}

// Python-accessible function to call CustomInit() on the BenchmarkClient.
// This function will return the session_id.
uint64_t CustomInitSession() {
    if (!benchmarkClient) {
        // If the client wasn't already created, initialize it.
        benchmarkClient = CreateBenchmarkClient();
    }
    return benchmarkClient->CustomInit();
}

PYBIND11_MODULE(redisstorepython, m) {
    m.doc() = "Redis Store Python Bindings";

    // Define the Operation enum
    py::enum_<Operation>(m, "Operation")
        .value("PUT", Operation::PUT)
        .value("GET", Operation::GET)
        .value("INCR", Operation::INCR)
        .value("SET", Operation::SET)
        .value("SADD", Operation::SADD)
        .value("EXISTS", Operation::EXISTS)
        .value("HMGET", Operation::HMGET)
        .value("HSET", Operation::HSET)
        .value("HMSET", Operation::HMSET)
        .value("HGETALL", Operation::HGETALL)
        .value("ZADD", Operation::ZADD)
        .value("ZINCRBY", Operation::ZINCRBY)
        .value("ZSCORE", Operation::ZSCORE)
        .value("ZREVRANGE", Operation::ZREVRANGE);

    // Define the ValueType enum
    py::enum_<ValueType>(m, "ValueType")
        .value("STRING", ValueType::STRING)
        .value("LIST", ValueType::LIST)
        .value("SET", ValueType::SET)
        .value("HASH", ValueType::HASH);
    
    // Define the Value class
    py::class_<Value>(m, "Value")
        .def(py::init<>())
        .def(py::init<const std::string&>())
        .def(py::init<const std::vector<std::string>&>())
        .def(py::init<const std::unordered_set<std::string>&>())
        .def(py::init<const std::unordered_map<std::string, std::string>&>())
        .def_readwrite("type", &Value::type)
        .def_readwrite("str", &Value::str)
        .def_readwrite("list", &Value::list)
        .def_readwrite("set", &Value::set)
        .def_readwrite("hash", &Value::hash)
        .def_static("NewString", &Value::NewString)
        .def_static("NewList", &Value::NewList)
        .def_static("NewSet", &Value::NewSet)
        .def_static("NewHash", &Value::NewHash)
        .def("is_nil", &Value::isNil);

    // Define the Command struct
    py::class_<Command>(m, "Command")
        .def(py::init<>())
        .def_readwrite("op", &Command::op)
        .def_readwrite("key", &Command::key)
        .def_readwrite("value", &Command::value)
        .def_readwrite("oldValue", &Command::oldValue);

    // Define the RedisStore class
    py::class_<RedisStore>(m, "RedisStore")
        .def(py::init<>())
        .def("execute", &RedisStore::execute)
        .def("__del__", [](RedisStore& self) { self.~RedisStore(); });

    // Expose Value conversion function
    m.def("python_to_value", &python_to_value, "Convert Python object to Value");

    // Expose BenchmarkClient creation
    m.def("create_benchmark_client", &CreateBenchmarkClient, "Create a BenchmarkClient instance");

    // Wrapper for SendRequest to handle Python types
    // m.def("send_request", [](Operation op, int64_t keys, py::object new_values, py::object old_values) {
    //     // Debug print input parameters
    //     Value newVal = python_to_value(new_values);
    //     Value oldVal = python_to_value(old_values);
        
    //     bool success;
    //     Value result;
    //     std::tie(success, result) = SendRequest(op, keys, newVal, oldVal);
        
    //     return py::make_tuple(success, value_to_python(result));
    // }, py::arg("op"), py::arg("keys"), py::arg("new_values"), py::arg("old_values") = py::none());
    
    // Wrapper for AsyncSendRequest to handle Python types
    m.def("async_send_request", [](uint64_t session_id, Operation op, int64_t keys, py::object new_values, py::object old_values) {
        // Debug print input parameters
        Value newVal = python_to_value(new_values);
        Value oldVal = python_to_value(old_values);
        
        bool success;
        int requestId;
        std::tie(success, requestId) = AsyncSendRequest(session_id, op, keys, newVal, oldVal);
        
        return py::make_tuple(success, requestId);
    }, py::arg("session_id"), py::arg("op"), py::arg("keys"), py::arg("new_values"), py::arg("old_values") = py::none());

    // Wrapper for AsyncGetResponse to handle Python types
    m.def("async_get_response", [](uint64_t session_id, uint64_t commandId) {
        bool success;
        Value result;
        std::tie(success, result) = AsyncGetResponse(session_id, commandId);

        return py::make_tuple(success, value_to_python(result));
    }, py::arg("session_id"), py::arg("command_id"));

    // Expose only the CustomInitSession function.
    m.def("custom_init_session", &CustomInitSession,
          "Calls BenchmarkClient::CustomInit() and returns the session id.");
}