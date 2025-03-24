#include "redisstore.h"

// Dispatcher function for commands.
Value RedisStore::execute(const Command& cmd) {
    switch(cmd.op) {
        case Operation::PUT:
            put(cmd.key, cmd.value);
            return cmd.value;  
        case Operation::GET: {
            auto val = get(cmd.key);
            return val.has_value() ? val.value() : NIL;
        }
        case Operation::INCR:
            return incr(cmd.key);
        case Operation::SET:
            return set(cmd.key, cmd.value);
        case Operation::SADD:
            return sadd(cmd.key, cmd.value.str);
        case Operation::EXISTS:
            return exists(cmd.key) ? Value::NewString("1") : Value::NewString("0");
        case Operation::HMGET:
            return hmget(cmd.key, cmd.value.list);
        case Operation::HSET:
            return hset(cmd.key, cmd.value.str, cmd.oldValue.str);
        case Operation::HGETALL:
            return hgetall(cmd.key);
        case Operation::ZADD:
            return zadd(cmd.key, cmd.value.str, cmd.oldValue.str);
        case Operation::ZINCRBY:
            return zincrby(cmd.key, cmd.value.str, cmd.oldValue.str);
        case Operation::ZSCORE:
            return zscore(cmd.key, cmd.oldValue.str);
        case Operation::ZREVRANGE: {
            int start = std::stoi(cmd.value.str);
            int stop  = std::stoi(cmd.oldValue.str);
            return zrevrange(cmd.key, start, stop);
        }
        default:
            std::cerr << "Operation not supported.\n";
            return NIL;
    }
}

void RedisStore::put(const std::string& key, const Value& val) {
    store[key] = val;
}

std::optional<Value> RedisStore::get(const std::string& key) {
    if (store.find(key) != store.end()) {
        return store[key];
    }
    // debug for get
    std::cout << "Key not found: " << key << "\n";
    return std::nullopt;
}

Value RedisStore::incr(const std::string& key) {
    auto it = store.find(key);
    if (it != store.end() && it->second.type == ValueType::STRING && !it->second.str.empty()) {
        try {
            // convert str -> int -> str
            int num = std::stoi(it->second.str);
            num++;
            it->second.str = std::to_string(num);
            return it->second;
        } catch (...) {
            it->second.str = "0";
            return it->second;
        }
    }
    // key does not exist, so initialize it
    store[key] = Value::NewString("1");
    return store[key];
}

Value RedisStore::set(const std::string& key, const Value& val) {
    put(key, val);
    // For SET, we return "OK" 
    return Value::NewString("OK");
}

Value RedisStore::sadd(const std::string& key, const std::string& member) {
    // If key does not exist or is not a set, create a new set.
    if (store.find(key) == store.end() || store[key].type != ValueType::SET) {
        store[key] = Value::NewSet({});
        store[key].type = ValueType::SET;
    }
    store[key].set.insert(member);
    return Value::NewString(std::to_string(store[key].set.size()));
}

bool RedisStore::exists(const std::string& key) {
    return store.find(key) != store.end();
}

// For HMGET, we return a hash (unordered_map) with the requested field-value pairs.
Value RedisStore::hmget(const std::string& key, const std::vector<std::string>& fields) {
    std::unordered_map<std::string, std::string> result;
    // if key doesn't exist return empty
    if (store.find(key) == store.end() || store[key].type != ValueType::HASH) {
        std::cout << "Key not found: " << key << "\n";
        return Value::NewHash(result); // Return empty hash if key doesn't exist or is not a hash
    }

    for (const auto& field : fields) {
        // If field doesn't exist, store an empty string
        result[field] = store[key].hash.count(field) ? store[key].hash[field] : "";
    }

    return Value::NewHash(result);
}

// HSET: set a field in the hash. Returns "1" if field did not exist, "0" if it did.
Value RedisStore::hset(const std::string& key, const std::string& field, const std::string& val) {
    if (store.find(key) == store.end() || store[key].type != ValueType::HASH) {
        store[key] = Value::NewHash({});
        store[key].type = ValueType::HASH;
    }
    bool isNew = (store[key].hash.find(field) == store[key].hash.end());
    store[key].hash[field] = val;
    return Value::NewString(isNew ? "1" : "0");
}

// HGETALL: return the whole hash (or an empty hash if key is missing or not a hash)
Value RedisStore::hgetall(const std::string& key) {
    if (store.find(key) != store.end() && store[key].type == ValueType::HASH) {
        return store[key];
    }
    return Value::NewHash({});
}

// ZADD: treat the sorted set as a hash mapping member->score.
Value RedisStore::zadd(const std::string& key, const std::string& member, const std::string& score) {
    if (store.find(key) == store.end() || store[key].type != ValueType::HASH) {
        store[key] = Value::NewHash({});
        store[key].type = ValueType::HASH;
    }
    store[key].hash[member] = score;
    return Value::NewString("1");
}

// ZINCRBY: increment the score (stored as string) for the given member.
Value RedisStore::zincrby(const std::string& key, const std::string& increment, const std::string& member) {
    //init if doesn't exist
    if (store.find(key) == store.end() || store[key].type != ValueType::HASH) {
        store[key] = Value::NewHash({});
        store[key].type = ValueType::HASH;
    }
    double inc = std::stod(increment);
    double current = 0.0;
    if (store[key].hash.find(member) != store[key].hash.end()) {
        current = std::stod(store[key].hash[member]);
    }
    current += inc;
    store[key].hash[member] = std::to_string(current);
    return Value::NewString(store[key].hash[member]);
}

// ZSCORE: return the score of a member.
Value RedisStore::zscore(const std::string& key, const std::string& member) {
    if (store.find(key) != store.end() && store[key].type == ValueType::HASH) {
        if (store[key].hash.find(member) != store[key].hash.end()) {
            return Value::NewString(store[key].hash[member]);
        }
    }
    return NIL;
}

// ZREVRANGE: get all members from the hash, sort them in descending order by score, and return a sublist.
Value RedisStore::zrevrange(const std::string& key, int start, int stop) {
    std::vector<std::pair<std::string, double>> members;
    if (store.find(key) != store.end() && store[key].type == ValueType::HASH) {
        for (const auto& [member, scoreStr] : store[key].hash) {
            double score = 0.0;
            try {
                score = std::stod(scoreStr);
            } catch (...) {
                score = 0.0;
            }
            members.emplace_back(member, score);
        }
    }
    // sort desc
    std::sort(members.begin(), members.end(), [](const auto& a, const auto& b) {
        return a.second > b.second;
    });
    std::vector<std::string> result;
    // Adjust indices (if negative, count from end)
    int n = members.size();
    if (start < 0) start = n + start;
    if (stop < 0) stop = n + stop;
    // Clamp bounds
    start = std::max(0, start);
    stop = std::min(n - 1, stop);
    if (start > stop) {
        return Value::NewList({});
    }
    for (int i = start; i <= stop; ++i) {
        result.push_back(members[i].first);
    }
    return Value::NewList(result);
}
