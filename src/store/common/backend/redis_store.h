#ifndef REDIS_STORE_H
#define REDIS_STORE_H

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <boost/optional.hpp>
#include <iostream>
#include <algorithm>
#include <sstream>

namespace redis
{

    enum class ValueType
    {
        STRING,
        LIST,
        SET,
        HASH
    };

    class Value
    {
    public:
        ValueType type;
        std::string str;
        std::vector<std::string> list;
        std::unordered_set<std::string> set;
        std::unordered_map<std::string, std::string> hash;

        Value() : type(ValueType::STRING), str("") {}
        Value(const std::string &s) : type(ValueType::STRING), str(s) {}
        Value(const std::vector<std::string> &l) : type(ValueType::LIST), list(l) {}
        Value(const std::unordered_set<std::string> &s) : type(ValueType::SET), set(s) {}
        Value(const std::unordered_map<std::string, std::string> &h) : type(ValueType::HASH), hash(h) {}

        static Value NewString(const std::string &s)
        {
            return Value(s);
        }
        static Value NewList(const std::vector<std::string> &l)
        {
            return Value(l);
        }
        static Value NewSet(const std::unordered_set<std::string> &s)
        {
            return Value(s);
        }
        static Value NewHash(const std::unordered_map<std::string, std::string> &h)
        {
            return Value(h);
        }

        bool isNil() const
        {
            return (type == ValueType::STRING && str.empty());
        }
    };

    static const Value NIL = Value::NewString("");

    // OPS
    enum class Operation
    {
        PUT,
        GET,
        INCR,
        SET,
        SADD,
        EXISTS,
        HMGET,
        HSET,
        HMSET,
        HGETALL,
        ZADD,
        ZINCRBY,
        ZSCORE,
        ZRANGE,
        ZREVRANGE
    };

    // A Command object carrying the operation, key, value and an optional extra field (oldValue).
    struct Command
    {
        Operation op;
        std::string key;
        Value value;
        Value oldValue; // used for both CAS ops and ops that require more than one field
    };

    // The main RedisStore class.
    class RedisStore
    {
    public:
        RedisStore() = default;
        virtual ~RedisStore();

        // Command execution
        // Mark as EXPORT or use appropriate compiler attribute if needed
        Value execute(const Command &cmd);

        // Declare binding.cpp as a friend to access execute
        friend std::pair<bool, Value> SendRequest(Operation op, int64_t keys, const Value &newVal, const Value &oldVal);

        // Redis Ops
        // Writes
        void put(const std::string &key, const Value &val);
        boost::optional<Value> get(const std::string &key);
        Value incr(const std::string &key);
        Value set(const std::string &key, const Value &val);
        Value sadd(const std::string &key, const std::string &member);
        bool exists(const std::string &key);

        // Hash reads
        Value hmget(const std::string &key, const std::string &field);
        Value hset(const std::string &key, const std::string &field, const std::string &val);
        Value hmset(const std::string &key, const std::unordered_map<std::string, std::string> &fields);
        Value hgetall(const std::string &key);

        // Sorted-set like operations
        Value zadd(const std::string &key, const std::string &member, const std::string &score);
        Value zincrby(const std::string &key, const std::string &increment, const std::string &member);
        Value zscore(const std::string &key, const std::string &value);
        Value zrange(const std::string &key, int start, int stop);
        Value zrevrange(const std::string &key, int start, int stop);

    private:
        std::unordered_map<std::string, Value> store;
    };
} // namespace redis

#endif // REDIS_STORE_H
