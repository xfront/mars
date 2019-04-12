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
 * shortlink.h
 *
 *  Created on: 2012-8-22
 *      Author: zhouzhijie
 */

#ifndef STN_SRC_SHORTLINK_H_
#define STN_SRC_SHORTLINK_H_

#include <string>
#include <map>
#include <vector>

#include "boost/signals2.hpp"
#include "boost/function.hpp"

#include "mars/comm/thread/thread.h"
#include "mars/comm/autobuffer.h"
#include "mars/comm/http.h"
#include "mars/comm/socket/socketselect.h"
#include "mars/comm/messagequeue/message_queue.h"
#include "mars/stn/stn.h"
#include "mars/stn/task_profile.h"
#include "mars/comm/socket/socket_address.h"

#include "net_source.h"
#include "shortlink_interface.h"

namespace mars {
namespace stn {

class shortlink_tracker;

class ShortLink : public ShortLinkInterface {
public:
    ShortLink(MessageQueue::MessageQueue_t msgQueueId, NetSource &netSource, const Task &task, bool useProxy);

    virtual ~ShortLink();

    ConnectProfile Profile() const { return mConnProfile; }

    void FillOutterIPAddr(const std::vector<IPPortItem> &outAddrs);

protected:
    virtual void SendRequest(AutoBuffer &bufferReq, AutoBuffer &taskExtend);

    virtual void __Run();

    virtual SOCKET __RunConnect(ConnectProfile &connProfile);

    virtual void __RunReadWrite(SOCKET sock, int &errType, int &errCode, ConnectProfile &connProfile);

    void __CancelAndWaitWorkerThread();

    void __UpdateProfile(const ConnectProfile &connProfile);

    void __RunResponseError(ErrCmdType type, int errCode, ConnectProfile &connProfile, bool report = true);

    void __OnResponse(ErrCmdType errType, int status, AutoBuffer &body, AutoBuffer &extension,
                      ConnectProfile &connProfile, bool report = true);

protected:
    MessageQueue::ScopeRegister mAsyncReg;
    NetSource &mNetSource;
    Task mTask;
    Thread mThread;

    SocketBreaker mBreaker;
    ConnectProfile mConnProfile;
    NetSource::DnsUtil mDnsUtil;
    const bool mUseProxy;
    AutoBuffer mSendBody;
    AutoBuffer mSendExtend;

    std::vector<IPPortItem> mOutterAddrs;

    boost::scoped_ptr<shortlink_tracker> mTracker;
};

}
}

#endif // STN_SRC_MMSHORTLINK_H_
