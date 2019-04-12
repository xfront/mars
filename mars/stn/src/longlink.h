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
 * longlink.h
 *
 *  Created on: 2014-2-27
 *      Author: yerungui
 */

#ifndef STN_SRC_LONGLINK_H_
#define STN_SRC_LONGLINK_H_

#include <string>
#include <list>

#include "boost/signals2.hpp"
#include "boost/function.hpp"

#include "mars/comm/thread/mutex.h"
#include "mars/comm/thread/thread.h"
#include "mars/comm/alarm.h"
#include "mars/comm/tickcount.h"
#include "mars/comm/move_wrapper.h"
#include "mars/comm/messagequeue/message_queue.h"
#include "mars/comm/socket/socketselect.h"

#include "mars/stn/stn.h"
#include "mars/stn/task_profile.h"

#include "mars/stn/src/net_source.h"
#include "mars/stn/src/longlink_identify_checker.h"

class AutoBuffer;

class XLogger;

class WakeUpLock;

class SmartHeartbeat;

namespace mars {
namespace comm {
class ProxyInfo;
}
namespace stn {

class NetSource;

class longlink_tracker;

struct LongLinkNWriteData {
    LongLinkNWriteData(ssize_t writeLen, const Task &task)
            : mWriteLen(writeLen), mTask(task) {}

    ssize_t mWriteLen;
    Task mTask;
};

struct StreamResp {
    StreamResp(const Task &task = Task(Task::kInvalidTaskID))
            : mTask(task)
            , mStream(KNullAtuoBuffer)
            , mExtension(KNullAtuoBuffer) {
    }

    Task mTask;
    move_wrapper<AutoBuffer> mStream;
    move_wrapper<AutoBuffer> mExtension;
};

class LongLink {
public:
    enum TLongLinkStatus {
        kConnectIdle = 0,
        kConnecting = 1,
        kConnected,
        kDisConnected,
        kConnectFailed,
    };

    // Note: Never Delete Item!!!Just Add!!!
    enum TDisconnectInternalCode {
        kNone = 0,
        kReset = 10000,        // no use
        kRemoteClosed = 10001,
        kUnknownErr = 10002,
        kNoopTimeout = 10003,
        kDecodeError = 10004,
        kUnknownRead = 10005,
        kUnknownWrite = 10006,
        kDecodeErr = 10007,
        kTaskTimeout = 10008,
        kNetworkChange = 10009,
        kIDCChange = 10010,
        kNetworkLost = 10011,
        kSelectError = 10012,
        kPipeError = 10013,
        kHasNewDnsIP = 10014,
        kSelectException = 10015,
        kLinkCheckTimeout = 10016,
        kForceNewGetDns = 10017,
        kLinkCheckError = 10018,
        kTimeCheckSucc = 10019,
    };
public:
    boost::signals2::signal<void(TLongLinkStatus _connectStatus)> SignalConnection;
    boost::signals2::signal<void(const ConnectProfile &_connprofile)> broadcast_linkstatus_signal_;

    boost::function<void(uint32_t taskId)> OnSend;
    boost::function<void(uint32_t taskId, size_t _cachedsize, size_t _package_size)> OnRecv;
    boost::function<void(ErrCmdType errorType, int errorCode, uint32_t cmdId, uint32_t taskId, AutoBuffer &body,
                         AutoBuffer &extension, const ConnectProfile &_info)> OnResponse;
    boost::function<void(int _line, ErrCmdType errType, int errCode, const std::string &ip,
                         uint16_t port)> fun_network_report_;

public:
    LongLink(const mq::MessageQueue_t &msgQueueId, NetSource &netSource);

    virtual ~LongLink();

    bool Send(const AutoBuffer &body, const AutoBuffer &extension, const Task &task);

    bool SendWhenNoData(const AutoBuffer &body, const AutoBuffer &extension, uint32_t cmdId, uint32_t taskId);

    bool Stop(uint32_t taskId);

    bool MakeSureConnected(bool *_newone = NULL);

    void Disconnect(TDisconnectInternalCode _scene);

    TLongLinkStatus ConnectStatus() const;

    ConnectProfile Profile() const { return mConnProfile; }

    tickcount_t &GetLastRecvTime() { return mLastRecvTime; }

private:
    LongLink(const LongLink &);

    LongLink &operator=(const LongLink &);

protected:
    void __ConnectStatus(TLongLinkStatus status);

    void __UpdateProfile(const ConnectProfile &connProfile);

    void __RunResponseError(ErrCmdType errorType, int errorCode, ConnectProfile &profile, bool networkReport = true);

    bool __SendNoopWhenNoData();

    bool __NoopReq(XLogger &_xlog, Alarm &_alarm, bool need_active_timeout);

    bool __NoopResp(uint32_t cmdId, uint32_t taskId, AutoBuffer &bufferReq, AutoBuffer &extension, Alarm &alarm,
                    bool &nooping, ConnectProfile &profile);

    virtual void __OnAlarm();

    virtual void __Run();

    virtual SOCKET __RunConnect(ConnectProfile &connProfile);

    virtual void __RunReadWrite(SOCKET sock, ErrCmdType &errType, int &errCode, ConnectProfile &profile);

protected:

    uint32_t __GetNextHeartbeatInterval();

    void __NotifySmartHeartbeatConnectStatus(TLongLinkStatus status);

    void __NotifySmartHeartbeatHeartReq(ConnectProfile &_profile, uint64_t _internal, uint64_t _actual_internal);

    void __NotifySmartHeartbeatHeartResult(bool _succes, bool failTimeout, ConnectProfile &_profile);

    void __NotifySmartHeartbeatJudgeDozeStyle();

protected:
    MessageQueue::ScopeRegister mAsyncReg;
    NetSource &mNetSource;

    Mutex mMutex;
    Thread mThread;

    boost::scoped_ptr<longlink_tracker> mTracker;
    NetSource::DnsUtil mDnsUtil;
    SocketBreaker mConnectBreak;
    TLongLinkStatus mConnectStatus;
    ConnectProfile mConnProfile;
    TDisconnectInternalCode mDisconnectInternalCode;

    SocketBreaker mReadWriteBreak;
    LongLinkIdentifyChecker mIdentifyChecker;
    std::list<std::pair<Task, move_wrapper<AutoBuffer>>> mSendList;
    tickcount_t mLastRecvTime;

    SmartHeartbeat *mSmartHeartBeat;
    WakeUpLock *mWakeLock;
};

}
}

#endif // STN_SRC_LONGLINK_H_
