/***********************************************************************
 *
 * store/common/frontend/request_utils.h:
 *
 * Copyright 2025 Austin Li, Anja Kalaba
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
#ifndef REQUEST_UTILS_H
#define REQUEST_UTILS_H

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <boost/optional.hpp>
#include <iostream>
#include <algorithm>
#include <sstream>

namespace request_utils
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
    enum class AsynchOperationType
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
        AsynchOperationType op;
        std::string key;
        Value value;
        Value oldValue; // used for both CAS ops and ops that require more than one field
    };
} // namespace request_utils

#endif // REQUEST_UTILS_H
