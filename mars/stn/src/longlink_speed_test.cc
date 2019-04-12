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
 * longlink_speed_test.cc
 *
 *  Created on: 2013-5-13
 *      Author: yanguoyue
 */

#include "longlink_speed_test.h"

#include "mars/comm/xlogger/xlogger.h"
#include "mars/comm/socket/unix_socket.h"
#include "mars/comm/socket/socket_address.h"
#include "mars/comm/autobuffer.h"
#include "mars/comm/time_utils.h"
#include "mars/comm/platform_comm.h"
#include "mars/stn/stn.h"
#include "mars/stn/proto/longlink_packer.h"

using namespace mars::stn;

static const unsigned int kCmdIdOutOfBand = 72;
static const int kTimeout = 10 * 1000;  // s

LongLinkSpeedTestItem::LongLinkSpeedTestItem(const std::string &ip, uint16_t port)
        : mIP(ip)
        , mPort(port)
        , mSocket(-1)
        , mState(kLongLinkSpeedTestConnecting)
        , mBeforeConnectTime(0)
        , mAfterConnectTime(0) {
    AutoBuffer body;
    AutoBuffer extension;
    longlink_noop_req_body(body, extension);

    longlink_pack(longlink_noop_cmdid(), Task::kNoopTaskID, body, extension, mReqBuf, NULL);
    mReqBuf.Seek(0, AutoBuffer::ESeekStart);

    mSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (mSocket == INVALID_SOCKET) {
        xerror2(TSF"socket create error, errno:%0", strerror(errno));
        return;
    }

    // set the socket to unblocked model
#ifdef _WIN32
    if (0 != socket_ipv6only(mSocket, 0)){ xwarn2(TSF"set ipv6only failed. error %_",strerror(socket_errno)); }
#endif

    int ret = socket_set_nobio(mSocket);

    if (ret != 0) {
        xerror2(TSF"nobio error");
        ::socket_close(mSocket);
        mSocket = -1;
        mSocket = -1;
        return;
    }

    if (::getNetInfo() == kWifi && socket_fix_tcp_mss(mSocket) < 0) {
#ifdef ANDROID
        xinfo2(TSF"wifi set tcp mss error:%0", strerror(socket_errno));
#endif
    }

    struct sockaddr_in addr;

    bzero(&addr, sizeof(addr));
    addr = *(struct sockaddr_in *) (&socket_address(mIP.c_str(), mPort).address());

    mBeforeConnectTime = gettickcount();

    if (0 != ::connect(mSocket, (sockaddr *) &addr, sizeof(addr))) {
        xerror2(TSF"connect fail");
    }
}

LongLinkSpeedTestItem::~LongLinkSpeedTestItem() {
    CloseSocket();
}

void LongLinkSpeedTestItem::HandleFDISSet(SocketSelect &_sel) {
    xverbose_function();

    if (kLongLinkSpeedTestFail == mState || kLongLinkSpeedTestSuc == mState) {
        return;
    }

    if (_sel.Exception_FD_ISSET(mSocket)) {
        xerror2(TSF"the socket is error, error:%0", strerror(errno));
        mState = kLongLinkSpeedTestFail;
    } else if (_sel.Write_FD_ISSET(mSocket)) {
        if (kLongLinkSpeedTestConnecting == mState) {
            mAfterConnectTime = gettickcount();
        }

        mState = __HandleSpeedTestReq();
    } else if (_sel.Read_FD_ISSET(mSocket)) {
        mState = __HandleSpeedTestResp();
    } else {
        // do nothing
    }
}

void LongLinkSpeedTestItem::HandleSetFD(SocketSelect &_sel) {
    switch (mState) {
        case kLongLinkSpeedTestConnecting:
        case kLongLinkSpeedTestOOB:
        case kLongLinkSpeedTestReq:
            _sel.Write_FD_SET(mSocket);
            _sel.Read_FD_SET(mSocket);
            _sel.Exception_FD_SET(mSocket);
            break;

        case kLongLinkSpeedTestResp:
            _sel.Read_FD_SET(mSocket);
            _sel.Exception_FD_SET(mSocket);
            break;

        default:
            xassert2(false);
            break;
    }
}

int LongLinkSpeedTestItem::GetSocket() {
    return mSocket;
}

std::string LongLinkSpeedTestItem::GetIP() {
    return mIP;
}

unsigned int LongLinkSpeedTestItem::GetPort() {
    return mPort;
}

unsigned long LongLinkSpeedTestItem::GetConnectTime() {
    return mAfterConnectTime - mBeforeConnectTime;
}

int LongLinkSpeedTestItem::GetState() {
    return mState;
}

void LongLinkSpeedTestItem::CloseSocket() {
    if (mSocket > 0) {
        ::socket_close(mSocket);
        mSocket = -1;
        mSocket = -1;
    }
}

int LongLinkSpeedTestItem::__HandleSpeedTestReq() {
    ssize_t nwrite = ::send(mSocket, mReqBuf.PosPtr(), mReqBuf.Length() - mReqBuf.Pos(), 0);

    if (nwrite <= 0) {
        xerror2(TSF"writen send <= 0, errno:%0, nwrite:%1", strerror(errno), nwrite);
        return kLongLinkSpeedTestFail;
    } else {
        xdebug2(TSF"send length:%0", nwrite);
        mReqBuf.Seek(nwrite, AutoBuffer::ESeekCur);

        if (mReqBuf.Length() - mReqBuf.Pos() <= 0) {
            return kLongLinkSpeedTestResp;
        } else {
            return kLongLinkSpeedTestReq;
        }
    }
}

int LongLinkSpeedTestItem::__HandleSpeedTestResp() {
    if (mRspBuf.Capacity() - mRspBuf.Pos() <= 0) {
        mRspBuf.AddCapacity(mRspBuf.Capacity() == 0 ? 1024 : mRspBuf.Capacity());
    }

    ssize_t nrecv = recv(mSocket, mRspBuf.PosPtr(), mRspBuf.Capacity() - mRspBuf.Pos(), 0);

    if (nrecv <= 0) {
        xerror2(TSF"recv nrecv <= 0, errno:%0, mRspBuf.Capacity():%1,mRspBuf.Pos():%2", strerror(errno),
                mRspBuf.Capacity(), mRspBuf.Pos());
        return kLongLinkSpeedTestFail;
    } else {
        xdebug2(TSF"recv length:%0", nrecv);
        mRspBuf.Length(nrecv + mRspBuf.Pos(), mRspBuf.Length() + nrecv);

        size_t pacLength = 0;
        uint32_t anSeq = 0;
        uint32_t anCmdID = 0;
        AutoBuffer body;
        AutoBuffer extension;

        int nRet = longlink_unpack(mRspBuf, anCmdID, anSeq, pacLength, body, extension, NULL);

        if (LONGLINK_UNPACK_FALSE == nRet) {
            xerror2(TSF"longlink_unpack false");
            return kLongLinkSpeedTestFail;
        } else if (LONGLINK_UNPACK_CONTINUE == nRet) {
            xdebug2(TSF"not recv an package,continue recv, mRspBuf.Lenght():%0", mRspBuf.Length());
            return kLongLinkSpeedTestResp;
        } else if (kCmdIdOutOfBand == anCmdID) {
            uint32_t nType = ((uint32_t *) body.Ptr(16))[0];
            uint32_t nTime = ((uint32_t *) body.Ptr(16))[1];
            nType = ntohl(nType);
            nTime = ntohl(nTime);
            xwarn2(TSF"out of band,nType:%0, nTime:%1", nType, nTime);

            mRspBuf.Reset();
            return kLongLinkSpeedTestOOB;
        } else if (longlink_noop_isresp(Task::kNoopTaskID, anCmdID, anSeq, body, extension)) {
            return kLongLinkSpeedTestSuc;
        } else {
            xassert2(false);
            return kLongLinkSpeedTestFail;
        }
    }
}

////////////////////////////////////////////////////////////////

LongLinkSpeedTest::LongLinkSpeedTest(const boost::shared_ptr<NetSource> &netSource) : mNetSource(netSource),
                                                                                       mSelector(mBreaker) {
    if (!mBreaker.IsCreateSuc()) {
        xassert2(false, "pipe error");
        return;
    }
}

LongLinkSpeedTest::~LongLinkSpeedTest() {
}

bool LongLinkSpeedTest::GetFastestSocket(int &_fdSocket, std::string &_strIp, unsigned int &port, IPSourceType &type,
                                         unsigned long &_connectMillSec) {
    xdebug_function();

    std::vector<IPPortItem> ipItemVec;

    if (!mNetSource->GetLongLinkSpeedTestIPs(ipItemVec)) {
        xerror2(TSF"ipItemVec is empty");
        return false;
    }

    std::vector<LongLinkSpeedTestItem *> speedTestItemVec;

    for (std::vector<IPPortItem>::iterator iter = ipItemVec.begin(); iter != ipItemVec.end(); ++iter) {
        LongLinkSpeedTestItem *item = new LongLinkSpeedTestItem((*iter).ip, (*iter).port);
        speedTestItemVec.push_back(item);
    }

    int tryCount = 0;
    bool loopShouldBeStop = false;

    while (!loopShouldBeStop) {
        mSelector.PreSelect();

        for (std::vector<LongLinkSpeedTestItem *>::iterator iter = speedTestItemVec.begin();
             iter != speedTestItemVec.end(); ++iter) {
            (*iter)->HandleSetFD(mSelector);
        }

        int selectRet = mSelector.Select(kTimeout);

        if (selectRet == 0) {
            xerror2(TSF"time out");
            break;
        }

        if (selectRet < 0) {
            xerror2(TSF"select errror, ret:%0, strerror(errno):%1", selectRet, strerror(errno));

            if (EINTR == errno && tryCount < 3) {
                ++tryCount;
                continue;
            } else {
                break;
            }
        }

        if (mSelector.IsException()) {
            xerror2(TSF"pipe exception");
            break;
        }

        if (mSelector.IsBreak()) {
            xwarn2(TSF"FD_ISSET(pipe_[0], &readfd)");
            break;
        }

        size_t count = 0;

        for (std::vector<LongLinkSpeedTestItem *>::iterator iter = speedTestItemVec.begin();
             iter != speedTestItemVec.end(); ++iter) {
            (*iter)->HandleFDISSet(mSelector);

            if (kLongLinkSpeedTestSuc == (*iter)->GetState()) {
                loopShouldBeStop = true;
                break;
            } else if (kLongLinkSpeedTestFail == (*iter)->GetState()) {
                ++count;
            } else {
                // do nothing
            }
        }

        if (count == speedTestItemVec.size()) {
            xwarn2(TSF"all speed tese fail");
            loopShouldBeStop = true;
        }
    }


    for (std::vector<LongLinkSpeedTestItem *>::iterator iter = speedTestItemVec.begin();
         iter != speedTestItemVec.end(); ++iter) {
        for (std::vector<IPPortItem>::iterator ipItemIter = ipItemVec.begin();
             ipItemIter != ipItemVec.end(); ++ipItemIter) {
            std::string ip = (*iter)->GetIP();

            if (ip != (*ipItemIter).ip || (*iter)->GetPort() != (*ipItemIter).port) {
                continue;
            }

            if (kLongLinkSpeedTestSuc == (*iter)->GetState()) {
                // (*ipItemIter).eState = ETestOK;
                type = (*ipItemIter).sourceType;
                _strIp = (*ipItemIter).ip;
                port = (*iter)->GetPort();
            } else if (kLongLinkSpeedTestFail == (*iter)->GetState()) {
                // (*ipItemIter).eState = ETestFail;
            } else {
                // (*ipItemIter).eState = ETestNone;
            }

            break;
        }
    }

    // report the result of speed test
    mNetSource->ReportLongLinkSpeedTestResult(ipItemVec);

    bool bRet = false;

    for (std::vector<LongLinkSpeedTestItem *>::iterator iter = speedTestItemVec.begin();
         iter != speedTestItemVec.end(); ++iter) {
        if (kLongLinkSpeedTestSuc == (*iter)->GetState() && !bRet) {
            bRet = true;
            _fdSocket = (*iter)->GetSocket();
            _connectMillSec = (*iter)->GetConnectTime();
            xdebug2(TSF"speed test success, socket:%0, use time:%1", _fdSocket, _connectMillSec);
        } else {
            (*iter)->CloseSocket();
        }

        delete *iter;
    }

    speedTestItemVec.clear();

    return bRet;
}

boost::shared_ptr<NetSource> LongLinkSpeedTest::GetNetSource() {
    return mNetSource;
}
