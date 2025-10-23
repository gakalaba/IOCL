#include "redis_store.h"

namespace redis
{

    // Destructor implementation
    RedisStore::~RedisStore()
    {
        // Clean up any resources if needed
        store.clear();
    }

    // Dispatcher function for commands.
    Value RedisStore::execute(const Command &cmd)
    {
        //std::cout << "inside execute" << std::endl;
        //std::cout << "Execution op=" << static_cast<int>(cmd.op)
                //   << " key=" << cmd.key
                //   << " value_type=" << static_cast<int>(cmd.value.type)
                //   << " value_str=" << cmd.value.str << std::endl;
        switch (cmd.op)
        {
        case Operation::PUT:
            //std::cout << "executing PUT" << std::endl;
            put(cmd.key, cmd.value);
            return cmd.value;
        case Operation::GET:
        {
            //std::cout << "executing GET" << std::endl;
            auto val = get(cmd.key);
            return val.is_initialized() ? val.value() : NIL;
        }
        case Operation::INCR:
            //std::cout << "executing INCR" << std::endl;
            return incr(cmd.key);
        case Operation::SET:
            //std::cout << "executing SET" << std::endl;
            return set(cmd.key, cmd.value);
        case Operation::SADD:
            //std::cout << "executing SADD" << std::endl;
            return sadd(cmd.key, cmd.value.str);
        case Operation::EXISTS:
            //std::cout << "executing EXISTS" << std::endl;
            return exists(cmd.key) ? Value::NewString("1") : Value::NewString("0");
        case Operation::HMSET:
            //std::cout << "executing HMSET" << std::endl;
            return hmset(cmd.key, cmd.value.hash);
        case Operation::HSET:
            //std::cout << "executing HSET" << std::endl;
            return hset(cmd.key, cmd.value.str, cmd.oldValue.str);
        case Operation::HMGET:
            //std::cout << "executing HMGET" << std::endl;
            return hmget(cmd.key, cmd.value.str);
        case Operation::HGETALL:
            //std::cout << "executing HGETALL" << std::endl;
            return hgetall(cmd.key);
        case Operation::ZADD:
            //std::cout << "executing ZADD" << std::endl;
            return zadd(cmd.key, cmd.value.str, cmd.oldValue.str);
        case Operation::ZINCRBY:
            //std::cout << "executing ZINCRBY" << std::endl;
            return zincrby(cmd.key, cmd.value.str, cmd.oldValue.str);
        case Operation::ZSCORE:
            //std::cout << "executing ZSCORE" << std::endl;
            return zscore(cmd.key, cmd.value.str);
        case Operation::ZREVRANGE:
        {
            //std::cout << "executing ZREVRANGE" << std::endl;
            int start = 1;  // default value
            int stop = 10;  // default value
            
            try {
                start = std::stoi(cmd.value.str);
            } catch (...) {
                //std::cout << "ZREVRANGE: Failed to convert start value, using default: " << start << std::endl;
            }
            
            try {
                stop = std::stoi(cmd.oldValue.str);
            } catch (...) {
                //std::cout << "ZREVRANGE: Failed to convert stop value, using default: " << stop << std::endl;
            }
            
            return zrevrange(cmd.key, start, stop);
        }
        case Operation::ZRANGE:
        {
            //std::cout << "executing ZRANGE" << std::endl;
            int start = 1;  // default value
            int stop = 10;  // default value
            
            try {
                start = std::stoi(cmd.value.str);
            } catch (...) {
                //std::cout << "ZRANGE: Failed to convert start value, using default: " << start << std::endl;
            }
            
            try {
                stop = std::stoi(cmd.oldValue.str);
            } catch (...) {
                //std::cout << "ZRANGE: Failed to convert stop value, using default: " << stop << std::endl;
            }
            
            return zrange(cmd.key, start, stop);
        }
        default:
            //std::cout << "cannot execute unsupported operation" << std::endl;
            std::cerr << "Operation not supported.\n";
            return NIL;
        }
    }

    void RedisStore::put(const std::string &key, const Value &val)
    {
        store[key] = val;
    }

    boost::optional<Value> RedisStore::get(const std::string &key)
    {
        if (store.find(key) != store.end())
        {
            return store[key];
        }
        // debug for get
        //std::cout << "Key not found: " << key << "\n";
        return boost::none;
    }

    Value RedisStore::incr(const std::string &key)
    {
        auto it = store.find(key);
        if (it != store.end() && it->second.type == ValueType::STRING && !it->second.str.empty())
        {
            try
            {
                // convert str -> int -> str
                int num = std::stoi(it->second.str);
                num++;
                it->second.str = std::to_string(num);
                return it->second;
            }
            catch (...)
            {
                it->second.str = "0";
                return it->second;
            }
        }
        // key does not exist, so initialize it
        store[key] = Value::NewString("1");
        return store[key];
    }

    Value RedisStore::set(const std::string &key, const Value &val)
    {
        put(key, val);
        // For SET, we return "OK"
        return Value::NewString("OK");
    }

    Value RedisStore::sadd(const std::string &key, const std::string &member)
    {
        // If key does not exist or is not a set, create a new set.
        if (store.find(key) == store.end() || store[key].type != ValueType::SET)
        {
            store[key] = Value::NewSet({});
            store[key].type = ValueType::SET;
        }
        store[key].set.insert(member);
        return Value::NewString(std::to_string(store[key].set.size()));
    }

    bool RedisStore::exists(const std::string &key)
    {
        return store.find(key) != store.end();
    }

    // For HMGET, we return a list of values for the requested fields.
    Value RedisStore::hmget(const std::string &key, const std::string &field1)
    {
        std::vector<std::string> result;

        // Check if key exists and is a hash
        if (store.find(key) == store.end() || store[key].type != ValueType::HASH)
        {
            return Value::NewList(result);
        }

        // Get first field, default to empty string if not found
        std::string value1 = store[key].hash.count(field1) ? store[key].hash[field1] : "";
        result.push_back(value1);

        return Value::NewList(result);
    }

    Value RedisStore::hset(const std::string &key, const std::string &field, const std::string &val)
    {
        if (store.find(key) == store.end() || store[key].type != ValueType::HASH)
        {
            store[key] = Value::NewHash({});
            store[key].type = ValueType::HASH;
        }
        bool isNew = (store[key].hash.find(field) == store[key].hash.end());
        store[key].hash[field] = val;
        return Value::NewString(isNew ? "1" : "0");
    }

    // HGETALL: return the whole hash (or an empty hash if key is missing or not a hash)
    Value RedisStore::hgetall(const std::string &key)
    {
        if (store.find(key) != store.end() && store[key].type == ValueType::HASH)
        {
            return store[key];
        }
        return Value::NewHash({});
    }

    // HMSET: set an entire hash map at a key
    Value RedisStore::hmset(const std::string &key, const std::unordered_map<std::string, std::string> &fields)
    {
        if (store.find(key) == store.end() || store[key].type != ValueType::HASH)
        {
            store[key] = Value::NewHash({});
            store[key].type = ValueType::HASH;
        }

        int newFieldCount = 0;
        for (const auto &entry : fields)
        {
            const std::string &field = entry.first;
            const std::string &val = entry.second;

            bool isNew = (store[key].hash.find(field) == store[key].hash.end());
            store[key].hash[field] = val;
            if (isNew)
                newFieldCount++;
        }

        return Value::NewString(std::to_string(newFieldCount));
    }

    // ZADD: treat the sorted set as a hash mapping member->score.
    Value RedisStore::zadd(const std::string &key, const std::string &member, const std::string &score)
    {
        if (store.find(key) == store.end() || store[key].type != ValueType::HASH)
        {
            store[key] = Value::NewHash({});
            store[key].type = ValueType::HASH;
        }
        //std::cout << "ZADD: Adding member " << member << " with score " << score << std::endl;
        store[key].hash[member] = score;

        return Value::NewString("1");
    }

    // ZINCRBY: increment the score (stored as string) for the given member.
    Value RedisStore::zincrby(const std::string &key, const std::string &increment, const std::string &member)
    {
        if (store.find(key) == store.end() || store[key].type != ValueType::HASH)
        {
            store[key] = Value::NewHash({});
            store[key].type = ValueType::HASH;
        }

        double inc = 0.0;
        try { inc = std::stod(increment); } catch (...) { inc = 0.0; }

        double current = 0.0;
        if (store[key].hash.find(member) != store[key].hash.end())
        {
            try { current = std::stod(store[key].hash[member]); } catch (...) { current = 0.0; }
        }

        current += inc;
        store[key].hash[member] = std::to_string(current);
        return Value::NewString(store[key].hash[member]);
    }

    // ZSCORE: return the score of a member.
    Value RedisStore::zscore(const std::string &key, const std::string &member)
    {
        //std::cout << "ZSCORE: Searching for key " << key << ", member " << member << std::endl;

        if (store.find(key) != store.end() && store[key].type == ValueType::HASH)
        {
            //std::cout << "ZSCORE: Key found, hash size: " << store[key].hash.size() << std::endl;

            if (store[key].hash.find(member) != store[key].hash.end())
            {
                std::string score = store[key].hash[member];
                //std::cout << "ZSCORE: Found score for member " << member << ": " << score << std::endl;
                return Value::NewString(score);
            }
            else
            {
                //std::cout << "ZSCORE: Member " << member << " not found in hash" << std::endl;
            }
        }
        else
        {
            //std::cout << "ZSCORE: Key " << key << " not found or not a hash" << std::endl;
        }

        return NIL;
    }

    // ZRANGE: get all members from the hash, sort them in ascending order by score, and return a sublist with alternating members and scores.
    Value RedisStore::zrange(const std::string &key, int start, int stop)
    {
        if (store.find(key) == store.end() || store[key].type != ValueType::HASH)
            return Value::NewList({});

        // Convert hash to vector of pairs for sorting
        std::vector<std::pair<std::string, double>> members_scores;
        for (const auto &entry : store[key].hash)
        {
            try
            {
                double score = std::stod(entry.second);
                members_scores.emplace_back(entry.first, score);
            }
            catch (...) { continue; }
        }

        std::sort(members_scores.begin(), members_scores.end(),
                [](const auto &a, const auto &b) { return a.second < b.second; });

        int size = static_cast<int>(members_scores.size());
        if (size == 0) return Value::NewList({});

        // Handle negative indices
        if (start < 0) start = std::max(0, size + start);
        if (stop < 0) stop = std::max(0, size + stop);

        // Clamp indices
        start = std::max(0, std::min(start, size - 1));
        stop = std::max(0, std::min(stop, size - 1));
        if (start > stop) return Value::NewList({});

        std::vector<std::string> result;
        for (int i = start; i <= stop; ++i)
        {
            result.push_back(members_scores[i].first);
            result.push_back(std::to_string(members_scores[i].second));
        }

        return Value::NewList(result);
    }

    // ZREVRANGE: get all members from the hash, sort them in descending order by score, and return a sublist.
    Value RedisStore::zrevrange(const std::string &key, int start, int stop)
    {
        if (store.find(key) == store.end() || store[key].type != ValueType::HASH)
        {
            return Value::NewList({});
        }

        // Convert hash to vector of pairs for sorting
        std::vector<std::pair<std::string, double>> members_scores;
        for (const auto &entry : store[key].hash)
        {
            const std::string &member = entry.first;
            const std::string &score_str = entry.second;
            try {
                double score = std::stod(score_str);
                members_scores.emplace_back(member, score);
            } catch (...) {
                //std::cout << "ZREVRANGE: Invalid score for member " << member << ": " << score_str << std::endl;
                continue;
            }
        }

        // Sort in descending order by score
        std::sort(members_scores.begin(), members_scores.end(),
                  [](const auto &a, const auto &b) { return a.second > b.second; });

        int n = static_cast<int>(members_scores.size());
        if (n == 0) return Value::NewList({});

        // Adjust indices for negative indexing
        if (start < 0) start = n + start;
        if (stop < 0) stop = n + stop;

        // Clamp indices to valid range
        start = std::max(0, std::min(start, n - 1));
        stop = std::max(0, std::min(stop, n - 1));

        // If start > stop, return empty list
        if (start > stop) return Value::NewList({});

        // Extract the range
        std::vector<std::string> result;
        for (int i = start; i <= stop; ++i)
        {
            result.push_back(members_scores[i].first);
            result.push_back(std::to_string(members_scores[i].second));
        }

        return Value::NewList(result);
    }

} // namespace redis
