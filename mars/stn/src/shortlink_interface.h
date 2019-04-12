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
 * shortlink_interface.h
 *
 *  Created on: Jul 21, 2016
 *      Author: wutianqiang
 */

#ifndef SRC_SHORTLINK_INTERFACE_H_
#define SRC_SHORTLINK_INTERFACE_H_

#include "mars/comm/autobuffer.h"
#include "mars/stn/stn.h"
#include "mars/stn/task_profile.h"
#include "mars/comm/messagequeue/callback.h"

namespace mars {
namespace stn {

class ShortLinkInterface {
public:
    virtual ~ShortLinkInterface() {};

    virtual void SendRequest(AutoBuffer &bufferReq, AutoBuffer &bufExt) = 0;

    virtual ConnectProfile Profile() const { return ConnectProfile(); }

    CallBack<boost::function<void(int _line, ErrCmdType errType, int errCode, const std::string &ip,
                                  const std::string &host, uint16_t port)> > OnNetReport;

    CallBack<boost::function<void(ShortLinkInterface *worker, ErrCmdType errType, int status, AutoBuffer &body,
                                  AutoBuffer &extension, bool cancelRetry,
                                  ConnectProfile &connProfile)> > OnResponse;

    CallBack<boost::function<void(ShortLinkInterface *worker)> > OnSend;

    CallBack<boost::function<void(ShortLinkInterface *worker, unsigned int cachedSize,
                                  unsigned int totalSize)> > OnRecv;
};

}
}

#endif /* SRC_SHORTLINK_INTERFACE_H_ */
