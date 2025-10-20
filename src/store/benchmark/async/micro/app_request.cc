/***********************************************************************
 *
 * store/benchmark/async/micro/app_request.cc:
 *
 * Copyright 2025 Anja Kalaba
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
#include "store/benchmark/async/micro/app_request.h"

#include <unordered_set>

namespace micro
{

    BasicAppRequest::BasicAppRequest(KeySelector *keySelector, int fanout, std::mt19937 &rand,
    uint32_t read_percentage)
        : AsyncAppRequest(),
          keySelector(keySelector),
          ttype_{"basic_appreq"},
          fanout_{fanout},
          read_percentage_{read_percentage}
    {
        for (int i = 0; i < fanout; ++i)
        {
            keyIdxs.push_back(keySelector->GetKey(rand));
        }
    }

    BasicAppRequest::~BasicAppRequest()
    {
    }

    Operation BasicAppRequest::GetNextOperation(std::size_t op_index)
    {
        size_t fanout = GetNumKeys();
        Debug("BASIC_APP_REQUEST with %lu subops: currently on op_index = %lu; read_percentage = %d", fanout, op_index, read_percentage_);

        if (0 <= op_index && op_index < fanout) {
            srand(time(0));
            if ((rand() % 100) < read_percentage_)
            {
                Debug("sending Get");
                return Get(GetKey(op_index - 1));
            }
            else
            {
                Debug("Sending Put");
                return Put(GetKey(op_index - 1), GetKey(op_index - 1));
            }   
        }
        else {
            PPanic("Not good!!! Sending operation out of bounds of app request!");
        }
    }

    const std::string &BasicAppRequest::GetTransactionType()
    {
        return ttype_;
    }

    const int BasicAppRequest::Fanout()
    {
        return fanout_;
    }

} // namespace micro
