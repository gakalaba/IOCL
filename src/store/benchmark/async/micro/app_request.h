/***********************************************************************
 *
 * store/benchmark/async/retwis/get_timeline.h:
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
#ifndef BASIC_APP_REQUEST_H
#define BASIC_APP_REQUEST_H

#include <functional>

#include "store/common/frontend/async_apprequest.h"
#include "store/benchmark/async/common/key_selector.h"

namespace micro
{

    class BasicAppRequest : public AsyncAppRequest
    {
    public:
        BasicAppRequest(KeySelector *keySelector, int numKeys, std::mt19937 &rand);
        virtual ~BasicAppRequest();

    protected:
        Operation GetNextOperation(std::size_t op_index) override;

        inline size_t GetNumKeys() const { return keyIdxs.size(); };

        const std::string &GetTransactionType() override { return "basic_appreq"; };

        KeySelector *keySelector;

    private:
        std::vector<int> keyIdxs;
    };

} // namespace micro

#endif /* BASIC_APP_REQUEST_H */
