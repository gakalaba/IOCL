/***********************************************************************
 *
 * store/common/backend/store.h:
 *
 * Copyright 2022 Jeffrey Helt, Matthew Burke, Amit Levy, Wyatt Lloyd
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 **********************************************************************/
/***********************************************************************
 *
 * store/common/backend/store.h:
 *
 * Copyright 2022 Jeffrey Helt, Matthew Burke, Amit Levy, Wyatt Lloyd
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 **********************************************************************/
#ifndef REDIS_STORE_H
#define REDIS_STORE_H

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <iostream>
#include <algorithm>
#include <sstream>

enum class ValueType {
    STRING,
    LIST,
    SET,
    HASH
};

// A minimal Value class holding one of the supported types.
class Value {
public:
    ValueType type;
    std::string str;
    std::vector<std::string> list;
    std::unordered_set<std::string> set;
    std::unordered_map<std::string, std::string> hash;

    Value() : type(ValueType::STRING), str("") { }
    Value(const std::string& s) : type(ValueType::STRING), str(s) { }
    Value(const std::vector<std::string>& l) : type(ValueType::LIST), list(l) { }
    Value(const std::unordered_set<std::string>& s) : type(ValueType::SET), set(s) { }
    Value(const std::unordered_map<std::string, std::string>& h) : type(ValueType::HASH), hash(h) { }

    // init methods for each type
    static Value NewString(const std::string& s) {
        return Value(s);
    }
    static Value NewList(const std::vector<std::string>& l) {
        return Value(l);
    }
    static Value NewSet(const std::unordered_set<std::string>& s) {
        return Value(s);
    }
    static Value NewHash(const std::unordered_map<std::string, std::string>& h) {
        return Value(h);
    }

    bool isNil() const {
        return (type == ValueType::STRING && str.empty());
    }
};

static const Value NIL = Value::NewString("");

// OPS
enum class Operation {
    PUT,
    GET,
    INCR,
    SET,
    SADD,
    EXISTS,
    HMGET,
    HSET,
    HGETALL,
    // Impemented via hash map for member->score
    ZADD,
    ZINCRBY,
    ZSCORE,
    ZREVRANGE
};

// A Command object carrying the operation, key, value and an optional extra field (oldValue).
struct Command {
    Operation op;
    std::string key;
    Value value;      
    Value oldValue;   // used for both CAS ops and ops that require more than one field
};

// The main RedisStore class.
class RedisStore {
public:
    RedisStore() = default;

    // Command execution
    Value execute(const Command& cmd);

    // Redis Ops
    // Writes
    void put(const std::string& key, const Value& val);
    std::optional<Value> get(const std::string& key);
    Value incr(const std::string& key);
    Value set(const std::string& key, const Value& val); 
    Value sadd(const std::string& key, const std::string& member);
    bool exists(const std::string& key);

    // Hash reads
    Value hmget(const std::string& key, const std::vector<std::string>& fields);
    Value hset(const std::string& key, const std::string& field, const std::string& val);
    Value hgetall(const std::string& key);

    // Sorted-set like operations
    Value zadd(const std::string& key, const std::string& member, const std::string& score);
    Value zincrby(const std::string& key, const std::string& increment, const std::string& member);
    Value zscore(const std::string& key, const std::string& member);
    Value zrevrange(const std::string& key, int start, int stop);

private:
    std::unordered_map<std::string, Value> store;
};

#endif // REDIS_STORE_H
