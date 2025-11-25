/***********************************************************************
 *
 * store/benchmark/async/micro/app_request.h:
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
        BasicAppRequest(KeySelector *keySelector, int fanout, std::mt19937 &rand, uint32_t read_percentage, bool WOReplacement);
        virtual ~BasicAppRequest();

    protected:
        Operation GetNextOperation(std::size_t op_index) override;

        inline const std::string &GetKey(int i) const
        {
            return keySelector->GetKey(keyIdxs[i]);
        }

        inline size_t GetNumKeys() const { return keyIdxs.size(); };

        virtual const std::string &GetTransactionType() override;
        virtual const int Fanout() override;

        KeySelector *keySelector;

    private:
        std::vector<int> keyIdxs;
        std::string ttype_;
        int fanout_;
        uint32_t read_percentage_;
        std::unordered_set<int> seenKeys_;
        bool WOReplacement_;
    };

} // namespace micro

#endif /* BASIC_APP_REQUEST_H */
