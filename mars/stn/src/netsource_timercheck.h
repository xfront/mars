// Tencent is pleased to support the open source community by making Mars available.
// Copyright (C) 2016 THL A29 Limited, a Tencent company. All rights reserved.

// Licensed under the MIT License (the "License"); you may not use this file except in 
// compliance with the License. You may obtain a copy of the License at
// http://opensource.org/licenses/MIT

// Unless required by applicable law or agreed to in writing, software distributed under the License is
// distributed on an "AS IS" basis, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
// either express or implied. See the License for the specific language governing permissions and
// limitations under the License.


/*
 * netsource_timercheck.h
 *
 *  Created on: 2013-5-16
 *      Author: yanguoyue
 */

#ifndef STN_SRC_NETSOURCE_TIMERCHECK_H_
#define STN_SRC_NETSOURCE_TIMERCHECK_H_

#include "boost/signals2.hpp"

#include "mars/comm/thread/thread.h"
#include "mars/baseevent/active_logic.h"
#include "mars/comm/socket/socketselect.h"
#include "mars/comm/messagequeue/message_queue.h"

#include "net_source.h"

class CommFrequencyLimit;

namespace mars {
namespace stn {

class LongLink;

class NetSourceTimerCheck {
public:
    NetSourceTimerCheck(NetSource *netSource, ActiveLogic &activeLogic, LongLink &longLink,
                        MessageQueue::MessageQueue_t msgQueueId);

    ~NetSourceTimerCheck();

    void CancelConnect();

public:
    boost::function<void()> TimeCheckSuccHook;

private:
    void __Run(const std::string &host);

    bool __TryConnnect(const std::string &host);

    void __OnActiveChanged(bool isActive);

    void __StartCheck();

    void __Check();

    void __StopCheck();

private:
    Thread mThread;
    boost::signals2::scoped_connection ActiveConn;
    NetSource *mNetSource;
    SocketBreaker mBreaker;
    SocketSelect mSelector;
    CommFrequencyLimit *mFrequencyLimit;
    LongLink &mLongLink;

    MessageQueue::ScopeRegister mAsyncReg;
    MessageQueue::MessagePost_t mAsyncPost;
    NetSource::DnsUtil mDnsUtil;
};

}
}


#endif // STN_SRC_NETSOURCE_TIMERCHECK_H_
