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
        std::cout << "inside execute" << std::endl;
        switch (cmd.op)
        {
        case Operation::PUT:
            std::cout << "Executing PUT for key: " << cmd.key << " with value type: " << static_cast<int>(cmd.value.type) << std::endl;
            put(cmd.key, cmd.value);
            return cmd.value;
        case Operation::GET:
        {
            auto val = get(cmd.key);
            return val.is_initialized() ? val.value() : NIL;
        }
        case Operation::INCR:
            return incr(cmd.key);
        case Operation::SET:
            return set(cmd.key, cmd.value);
        case Operation::SADD:
            return sadd(cmd.key, cmd.value.str);
        case Operation::EXISTS:
            return exists(cmd.key) ? Value::NewString("1") : Value::NewString("0");
        case Operation::HMSET:
            return hmset(cmd.key, cmd.value.hash);
        case Operation::HSET:
            return hset(cmd.key, cmd.value.str, cmd.oldValue.str);
        case Operation::HMGET:
            return hmget(cmd.key, cmd.value.str);
        case Operation::HGETALL:
            return hgetall(cmd.key);
        case Operation::ZADD:
            return zadd(cmd.key, cmd.value.str, cmd.oldValue.str);
        case Operation::ZINCRBY:
            return zincrby(cmd.key, cmd.value.str, cmd.oldValue.str);
        case Operation::ZSCORE:
            return zscore(cmd.key, cmd.value.str);
        case Operation::ZREVRANGE:
        {
            int start = std::stoi(cmd.value.str);
            int stop = std::stoi(cmd.oldValue.str);
            return zrevrange(cmd.key, start, stop);
        }
        case Operation::ZRANGE:
        {
            int start = std::stoi(cmd.value.str);
            int stop = std::stoi(cmd.oldValue.str);
            return zrange(cmd.key, start, stop);
        }
        default:
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
        std::cout << "Key not found: " << key << "\n";
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
        std::cout << "ZADD: Adding member " << member << " with score " << score << std::endl;
        store[key].hash[member] = score;

        return Value::NewString("1");
    }

    // ZINCRBY: increment the score (stored as string) for the given member.
    Value RedisStore::zincrby(const std::string &key, const std::string &increment, const std::string &member)
    {
        // init if doesn't exist
        if (store.find(key) == store.end() || store[key].type != ValueType::HASH)
        {
            store[key] = Value::NewHash({});
            store[key].type = ValueType::HASH;
        }
        std::cout << "Increment for member, " << member << ": " << increment << std::endl;
        double inc = std::stod(increment);
        double current = 0.0;
        if (store[key].hash.find(member) != store[key].hash.end())
        {
            current = std::stod(store[key].hash[member]);
        }
        current += inc;
        std::cout << "Incremented: " << current << std::endl;

        store[key].hash[member] = std::to_string(current);
        return Value::NewString(store[key].hash[member]);
    }

    // ZSCORE: return the score of a member.
    Value RedisStore::zscore(const std::string &key, const std::string &member)
    {
        std::cout << "ZSCORE: Searching for key " << key << ", member " << member << std::endl;

        if (store.find(key) != store.end() && store[key].type == ValueType::HASH)
        {
            std::cout << "ZSCORE: Key found, hash size: " << store[key].hash.size() << std::endl;

            if (store[key].hash.find(member) != store[key].hash.end())
            {
                std::string score = store[key].hash[member];
                std::cout << "ZSCORE: Found score for member " << member << ": " << score << std::endl;
                return Value::NewString(score);
            }
            else
            {
                std::cout << "ZSCORE: Member " << member << " not found in hash" << std::endl;
            }
        }
        else
        {
            std::cout << "ZSCORE: Key " << key << " not found or not a hash" << std::endl;
        }

        return NIL;
    }

    // ZRANGE: get all members from the hash, sort them in ascending order by score, and return a sublist with alternating members and scores.
    Value RedisStore::zrange(const std::string &key, int start, int stop)
    {

        if (store.find(key) == store.end() || store[key].type != ValueType::HASH)
        {
            for (const auto &entry : store)
            {
                const std::string &k = entry.first;
                const Value &v = entry.second;

                std::cout << "  Key: " << k << ", Type: " << static_cast<int>(v.type) << std::endl;
            }
            return Value::NewList({});
        }

        for (const auto &entry : store[key].hash)
        {
            const std::string &member = entry.first;
            const std::string &score = entry.second;

            std::cout << "ZRANGE: Member: " << member << ", Score: " << score << std::endl;
        }

        // Convert hash to vector of pairs for sorting
        std::vector<std::pair<std::string, double>> members_scores;
        for (const auto &entry : store[key].hash)
        {
            const std::string &member = entry.first;
            const std::string &score_str = entry.second;

            try
            {
                double score = std::stod(score_str);
                members_scores.emplace_back(member, score);
            }
            catch (...)
            {
                std::cout << "ZRANGE: Invalid score for member " << member << ": " << score_str << std::endl;
                continue;
            }
        }

        std::cout << "ZRANGE: Sorted members count = " << members_scores.size() << std::endl;

        // Sort in ascending order by score
        std::sort(members_scores.begin(), members_scores.end(),
                  [](const auto &a, const auto &b)
                  { return a.second < b.second; });

        // Adjust indices for negative indexing
        if (start < 0)
            start = std::max(0, static_cast<int>(members_scores.size()) + start);
        if (stop < 0)
            stop = std::max(0, static_cast<int>(members_scores.size()) + stop);

        // Clamp indices
        start = std::min(start, static_cast<int>(members_scores.size() - 1));
        stop = std::min(stop, static_cast<int>(members_scores.size() - 1));

        // Extract the range
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

            try
            {
                double score = std::stod(score_str);
                members_scores.emplace_back(member, score);
            }
            catch (...)
            {
                std::cout << "ZREVRANGE: Invalid score for member " << member << ": " << score_str << std::endl;
                continue;
            }
        }

        // Sort in descending order by score
        std::sort(members_scores.begin(), members_scores.end(),
                  [](const auto &a, const auto &b)
                  { return a.second > b.second; });

        // Adjust indices for negative indexing
        if (start < 0)
            start = std::max(0, static_cast<int>(members_scores.size()) + start);
        if (stop < 0)
            stop = std::max(0, static_cast<int>(members_scores.size()) + stop);

        // Clamp indices
        start = std::min(start, static_cast<int>(members_scores.size() - 1));
        stop = std::min(stop, static_cast<int>(members_scores.size() - 1));

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
