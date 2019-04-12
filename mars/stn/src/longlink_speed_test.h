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
 * longlink_speed_test.h
 *
 *  Created on: 2013-5-13
 *      Author: yanguoyue
 */

#ifndef STN_SRC_LONGLINK_SPEED_TEST_H_
#define STN_SRC_LONGLINK_SPEED_TEST_H_

#include <string>
#include <vector>

#include "boost/shared_ptr.hpp"

#include "mars/comm/autobuffer.h"
#include "mars/comm/socket/socketselect.h"
#include "mars/comm/socket/unix_socket.h"

#include "net_source.h"

enum ELongLinkSpeedTestState {
    kLongLinkSpeedTestConnecting,
    kLongLinkSpeedTestReq,
    kLongLinkSpeedTestResp,
    kLongLinkSpeedTestOOB,
    kLongLinkSpeedTestSuc,
    kLongLinkSpeedTestFail,
};

namespace mars {
namespace stn {

class LongLinkSpeedTestItem {
public:
    LongLinkSpeedTestItem(const std::string &ip, uint16_t port);

    ~LongLinkSpeedTestItem();

    void HandleFDISSet(SocketSelect &_sel);

    void HandleSetFD(SocketSelect &_sel);

    int GetSocket();

    std::string GetIP();

    unsigned int GetPort();

    unsigned long GetConnectTime();

    int GetState();

    void CloseSocket();

private:
    int __HandleSpeedTestReq();

    int __HandleSpeedTestResp();

private:
    std::string mIP;
    unsigned int mPort;
    SOCKET mSocket;
    int mState;

    uint64_t mBeforeConnectTime;
    uint64_t mAfterConnectTime;

    AutoBuffer mReqBuf;
    AutoBuffer mRspBuf;
};

class LongLinkSpeedTest {
public:
    LongLinkSpeedTest(const boost::shared_ptr<NetSource> &netSource);

    ~LongLinkSpeedTest();

    bool GetFastestSocket(int &_fdSocket, std::string &_strIp, unsigned int &port, IPSourceType &type,
                          unsigned long &_connectMillSec);

    boost::shared_ptr<NetSource> GetNetSource();

private:
    boost::shared_ptr<NetSource> mNetSource;
    SocketBreaker mBreaker;
    SocketSelect mSelector;
};

}
}


#endif // STN_SRC_LONGLINK_SPEED_TEST_H_
