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
 * longlink.cc
 *
 *  Created on: 2014-2-27
 *      Author: yerungui
 */

#include "longlink.h"

#include <algorithm>

#include "boost/bind.hpp"

#include "mars/app/app.h"
#include "mars/baseevent/active_logic.h"
#include "mars/comm/thread/lock.h"
#include "mars/comm/autobuffer.h"
#include "mars/comm/comm_data.h"
#include "mars/comm/xlogger/xlogger.h"
#include "mars/comm/socket/local_ipstack.h"
#include "mars/comm/socket/complexconnect.h"
#include "mars/comm/socket/unix_socket.h"
#include "mars/comm/socket/socket_address.h"
#include "mars/comm/platform_comm.h"
#include "mars/comm/messagequeue/message_queue.h"
#include "mars/baseevent/baseprjevent.h"

#if defined(__ANDROID__) || defined(__APPLE__)

#include "mars/comm/socket/getsocktcpinfo.h"

#endif

#include "mars/stn/config.h"

#include "proto/longlink_packer.h"
#include "smart_heartbeat.h"

#define AYNC_HANDLER  mAsyncReg.Get()
#define STATIC_RETURN_SYNC2ASYNC_FUNC(func) RETURN_SYNC2ASYNC_FUNC(func, )

using namespace mars::stn;
using namespace mars::app;

namespace {
class LongLinkConnectObserver : public IComplexConnect {
public:
    LongLinkConnectObserver(LongLink &longLink, const std::vector<IPPortItem> &ipList)
    : mLongLink(longLink)
    , mIpList(ipList) {
        memset(mConnectingIndex, 0, sizeof(mConnectingIndex));
    };

    virtual void OnCreated(unsigned int index, const socket_address &addr, SOCKET socket) {}

    virtual void OnConnect(unsigned int index, const socket_address &addr, SOCKET socket) {
        mConnectingIndex[index] = 1;
    }

    virtual void OnConnected(unsigned int index, const socket_address &addr, SOCKET socket, int error, int rtt) {
        if (0 == error) {
            if (!OnShouldVerify(index, addr)) {
                mConnectingIndex[index] = 0;
            }
        } else {
            xwarn2(TSF"index:%_, connnet fail host:%_, iptype:%_", index, mIpList[index].host,
                   mIpList[index].sourceType);
            //xassert2(mLongLink.fun_network_report_);
            mConnectingIndex[index] = 0;

            if (mLongLink.fun_network_report_) {
                mLongLink.fun_network_report_(__LINE__, kEctSocket, error, addr.ip(), addr.port());
            }
        }
    }

    virtual bool OnShouldVerify(unsigned int index, const socket_address &addr) {
        return longlink_complexconnect_need_verify();
    }

    virtual bool
    OnVerifySend(unsigned int index, const socket_address &addr, SOCKET socket, AutoBuffer &_buffer_send) {
        AutoBuffer body;
        AutoBuffer extension;
        longlink_noop_req_body(body, extension);
        longlink_pack(longlink_noop_cmdid(), Task::kNoopTaskID, body, extension, _buffer_send, NULL);
        return true;
    }

    virtual bool
    OnVerifyRecv(unsigned int index, const socket_address &addr, SOCKET socket, const AutoBuffer &_buffer_recv) {

        mConnectingIndex[index] = 0;

        uint32_t cmdid = 0;
        uint32_t taskid = Task::kInvalidTaskID;
        size_t pack_len = 0;
        AutoBuffer bufferbody;
        AutoBuffer extension;
        int ret = longlink_unpack(_buffer_recv, cmdid, taskid, pack_len, bufferbody, extension, NULL);

        if (LONGLINK_UNPACK_OK != ret) {
            xerror2(TSF"0>ret, index:%_, sock:%_, %_, ret:%_, cmdid:%_, taskId:%_, pack_len:%_, recv_len:%_",
                    index, socket, addr.url(), ret, cmdid, taskid, pack_len, _buffer_recv.Length());
            if (mLongLink.fun_network_report_) {
                mLongLink.fun_network_report_(__LINE__, kEctSocket, EBADMSG, addr.ip(), addr.port());
            }
            return false;
        }

        if (!longlink_noop_isresp(taskid, cmdid, taskid, bufferbody, extension)) {
            xwarn2(TSF"index:%_, sock:%_, %_, ret:%_, cmdid:%_, taskId:%_, pack_len:%_, recv_len:%_", index,
                   socket, addr.url(), ret, cmdid, taskid, pack_len, _buffer_recv.Length());
        }

        return true;
    }

    char mConnectingIndex[32];

private:
    LongLinkConnectObserver(const LongLinkConnectObserver &);

    LongLinkConnectObserver &operator=(const LongLinkConnectObserver &);

public:
    LongLink &mLongLink;
    const std::vector<IPPortItem> &mIpList;
};

}

LongLink::LongLink(const mq::MessageQueue_t &msgQueueId, NetSource &netSource)
        : mAsyncReg(MessageQueue::InstallAsyncHandler(msgQueueId))
        , mNetSource(netSource)
        , mThread(boost::bind(&LongLink::__Run, this), XLOGGER_TAG "::lonklink")
        , mConnectStatus(kConnectIdle)
        , mDisconnectInternalCode(kNone)
#ifdef ANDROID
        , mSmartHeartBeat(new SmartHeartbeat), mWakeLock(new WakeUpLock)
#else
, mSmartHeartBeat(NULL)
, mWakeLock(NULL)
#endif
{
    xinfo2(TSF"handler:(%_,%_)", mAsyncReg.Get().queue, mAsyncReg.Get().seq);
}

LongLink::~LongLink() {
    Disconnect(kReset);
    mAsyncReg.CancelAndWait();
    if (NULL != mSmartHeartBeat) {
        delete mSmartHeartBeat, mSmartHeartBeat = NULL;
    }
#ifdef ANDROID
    if (NULL != mWakeLock) {
        delete mWakeLock, mWakeLock = NULL;
    }
#endif
}

bool LongLink::Send(const AutoBuffer &body, const AutoBuffer &extension, const Task &task) {
    ScopedLock lock(mMutex);

    if (kConnected != mConnectStatus) return false;

    xassert2(mTracker.get());

    mSendList.push_back(std::make_pair(task, move_wrapper<AutoBuffer>(AutoBuffer())));
    longlink_pack(task.cmdId, task.taskId, body, extension, mSendList.back().second, mTracker.get());
    mSendList.back().second->Seek(0, AutoBuffer::ESeekStart);

    mReadWriteBreak.Break();
    return true;
}

bool
LongLink::SendWhenNoData(const AutoBuffer &body, const AutoBuffer &extension, uint32_t cmdId, uint32_t taskId) {
    ScopedLock lock(mMutex);

    if (kConnected != mConnectStatus) return false;
    if (!mSendList.empty()) return false;

    xassert2(mTracker.get());

    Task task(taskId);
    task.sendOnly = true;
    task.cmdId = cmdId;
    task.taskId = taskId;
    mSendList.push_back(std::make_pair(task, move_wrapper<AutoBuffer>(AutoBuffer())));
    longlink_pack(cmdId, taskId, body, extension, mSendList.back().second, mTracker.get());
    mSendList.back().second->Seek(0, AutoBuffer::ESeekStart);

    mReadWriteBreak.Break();
    return true;
}

bool LongLink::__SendNoopWhenNoData() {
    AutoBuffer body;
    AutoBuffer extension;
    longlink_noop_req_body(body, extension);
    return SendWhenNoData(body, extension, longlink_noop_cmdid(), Task::kNoopTaskID);
}

bool LongLink::Stop(uint32_t taskId) {
    ScopedLock lock(mMutex);

    for (auto it = mSendList.begin(); it != mSendList.end(); ++it) {
        if (taskId == it->first.taskId && 0 == it->second->Pos()) {
            mSendList.erase(it);
            return true;
        }
    }

    return false;
}


bool LongLink::MakeSureConnected(bool *_newone) {
    if (_newone)
        *_newone = false;

    ScopedLock lock(mMutex);

    if (kConnected == ConnectStatus())
        return true;

    bool newone = false;
    mThread.start(&newone);

    if (newone) {
        mConnectStatus = kConnectIdle;
        mConnProfile.Reset();
        mIdentifyChecker.Reset();
        mDisconnectInternalCode = kNone;
        mReadWriteBreak.Clear();
        mConnectBreak.Clear();
        mSendList.clear();
    }

    if (_newone)
        *_newone = newone;

    return false;
}

void LongLink::Disconnect(TDisconnectInternalCode _scene) {
    xinfo2(TSF"_scene:%_", _scene);

    ScopedLock lock(mMutex);

    if (!mThread.isRunning())
        return;

    mDisconnectInternalCode = _scene;

    bool recreate = false;

    if (!mReadWriteBreak.Break() || !mConnectBreak.Break()) {
        xassert2(false, "breaker fail");
        mConnectBreak.Close();
        mReadWriteBreak.Close();
        recreate = true;
    }
    lock.unlock();

    mDnsUtil.Cancel();
    mThread.join();

    if (recreate) {
        mConnectBreak.ReCreate();
        mReadWriteBreak.ReCreate();
    }
}

bool LongLink::__NoopReq(XLogger &_log, Alarm &_alarm, bool need_active_timeout) {
    AutoBuffer buffer;
    uint32_t req_cmdid = 0;
    bool suc = false;

    if (mIdentifyChecker.GetIdentifyBuffer(buffer, req_cmdid)) {
        Task task(Task::kLongLinkIdentifyCheckerTaskID);
        task.cmdId = req_cmdid;
        suc = Send(buffer, KNullAtuoBuffer, task);
        mIdentifyChecker.SetID(Task::kLongLinkIdentifyCheckerTaskID);
        xinfo2(TSF"start noop synccheck taskId:%0, cmdid:%1, ", Task::kLongLinkIdentifyCheckerTaskID, req_cmdid)
            >> _log;
    } else {
        suc = __SendNoopWhenNoData();
        xinfo2(TSF"start noop taskId:%0, cmdid:%1, ", Task::kNoopTaskID, longlink_noop_cmdid()) >> _log;
    }

    if (suc) {
        _alarm.Cancel();
        _alarm.Start(need_active_timeout ? (5 * 1000) : (8 * 1000));
#ifdef ANDROID
        mWakeLock->Lock(8 * 1000);
#endif
    } else {
        xerror2("send noop fail");
    }

    return suc;
}

bool LongLink::__NoopResp(uint32_t cmdId, uint32_t taskId, AutoBuffer &bufferReq, AutoBuffer &extension, Alarm &alarm,
                          bool &nooping, ConnectProfile &profile) {
    bool is_noop = false;

    if (mIdentifyChecker.IsIdentifyResp(cmdId, taskId, bufferReq, extension)) {
        xinfo2(TSF"end noop synccheck");
        is_noop = true;
        if (mIdentifyChecker.OnIdentifyResp(bufferReq)) {
            if (fun_network_report_)
                fun_network_report_(__LINE__, kEctOK, 0, profile.ip, profile.port);
        }
    }

    if (longlink_noop_isresp(Task::kNoopTaskID, cmdId, taskId, bufferReq, extension)) {
        longlink_noop_resp_body(bufferReq, extension);
        xinfo2(TSF"end noop");
        is_noop = true;
    }

    if (is_noop && nooping) {
        nooping = false;
        alarm.Cancel();
        __NotifySmartHeartbeatHeartResult(true, false, profile);
#ifdef ANDROID
        mWakeLock->Lock(500);
#endif
    }

    return is_noop;
}

void
LongLink::__RunResponseError(ErrCmdType errorType, int errorCode, ConnectProfile &profile, bool networkReport) {

    AutoBuffer buf;
    AutoBuffer extension;
    if (OnResponse)
        OnResponse(errorType, errorCode, 0, Task::kInvalidTaskID, buf, extension, profile);
    //xassert2(fun_network_report_);

    if (networkReport && fun_network_report_)
        fun_network_report_(__LINE__, errorType, errorCode, profile.ip, profile.port);
}

LongLink::TLongLinkStatus LongLink::ConnectStatus() const {
    return mConnectStatus;
}

void LongLink::__ConnectStatus(TLongLinkStatus status) {
    if (status == mConnectStatus) return;
    xinfo2(TSF"connect status from:%0 to:%1, nettype:%_", mConnectStatus, status, ::getNetInfo());
    mConnectStatus = status;
    __NotifySmartHeartbeatConnectStatus(mConnectStatus);
    if (kConnected == mConnectStatus && fun_network_report_)
        fun_network_report_(__LINE__, kEctOK, 0, mConnProfile.ip, mConnProfile.port);
    STATIC_RETURN_SYNC2ASYNC_FUNC(boost::bind(boost::ref(SignalConnection), mConnectStatus));
}

void LongLink::__UpdateProfile(const ConnectProfile &connProfile) {
    STATIC_RETURN_SYNC2ASYNC_FUNC(boost::bind(&LongLink::__UpdateProfile, this, connProfile));
    mConnProfile = connProfile;

    if (0 != mConnProfile.disconnTime) broadcast_linkstatus_signal_(mConnProfile);
}

void LongLink::__OnAlarm() {
    mReadWriteBreak.Break();
#ifdef ANDROID
    mWakeLock->Lock(3 * 1000);
#endif
}

void LongLink::__Run() {
    // sync to MakeSureConnected data reset
    {
        ScopedLock lock(mMutex);
        mTracker.reset(longlink_tracker::Create());
    }

    uint64_t cur_time = gettickcount();
    xinfo_function(TSF"LongLink Rebuild span:%_, net:%_",
                   mConnProfile.disconnTime != 0 ? cur_time - mConnProfile.disconnTime : 0, getNetInfo());

    ConnectProfile conn_profile;
    conn_profile.startTime = cur_time;
    conn_profile.connReason = mConnProfile.disconnErrCode;
    getCurrNetLabel(conn_profile.netType);
    conn_profile.tid = xlogger_tid();
    __UpdateProfile(conn_profile);

#ifdef ANDROID
    mWakeLock->Lock(40 * 1000);
#endif
    SOCKET sock = __RunConnect(conn_profile);
#ifdef ANDROID
    mWakeLock->Lock(1000);
#endif

    if (INVALID_SOCKET == sock) {
        conn_profile.disconnTime = ::gettickcount();
        conn_profile.disconnSignal = ::getSignal(::getNetInfo() == kWifi);
        __UpdateProfile(conn_profile);

        ScopedLock lock(mMutex);
        mTracker.reset();
        return;
    }

    ErrCmdType errtype = kEctOK;
    int errcode = 0;
    __RunReadWrite(sock, errtype, errcode, conn_profile);

    socket_close(sock);

    conn_profile.disconnTime = ::gettickcount();
    conn_profile.disconnErrType = errtype;
    conn_profile.disconnErrCode = errcode;
    conn_profile.disconnSignal = ::getSignal(::getNetInfo() == kWifi);

    __ConnectStatus(kDisConnected);
    xinfo2(TSF"longlink lifetime:%_", (gettickcount() - conn_profile.connTime));
    __UpdateProfile(conn_profile);

    if (kEctOK != errtype) __RunResponseError(errtype, errcode, conn_profile);

#ifdef ANDROID
    mWakeLock->Lock(1000);
#endif

    ScopedLock lock(mMutex);
    mTracker.reset();
}

SOCKET LongLink::__RunConnect(ConnectProfile &connProfile) {

    __ConnectStatus(kConnecting);
    connProfile.dnsTime = ::gettickcount();
    __UpdateProfile(connProfile);

    std::vector<IPPortItem> ip_items;
    std::vector<socket_address> vecaddr;

    mNetSource.GetLongLinkItems(ip_items, mDnsUtil);
    mars::comm::ProxyInfo proxy_info = mars::app::GetProxyInfo("");
    bool use_proxy = proxy_info.IsValid() && mars::comm::kProxyNone != proxy_info.type &&
                     mars::comm::kProxyHttp != proxy_info.type && mNetSource.GetLongLinkDebugIP().empty();
    xinfo2(TSF"task socket dns ip:%_ proxytype:%_ useproxy:%_", NetSource::DumpTable(ip_items), proxy_info.type,
           use_proxy);

    std::string log;
    std::string netInfo;
    getCurrNetLabel(netInfo);
    bool isnat64 = ELocalIPStack_IPv6 == local_ipstack_detect_log(log);//local_ipstack_detect();
    xinfo2(TSF"ipstack log:%_, netInfo:%_", log, netInfo);

    for (unsigned int i = 0; i < ip_items.size(); ++i) {
        if (use_proxy) {
            vecaddr.push_back(socket_address(ip_items[i].ip.c_str(), ip_items[i].port));
        } else {
            vecaddr.push_back(socket_address(ip_items[i].ip.c_str(), ip_items[i].port).v4tov6_address(isnat64));
        }
    }

    if (vecaddr.empty()) {
        xerror2("task socket close sock:-1 vecaddr empty");
        __ConnectStatus(kConnectFailed);
        __RunResponseError(kEctDns, kEctDnsMakeSocketPrepared, connProfile);
        return INVALID_SOCKET;
    }

    connProfile.proxyInfo = proxy_info;
    connProfile.ipList = ip_items;
    connProfile.host = ip_items[0].host;
    connProfile.ipType = ip_items[0].sourceType;
    connProfile.ip = ip_items[0].ip;
    connProfile.port = ip_items[0].port;
    connProfile.nat64 = isnat64;
    connProfile.dnsEndtime = ::gettickcount();
    __UpdateProfile(connProfile);

    socket_address *proxy_addr = NULL;

    if (use_proxy) {
        std::string proxy_ip = proxy_info.ip;
        if (proxy_info.ip.empty() && !proxy_info.host.empty()) {
            std::vector<std::string> ips;
            if (!mDnsUtil.GetDNS().GetHostByName(proxy_info.host, ips) || ips.empty()) {
                xwarn2(TSF"dns %_ error", proxy_info.host);
                __ConnectStatus(kConnectFailed);
                __RunResponseError(kEctDns, kEctDnsMakeSocketPrepared, connProfile);
                return INVALID_SOCKET;
            }

            proxy_addr = &((new socket_address(ips.front().c_str(), proxy_info.port))->v4tov6_address(isnat64));

        } else {
            proxy_addr = &((new socket_address(proxy_ip.c_str(), proxy_info.port))->v4tov6_address(isnat64));
        }

        connProfile.ipType = kIPSourceProxy;

    }

    // set the first ip info to the profiler, after connect, the ip info will be overwrriten by the real one

    LongLinkConnectObserver connect_observer(*this, ip_items);
    ComplexConnect com_connect(kLonglinkConnTimeout, kLonglinkConnInteral, kLonglinkConnInteral, kLonglinkConnMax);

    SOCKET sock = com_connect.ConnectImpatient(vecaddr, mConnectBreak, &connect_observer, proxy_info.type, proxy_addr,
                                               proxy_info.username, proxy_info.password);

    delete proxy_addr;

    connProfile.connTime = gettickcount();
    connProfile.connErrcode = com_connect.ErrorCode();
    connProfile.connRtt = com_connect.IndexRtt();
    connProfile.connCost = com_connect.TotalCost();
    connProfile.tryIpCount = com_connect.TryCount();
    __UpdateProfile(connProfile);

    if (INVALID_SOCKET == sock) {
        xwarn2(TSF"task socket connect fail sock:-1, costtime:%0", com_connect.TotalCost());

        __ConnectStatus(kConnectFailed);

        if (kNone == mDisconnectInternalCode)
            __RunResponseError(kEctSocket, kEctSocketMakeSocketPrepared, connProfile, false);

        return INVALID_SOCKET;
    }

    xassert2(0 <= com_connect.Index() && (unsigned int) com_connect.Index() < ip_items.size());

    if (fun_network_report_) {
        for (int i = 0; i < com_connect.Index(); ++i) {
            if (1 == connect_observer.mConnectingIndex[i])
                fun_network_report_(__LINE__, kEctSocket, SOCKET_ERRNO(ETIMEDOUT), ip_items[i].ip,
                                    ip_items[i].port);
        }
    }

    connProfile.ipIndex = com_connect.Index();
    connProfile.host = ip_items[com_connect.Index()].host;
    connProfile.ipType = ip_items[com_connect.Index()].sourceType;
    connProfile.ip = ip_items[com_connect.Index()].ip;
    connProfile.port = ip_items[com_connect.Index()].port;
    connProfile.localIp = socket_address::getsockname(sock).ip();
    connProfile.localPort = socket_address::getsockname(sock).port();

    xinfo2(TSF"task socket connect suc sock:%_, host:%_, ip:%_, port:%_, local_ip:%_, local_port:%_, iptype:%_, costtime:%_, rtt:%_, totalcost:%_, index:%_, net:%_",
           sock, connProfile.host, connProfile.ip, connProfile.port, connProfile.localIp,
           connProfile.localPort, IPSourceTypeString[connProfile.ipType], com_connect.TotalCost(),
           com_connect.IndexRtt(), com_connect.IndexTotalCost(), com_connect.Index(), ::getNetInfo());
    __ConnectStatus(kConnected);
    __UpdateProfile(connProfile);

    xerror2_if(0 != socket_disable_nagle(sock, 1), TSF"socket_disable_nagle sock:%0, %1(%2)", sock, socket_errno, socket_strerror(socket_errno));

    //    struct linger so_linger;
    //    so_linger.l_onoff = 1;
    //    so_linger.l_linger = 0;

    //    xerror2_if(0 != setsockopt(sock, SOL_SOCKET, SO_LINGER, (const char*)&so_linger, sizeof(so_linger)),
    //               TSF"SO_LINGER sock:%0, %1(%2)", sock, socket_errno, socket_strerror(socket_errno));

    return sock;
}

void LongLink::__RunReadWrite(SOCKET sock, ErrCmdType &errType, int &errCode, ConnectProfile &profile) {

    Alarm alarmNoopInterval(boost::bind(&LongLink::__OnAlarm, this), false);
    Alarm alarmNoopTimeout(boost::bind(&LongLink::__OnAlarm, this), false);

    std::map<uint32_t, StreamResp> sendTaskMap;
    std::vector<LongLinkNWriteData> sentTaskList;

    AutoBuffer bufRecv;
    bool firstNoopSent = false;
    bool nooping = false;
    xgroup2_define(close_log);

    while (true) {
        if (!alarmNoopInterval.IsWaiting()) {
            if (firstNoopSent && alarmNoopInterval.Status() != Alarm::kOnAlarm) {
                xassert2(false, "noop interval alarm not running");
            }

            if (firstNoopSent && alarmNoopInterval.Status() == Alarm::kOnAlarm) {
                __NotifySmartHeartbeatJudgeDozeStyle();
            }
            xgroup2_define(noop_xlog);
            uint64_t last_noop_interval = alarmNoopInterval.After();
            uint64_t last_noop_actual_interval = (alarmNoopInterval.Status() == Alarm::kOnAlarm)
                                                 ? alarmNoopInterval.ElapseTime() : 0;
            bool has_late_toomuch = (last_noop_actual_interval >= (15 * 60 * 1000));

            if (__NoopReq(noop_xlog, alarmNoopTimeout, has_late_toomuch)) {
                nooping = true;
                __NotifySmartHeartbeatHeartReq(profile, last_noop_interval, last_noop_actual_interval);
            }

            firstNoopSent = true;

            uint64_t noop_interval = __GetNextHeartbeatInterval();
            xinfo2(TSF" last:(%_,%_), next:%_", last_noop_interval, last_noop_actual_interval, noop_interval)
                >> noop_xlog;
            alarmNoopInterval.Cancel();
            alarmNoopInterval.Start((int) noop_interval);
        }

        if (nooping && (alarmNoopTimeout.Status() == Alarm::kInit || alarmNoopTimeout.Status() == Alarm::kCancel)) {
            xassert2(false, "noop but alarmnooptimeout not running, take as noop timeout");
            errType = kEctSocket;
            errCode = kEctSocketRecvErr;
            goto End;
        }

        SocketSelect sel(mReadWriteBreak, true);
        sel.PreSelect();
        sel.Read_FD_SET(sock);
        sel.Exception_FD_SET(sock);

        ScopedLock lock(mMutex);

        if (!mSendList.empty()) sel.Write_FD_SET(sock);

        lock.unlock();

        int retsel = sel.Select(10 * 60 * 1000);

        if (kNone != mDisconnectInternalCode) {
            xwarn2(TSF"task socket close sock:%0, user disconnect:%1, nread:%_, nwrite:%_", sock,
                   mDisconnectInternalCode, socket_nread(sock), socket_nwrite(sock)) >> close_log;
            errType = kEctCanceld;
            errCode = kEctSocketUserBreak;
            goto End;
        }

        if (0 > retsel) {
            xfatal2(TSF"task socket close sock:%0, 0 > retsel, errno:%_, nread:%_, nwrite:%_", sock, sel.Errno(),
                    socket_nread(sock), socket_nwrite(sock)) >> close_log;
            errType = kEctSocket;
            errCode = sel.Errno();
            goto End;
        }

        if (sel.IsException()) {
            xerror2(TSF"task socket close sock:%0, socketselect excptoin:%1(%2), nread:%_, nwrite:%_", sock,
                    socket_errno, socket_strerror(socket_errno), socket_nread(sock), socket_nwrite(sock))
                >> close_log;
            errType = kEctSocket;
            errCode = socket_errno;
            goto End;
        }

        if (sel.Exception_FD_ISSET(sock)) {
            int error = socket_error(sock);
            xerror2(TSF"task socket close sock:%0, excptoin:%1(%2), nread:%_, nwrite:%_", sock, error,
                    socket_strerror(error), socket_nread(sock), socket_nwrite(sock)) >> close_log;
            errType = kEctSocket;
            errCode = error;
            goto End;
        }

        if (nooping && alarmNoopTimeout.Status() == Alarm::kOnAlarm) {
            xerror2(TSF"task socket close sock:%0, noop timeout, nread:%_, nwrite:%_", sock, socket_nread(sock),
                    socket_nwrite(sock)) >> close_log;
//            __NotifySmartHeartbeatJudgeDozeStyle();
            errType = kEctSocket;
            errCode = kEctSocketRecvErr;
            goto End;
        }

        lock.lock();
        if (socket_nwrite(sock) == 0 && !sentTaskList.empty()) {
            sentTaskList.clear();
        }

        if (sel.Write_FD_ISSET(sock) && !mSendList.empty()) {
            xgroup2_define(xlog_group);
            xinfo2(TSF"task socket send sock:%0, ", sock) >> xlog_group;

#ifndef WIN32
            iovec *vecwrite = (iovec *) calloc(mSendList.size(), sizeof(iovec));
            unsigned int offset = 0;

            for (auto it = mSendList.begin(); it != mSendList.end(); ++it) {
                vecwrite[offset].iov_base = it->second->PosPtr();
                vecwrite[offset].iov_len = it->second->PosLength();

                ++offset;
            }

            ssize_t writelen = writev(sock, vecwrite, (int) mSendList.size());

            free(vecwrite);
#else

            //ssize_t writelen = ::send(sock, mSendList.begin()->data.PosPtr(), mSendList.begin()->data.PosLength(), 0);
            ssize_t writelen = ::send(sock, mSendList.begin()->second->PosPtr(), mSendList.begin()->second->PosLength(), 0);
#endif

            if (0 == writelen || (0 > writelen && !IS_NOBLOCK_SEND_ERRNO(socket_errno))) {
                int error = socket_error(sock);

                errType = kEctSocket;
                errCode = error;
                xerror2(TSF"sock:%0, send:%1(%2)", sock, error, socket_strerror(error)) >> xlog_group;
                goto End;
            }

            if (0 > writelen) writelen = 0;

            unsigned long long noop_interval = __GetNextHeartbeatInterval();
            alarmNoopInterval.Cancel();
            alarmNoopInterval.Start((int) noop_interval);

            xinfo2(TSF"all send:%_, count:%_, ", writelen, mSendList.size()) >> xlog_group;

            GetSignalOnNetworkDataChange()(XLOGGER_TAG, writelen, 0);

            auto it = mSendList.begin();

            while (it != mSendList.end() && 0 < writelen) {
                if (0 == it->second->Pos() && OnSend) OnSend(it->first.taskId);

                if ((size_t) writelen >= it->second->PosLength()) {
                    xinfo2(TSF"sub send taskId:%_, cmdid:%_, %_, len(S:%_, %_/%_), ", it->first.taskId,
                           it->first.cmdId, it->first.cgi, it->second->PosLength(), it->second->PosLength(),
                           it->second->Length()) >> xlog_group;
                    writelen -= it->second->PosLength();
                    if (!it->first.sendOnly) { sendTaskMap[it->first.taskId].mTask = it->first; }

                    LongLinkNWriteData nwrite(it->second->Length(), it->first);
                    sentTaskList.push_back(nwrite);

                    it = mSendList.erase(it);
                } else {
                    xinfo2(TSF"sub send taskId:%_, cmdid:%_, %_, len(S:%_, %_/%_), ", it->first.taskId,
                           it->first.cmdId, it->first.cgi, writelen, it->second->PosLength(), it->second->Length())
                        >> xlog_group;
                    it->second->Seek(writelen, AutoBuffer::ESeekCur);
                    writelen = 0;
                }
            }
        }

        lock.unlock();

        if (sel.Read_FD_ISSET(sock)) {
            bufRecv.AllocWrite(64 * 1024, false);
            ssize_t recvlen = recv(sock, bufRecv.PosPtr(), 64 * 1024, 0);

            if (0 == recvlen) {
                errType = kEctSocket;
                errCode = kEctSocketShutdown;
                xwarn2(TSF"task socket close sock:%0, remote disconnect", sock) >> close_log;
                goto End;
            }

            if (0 > recvlen && !IS_NOBLOCK_READ_ERRNO(socket_errno)) {
                errType = kEctSocket;
                errCode = socket_errno;
                xerror2(TSF"task socket close sock:%0, recv len: %1 errno:%2(%3)", sock, recvlen, socket_errno,
                        socket_strerror(socket_errno)) >> close_log;
                goto End;
            }

            if (0 > recvlen) recvlen = 0;

            GetSignalOnNetworkDataChange()(XLOGGER_TAG, 0, recvlen);

            bufRecv.Length(bufRecv.Pos() + recvlen, bufRecv.Length() + recvlen);
            xinfo2(TSF"task socket recv sock:%_, recv len:%_, buff len:%_", sock, recvlen, bufRecv.Length());

            while (0 < bufRecv.Length()) {
                uint32_t cmdid = 0;
                uint32_t taskid = Task::kInvalidTaskID;
                size_t packlen = 0;
                AutoBuffer body;
                AutoBuffer extension;

                int unpackret = longlink_unpack(bufRecv, cmdid, taskid, packlen, body, extension, mTracker.get());

                if (LONGLINK_UNPACK_FALSE == unpackret) {
                    xerror2(TSF"task socket recv sock:%0, unpack error dump:%1", sock,
                            xdump(bufRecv.Ptr(), bufRecv.Length()));
                    errType = kEctNetMsgXP;
                    errCode = kEctNetMsgXPHandleBufferErr;
                    goto End;
                }

                StreamResp &stream_resp = sendTaskMap[taskid];
                xinfo2(TSF"task socket recv sock:%_, pack recv %_ taskId:%_, cmdid:%_, %_, packlen:(%_/%_)", sock,
                       LONGLINK_UNPACK_CONTINUE == unpackret ? "continue" : "finish", taskid, cmdid,
                       stream_resp.mTask.cgi, LONGLINK_UNPACK_CONTINUE == unpackret ? bufRecv.Length() : packlen,
                       packlen);
                mLastRecvTime.gettickcount();

                if (LONGLINK_UNPACK_CONTINUE == unpackret) {
                    if (OnRecv)
                        OnRecv(taskid, bufRecv.Length(), packlen);
                    break;
                }

                if (stream_resp.mStream->Ptr()) {
                    stream_resp.mStream->Write(body);
                } else {
                    stream_resp.mStream->Attach(body);
                }

                if (stream_resp.mExtension->Ptr()) {
                    stream_resp.mExtension->Write(extension);
                } else {
                    stream_resp.mExtension->Attach(extension);
                }

                bufRecv.Move(-(int) (packlen));
                xassert2(unpackret == LONGLINK_UNPACK_STREAM_END
                         || unpackret == LONGLINK_UNPACK_OK
                         || unpackret == LONGLINK_UNPACK_STREAM_PACKAGE,
                         TSF"unpackret: %_", unpackret);

                if (LONGLINK_UNPACK_STREAM_PACKAGE == unpackret) {
                    if (OnRecv)
                        OnRecv(taskid, packlen, packlen);
                } else if (!__NoopResp(cmdid, taskid, stream_resp.mStream, stream_resp.mExtension, alarmNoopTimeout,
                                       nooping, profile)) {
                    if (OnResponse)
                        OnResponse(kEctOK, 0, cmdid, taskid, stream_resp.mStream, stream_resp.mExtension, profile);
                    sendTaskMap.erase(taskid);
                }
            }
        }
    }


End:
    if (nooping) __NotifySmartHeartbeatHeartResult(false, (errCode == kEctSocketRecvErr), profile);

    std::string netInfo;
    getCurrNetLabel(netInfo);
    xinfo2(TSF", net_type:%_", netInfo) >> close_log;

    int nwrite_size = socket_nwrite(sock);
    int nread_size = socket_nread(sock);
    if (nwrite_size > 0 && !sentTaskList.empty()) {
        xinfo2(TSF", info nwrite:%_ ", nwrite_size) >> close_log;
        ssize_t maxnwrite = 0;
        for (std::vector<LongLinkNWriteData>::reverse_iterator it = sentTaskList.rbegin();
             it != sentTaskList.rend(); ++it) {
            if (nwrite_size <= (maxnwrite + it->mWriteLen)) {
                xinfo2(TSF"taskId:%_, cmdid:%_, cgi:%_ ; ", it->mTask.taskId, it->mTask.cmdId, it->mTask.cgi)
                    >> close_log;
                break;
            } else {
                maxnwrite += it->mWriteLen;
                xinfo2(TSF"taskId:%_, cmdid:%_, cgi:%_ ; ", it->mTask.taskId, it->mTask.cmdId, it->mTask.cgi)
                    >> close_log;
            }
        }
    }
    sentTaskList.clear();

    if (nread_size > 0 && errType != kEctNetMsgXP && errCode != kEctNetMsgXPHandleBufferErr) {
        xinfo2(TSF", info nread:%_ ", nread_size) >> close_log;
        AutoBuffer bufRecv;
        bufRecv.AllocWrite(64 * 1024, false);
        ssize_t recvlen = recv(sock, bufRecv.PosPtr(), 64 * 1024, 0);

        xinfo2_if(recvlen <= 0, TSF", recvlen:%_ error:%_ %_", recvlen, socket_errno, socket_strerror(socket_errno)) >> close_log;
        if (recvlen > 0) {
            bufRecv.Length(bufRecv.Pos() + recvlen, bufRecv.Length() + recvlen);

            while (0 < bufRecv.Length()) {
                uint32_t cmdid = 0;
                uint32_t taskid = Task::kInvalidTaskID;
                size_t packlen = 0;
                AutoBuffer body;
                AutoBuffer extension;

                int unpackret = longlink_unpack(bufRecv, cmdid, taskid, packlen, body, extension, mTracker.get());
                xinfo2(TSF"taskId:%_, cmdid:%_, cgi:%_; ", taskid, cmdid, sendTaskMap[taskid].mTask.cgi)
                    >> close_log;
                if (LONGLINK_UNPACK_CONTINUE == unpackret || LONGLINK_UNPACK_FALSE == unpackret) {
                    break;
                } else {
                    sendTaskMap.erase(taskid);
                    bufRecv.Move(-(int) (packlen));
                }
            }
        }
    }

#if defined(__ANDROID__) || defined(__APPLE__)
    struct tcp_info _info;
    if (getsocktcpinfo(sock, &_info) == 0) {
        char tcp_info_str[1024] = {0};
        xinfo2(TSF"task socket close getsocktcpinfo:%_", tcpinfo2str(&_info, tcp_info_str, sizeof(tcp_info_str)))
            >> close_log;
    }
#endif
}

void LongLink::__NotifySmartHeartbeatHeartReq(ConnectProfile &_profile, uint64_t _internal, uint64_t _actual_internal) {
    if (longlink_noop_interval() > 0) {
        return;
    }

    if (!mSmartHeartBeat) return;

    NoopProfile noop_profile;
    noop_profile.noop_internal = _internal;
    noop_profile.noop_actual_internal = _actual_internal;
    noop_profile.noop_starttime = ::gettickcount();
    _profile.noopProfiles.push_back(noop_profile);

    mSmartHeartBeat->OnHeartbeatStart();
}

void LongLink::__NotifySmartHeartbeatHeartResult(bool _succes, bool failTimeout, ConnectProfile &_profile) {
    if (longlink_noop_interval() > 0) {
        return;
    }

    if (!mSmartHeartBeat) return;

    if (!_profile.noopProfiles.empty()) {
        NoopProfile &noop_profile = _profile.noopProfiles.back();
        noop_profile.noop_cost = ::gettickcount() - noop_profile.noop_starttime;
        noop_profile.success = _succes;
    }
    if (mSmartHeartBeat) mSmartHeartBeat->OnHeartResult(_succes, failTimeout);
}

void LongLink::__NotifySmartHeartbeatJudgeDozeStyle() {
    if (longlink_noop_interval() > 0) {
        return;
    }

    if (!mSmartHeartBeat) return;
    mSmartHeartBeat->JudgeDozeStyle();
}

void LongLink::__NotifySmartHeartbeatConnectStatus(TLongLinkStatus status) {
    if (longlink_noop_interval() > 0) {
        return;
    }

    if (!mSmartHeartBeat) return;

    switch (status) {
        case kConnected:
            mSmartHeartBeat->OnLongLinkEstablished();
            break;

        case kConnectFailed:  // no break;
        case kDisConnected:
            mSmartHeartBeat->OnLongLinkDisconnect();
            break;

        default:
            break;
    }
}

unsigned int LongLink::__GetNextHeartbeatInterval() {
    if (longlink_noop_interval() > 0) {
        return longlink_noop_interval();
    }

    if (!mSmartHeartBeat) return MinHeartInterval;

    return mSmartHeartBeat->GetNextHeartbeatInterval();
}

