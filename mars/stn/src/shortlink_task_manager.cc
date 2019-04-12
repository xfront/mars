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
 * shortlink_task_manager.cc
 *
 *  Created on: 2012-8-24
 *      Author: zhouzhijie
 */

#include "shortlink_task_manager.h"

#include <algorithm>

#include "boost/bind.hpp"

#include "mars/app/app.h"
#include "mars/comm/thread/lock.h"
#include "mars/comm/xlogger/xlogger.h"
#include "mars/comm/time_utils.h"
#include "mars/comm/autobuffer.h"
#include "mars/comm/move_wrapper.h"
#include "mars/comm/platform_comm.h"

#ifdef ANDROID

#include "mars/comm/android/wakeuplock.h"

#endif

#include "dynamic_timeout.h"
#include "net_channel_factory.h"
#include "weak_network_logic.h"

using namespace mars::stn;
using namespace mars::app;

#define AYNC_HANDLER mAsyncReg.Get()
#define RETURN_SHORTLINK_SYNC2ASYNC_FUNC_TITLE(func, title) RETURN_SYNC2ASYNC_FUNC_TITLE(func, title, )

ShortLinkTaskManager::ShortLinkTaskManager(NetSource &netSource, DynamicTimeout &_dynamictimeout,
                                           MessageQueue::MessageQueue_t msgQueueId)
        : mAsyncReg(MessageQueue::InstallAsyncHandler(msgQueueId))
        , mNetSource(netSource)
        , mDefaultUseProxy(true)
        , mTasksContinuousFailCount(0)
        , mDynamicTimeout(_dynamictimeout)
#ifdef ANDROID
        , mWakeupLock(new WakeUpLock())
#endif
{
    xinfo_function(TSF"handler:(%_,%_)", mAsyncReg.Get().queue, mAsyncReg.Get().seq);
    xinfo2(TSF"ShortLinkTaskManager messagequeue_id=%_", MessageQueue::Handler2Queue(mAsyncReg.Get()));
}

ShortLinkTaskManager::~ShortLinkTaskManager() {
    xinfo_function();
    mAsyncReg.CancelAndWait();
    xinfo2(TSF"mTaskList count=%0", mTaskList.size());
    __BatchErrorRespHandle(kEctLocal, kEctLocalReset, kTaskFailHandleTaskEnd, Task::kInvalidTaskID, false);
#ifdef ANDROID
    delete mWakeupLock;
#endif
}

bool ShortLinkTaskManager::StartTask(const Task &task0) {
    xverbose_function();

    if (task0.sendOnly) {
        xassert2(false);
        xerror2(TSF"taskId:%_, short link should have resp", task0.taskId);
        return false;
    }

    xdebug2(TSF"taskId:%0", task0.taskId);

    TaskProfile task(task0);
    task.linkType = Task::kChannelShort;

    mTaskList.push_back(task);
    mTaskList.sort(__CompareTask);

    __RunLoop();
    return true;
}

bool ShortLinkTaskManager::StopTask(uint32_t taskId) {
    xverbose_function();

    std::list<TaskProfile>::iterator first = mTaskList.begin();
    std::list<TaskProfile>::iterator last = mTaskList.end();

    while (first != last) {
        if (taskId == first->task.taskId) {
            xinfo2(TSF"find the task, taskId:%0", taskId);

            __DeleteShortLink(first->runningId);
            mTaskList.erase(first);
            return true;
        }

        ++first;
    }

    return false;
}

bool ShortLinkTaskManager::HasTask(uint32_t taskId) const {
    xverbose_function();

    std::list<TaskProfile>::const_iterator first = mTaskList.begin();
    std::list<TaskProfile>::const_iterator last = mTaskList.end();

    while (first != last) {
        if (taskId == first->task.taskId) {
            return true;
        }
        ++first;
    }

    return false;
}

void ShortLinkTaskManager::ClearTasks() {
    xverbose_function();

    xinfo2(TSF"cmd size:%0", mTaskList.size());

    for (std::list<TaskProfile>::iterator it = mTaskList.begin(); it != mTaskList.end(); ++it) {
        __DeleteShortLink(it->runningId);
    }

    mTaskList.clear();
}

unsigned int ShortLinkTaskManager::GetTasksContinuousFailCount() {
    return mTasksContinuousFailCount;
}


void ShortLinkTaskManager::__RunLoop() {
    if (mTaskList.empty()) {
#ifdef ANDROID
        /*cancel the last wakeuplock*/
        mWakeupLock->Lock(500);
#endif
        return;
    }

    __RunOnTimeout();
    __RunOnStartTask();

    if (!mTaskList.empty()) {
#ifdef ANDROID
        mWakeupLock->Lock(60 * 1000);
#endif
        MessageQueue::FasterMessage(mAsyncReg.Get(),
                                    MessageQueue::Message((MessageQueue::MessageTitle_t) this,
                                                          boost::bind(&ShortLinkTaskManager::__RunLoop, this),
                                                          "ShortLinkTaskManager::__RunLoop"),
                                    MessageQueue::MessageTiming(1000));
    } else {
#ifdef ANDROID
        /*cancel the last wakeuplock*/
        mWakeupLock->Lock(500);
#endif
    }
}

void ShortLinkTaskManager::__RunOnTimeout() {
    xverbose2(TSF"mTaskList size=%0", mTaskList.size());
    std::list<TaskProfile>::iterator first = mTaskList.begin();
    std::list<TaskProfile>::iterator last = mTaskList.end();

    uint64_t cur_time = ::gettickcount();

    while (first != last) {
        std::list<TaskProfile>::iterator next = first;
        ++next;

        ErrCmdType err_type = kEctLocal;
        int socket_timeout_code = 0;

        if (cur_time - first->startTaskTime >= first->taskTimeout) {
            err_type = kEctLocal;
            socket_timeout_code = kEctLocalTaskTimeout;
        } else if (first->runningId && 0 < first->transferProfile.startSendTime &&
                   cur_time - first->transferProfile.startSendTime >= first->transferProfile.readWriteTimeout) {
            xerror2(TSF"task read-write timeout, taskId:%_, wworker:%_, nStartSendTime:%_, nReadWriteTimeOut:%_",
                    first->task.taskId, (void *) first->runningId, first->transferProfile.startSendTime / 1000,
                    first->transferProfile.readWriteTimeout / 1000);
            err_type = kEctHttp;
            socket_timeout_code = kEctHttpReadWriteTimeout;
        } else if (first->runningId && 0 < first->transferProfile.startSendTime &&
                   0 == first->transferProfile.lastReceivePkgTime &&
                   cur_time - first->transferProfile.startSendTime >= first->transferProfile.firstPkgTimeout) {
            xerror2(TSF"task first-pkg timeout taskId:%_, wworker:%_, nStartSendTime:%_, nfirstpkgtimeout:%_",
                    first->task.taskId, (void *) first->runningId, first->transferProfile.startSendTime / 1000,
                    first->transferProfile.firstPkgTimeout / 1000);
            err_type = kEctHttp;
            socket_timeout_code = kEctHttpFirstPkgTimeout;
        } else if (first->runningId && 0 < first->transferProfile.startSendTime &&
                   0 < first->transferProfile.lastReceivePkgTime &&
                   cur_time - first->transferProfile.lastReceivePkgTime >=
                   ((kMobile != getNetInfo()) ? kWifiPackageInterval : kGPRSPackageInterval)) {
            xerror2(TSF"task pkg-pkg timeout, taskId:%_, wworker:%_, nLastRecvTime:%_, pkg-pkg timeout:%_",
                    first->task.taskId, (void *) first->runningId,
                    first->transferProfile.lastReceivePkgTime / 1000,
                    ((kMobile != getNetInfo()) ? kWifiPackageInterval : kGPRSPackageInterval) / 1000);
            err_type = kEctHttp;
            socket_timeout_code = kEctHttpPkgPkgTimeout;
        } else {
            // pass
        }

        if (0 != socket_timeout_code) {
            std::string ip = first->runningId ? ((ShortLinkInterface *) first->runningId)->Profile().ip : "";
            std::string host = first->runningId ? ((ShortLinkInterface *) first->runningId)->Profile().host : "";
            int port = first->runningId ? ((ShortLinkInterface *) first->runningId)->Profile().port : 0;
            mDynamicTimeout.CgiTaskStatistic(first->task.cgi, kDynTimeTaskFailedPkgLen, 0);
            __SetLastFailedStatus(first);
            __SingleRespHandle(first, err_type, socket_timeout_code,
                               err_type == kEctLocal ? kTaskFailHandleTaskTimeout : kTaskFailHandleDefault, 0,
                               first->runningId ? ((ShortLinkInterface *) first->runningId)->Profile()
                                                 : ConnectProfile());
            xassert2(NotifyNetworkErrorHook);
            NotifyNetworkErrorHook(__LINE__, err_type, socket_timeout_code, ip, host, port);
        }

        first = next;
    }
}

void ShortLinkTaskManager::__RunOnStartTask() {
    std::list<TaskProfile>::iterator first = mTaskList.begin();
    std::list<TaskProfile>::iterator last = mTaskList.end();

    bool ismakesureauthruned = false;
    bool ismakesureauthsuccess = false;
    uint64_t curtime = ::gettickcount();
    int sent_count = 0;

    while (first != last) {
        std::list<TaskProfile>::iterator next = first;
        ++next;

        if (first->runningId) {
            ++sent_count;
            first = next;
            continue;
        }

        //重试间隔
        if (first->retryTimeInterval > curtime - first->retryStartTime) {
            xdebug2(TSF"retry interval, taskId:%0, task retry late task, wait:%1", first->task.taskId,
                    (curtime - first->transferProfile.loopStartTaskTime) / 1000);
            first = next;
            continue;
        }

        // make sure login
        if (first->task.needAuthed) {

            if (!ismakesureauthruned) {
                ismakesureauthruned = true;
                ismakesureauthsuccess = MakesureAuthed();
            }

            if (!ismakesureauthsuccess) {
                xinfo2_if(curtime % 3 == 1, TSF"makeSureAuth retsult=%0", ismakesureauthsuccess);
                first = next;
                continue;
            }
        }

        AutoBuffer bufreq;
        AutoBuffer buffer_extension;
        int error_code = 0;

        if (!Req2Buf(first->task.taskId, first->task.userContext, bufreq, buffer_extension, error_code,
                     Task::kChannelShort)) {
            __SingleRespHandle(first, kEctEnDecode, error_code, kTaskFailHandleTaskEnd, 0,
                               first->runningId ? ((ShortLinkInterface *) first->runningId)->Profile()
                                                 : ConnectProfile());
            first = next;
            continue;
        }

        //雪崩检测
        xassert2(AntiAvalancheCheckHook);

        if (!AntiAvalancheCheckHook(first->task, bufreq.Ptr(), (int) bufreq.Length())) {
            __SingleRespHandle(first, kEctLocal, kEctLocalAntiAvalanche, kTaskFailHandleTaskEnd, 0,
                               first->runningId ? ((ShortLinkInterface *) first->runningId)->Profile()
                                                 : ConnectProfile());
            first = next;
            continue;
        }

        first->transferProfile.loopStartTaskTime = ::gettickcount();
        first->transferProfile.firstPkgTimeout = __FirstPkgTimeout(first->task.serverProcessCost, bufreq.Length(),
                                                                      sent_count, mDynamicTimeout.GetStatus());
        first->currentDyntimeStatus = (first->task.serverProcessCost <= 0) ? mDynamicTimeout.GetStatus()
                                                                               : kEValuating;
        first->transferProfile.readWriteTimeout = __ReadWriteTimeout(first->transferProfile.firstPkgTimeout);
        first->transferProfile.sendDataSize = bufreq.Length();

        first->useProxy = (first->remainRetryCount == 0 && first->task.retryCount > 0) ? !mDefaultUseProxy
                                                                                           : mDefaultUseProxy;
        ShortLinkInterface *worker = ShortLinkChannelFactory::Create(MessageQueue::Handler2Queue(mAsyncReg.Get()),
                                                                     mNetSource, first->task, first->useProxy);
        worker->OnSend.set(boost::bind(&ShortLinkTaskManager::__OnSend, this, _1), AYNC_HANDLER);
        worker->OnRecv.set(boost::bind(&ShortLinkTaskManager::__OnRecv, this, _1, _2, _3), AYNC_HANDLER);
        worker->OnResponse.set(boost::bind(&ShortLinkTaskManager::__OnResponse, this, _1, _2, _3, _4, _5, _6, _7),
                               AYNC_HANDLER);
        first->runningId = (intptr_t) worker;

        xassert2(worker && first->runningId);
        if (!first->runningId) {
            xwarn2(TSF"task add into shortlink readwrite fail cgi:%_, cmdid:%_, taskId:%_", first->task.cgi,
                   first->task.cmdId, first->task.taskId);
            first = next;
            continue;
        }

        worker->OnNetReport.set(NotifyNetworkErrorHook);
        worker->SendRequest(bufreq, buffer_extension);

        xinfo2(TSF"task add into shortlink readwrite cgi:%_, cmdid:%_, taskId:%_, work:%_, size:%_, timeout(firstpkg:%_, rw:%_, task:%_), retry:%_, useProxy:%_",
               first->task.cgi, first->task.cmdId, first->task.taskId, (ShortLinkInterface *) first->runningId,
               first->transferProfile.sendDataSize, first->transferProfile.firstPkgTimeout / 1000,
               first->transferProfile.readWriteTimeout / 1000, first->taskTimeout / 1000, first->remainRetryCount,
               first->useProxy);
        ++sent_count;
        first = next;
    }
}

struct find_seq {
public:
    bool operator()(const TaskProfile &value) { return p_worker == (ShortLinkInterface *) value.runningId; }

public:
    ShortLinkInterface *p_worker;
};

void
ShortLinkTaskManager::__OnResponse(ShortLinkInterface *worker, ErrCmdType errType, int status, AutoBuffer &body,
                                   AutoBuffer &extension, bool cancelRetry, ConnectProfile &connProfile) {

    xdebug2(TSF"worker=%0, errType=%1, status=%2, body.lenght=%3, cancelRetry=%4", worker, errType,
            status, body.Length(), cancelRetry);

    ShortLinkRspHook(status);

    std::list<TaskProfile>::iterator it = __LocateBySeq(
            (intptr_t) worker);    // must used iter pWorker, not used aSelf. aSelf may be destroy already

    if (mTaskList.end() == it) {
        xerror2(TSF"task no found: status:%_, worker:%_", status, worker);
        return;
    }

    if (errType != kEctOK) {
        if (errType == kEctSocket && status == kEctSocketMakeSocketPrepared) {
            mDynamicTimeout.CgiTaskStatistic(it->task.cgi, kDynTimeTaskFailedPkgLen, 0);
            __SetLastFailedStatus(it);
        }

        if (errType == kEctSocket) {
            it->forceNoRetry = cancelRetry;
        }
        __SingleRespHandle(it, errType, status, kTaskFailHandleDefault, body.Length(), connProfile);
        return;

    }

    it->transferProfile.receivedSize = body.Length();
    it->transferProfile.receiveDataSize = body.Length();
    it->transferProfile.lastReceivePkgTime = ::gettickcount();

    int err_code = 0;
    int handle_type = Buf2Resp(it->task.taskId, it->task.userContext, body, extension, err_code,
                               Task::kChannelShort);

    switch (handle_type) {
        case kTaskFailHandleNoError: {
            mDynamicTimeout.CgiTaskStatistic(it->task.cgi, (unsigned int) it->transferProfile.sendDataSize +
                                                            (unsigned int) body.Length(),
                                              ::gettickcount() - it->transferProfile.startSendTime);
            __SingleRespHandle(it, kEctOK, err_code, handle_type, (unsigned int) it->transferProfile.receiveDataSize,
                               connProfile);
            xassert2(NotifyNetworkErrorHook);
            NotifyNetworkErrorHook(__LINE__, kEctOK, err_code, connProfile.ip, connProfile.host,
                                    connProfile.port);
        }
            break;
        case kTaskFailHandleSessionTimeout: {
            xassert2(NotifyRetryAllTasksHook);
            xwarn2(TSF"task decode error session timeout taskId:%_, cmdid:%_, cgi:%_", it->task.taskId,
                   it->task.cmdId, it->task.cgi);
            NotifyRetryAllTasksHook(kEctEnDecode, err_code, handle_type, it->task.taskId);
        }
            break;
        case kTaskFailHandleRetryAllTasks: {
            xassert2(NotifyRetryAllTasksHook);
            xwarn2(TSF"task decode error retry all task taskId:%_, cmdid:%_, cgi:%_", it->task.taskId,
                   it->task.cmdId, it->task.cgi);
            NotifyRetryAllTasksHook(kEctEnDecode, err_code, handle_type, it->task.taskId);
        }
            break;
        case kTaskFailHandleTaskEnd: {
            __SingleRespHandle(it, kEctEnDecode, err_code, handle_type,
                               (unsigned int) it->transferProfile.receiveDataSize, connProfile);
        }
            break;
        case kTaskFailHandleDefault: {
            xerror2(TSF"task decode error handle_type:%_, err_code:%_, pWorker:%_, taskId:%_ body dump:%_",
                    handle_type, err_code, (void *) it->runningId, it->task.taskId,
                    xdump(body.Ptr(), body.Length()));
            __SingleRespHandle(it, kEctEnDecode, err_code, handle_type,
                               (unsigned int) it->transferProfile.receiveDataSize, connProfile);
            xassert2(NotifyNetworkErrorHook);
            NotifyNetworkErrorHook(__LINE__, kEctEnDecode, handle_type, connProfile.ip, connProfile.host,
                                    connProfile.port);
        }
            break;
        default: {
            xassert2(false, TSF"task decode error fail_handle:%_, taskId:%_", handle_type, it->task.taskId);
            __SingleRespHandle(it, kEctEnDecode, err_code, handle_type,
                               (unsigned int) it->transferProfile.receiveDataSize, connProfile);
            xassert2(NotifyNetworkErrorHook);
            NotifyNetworkErrorHook(__LINE__, kEctEnDecode, handle_type, connProfile.ip, connProfile.host,
                                    connProfile.port);
            break;
        }

    }
}

void ShortLinkTaskManager::__OnSend(ShortLinkInterface *worker) {

    std::list<TaskProfile>::iterator it = __LocateBySeq((intptr_t) worker);

    if (mTaskList.end() != it) {
        if (it->transferProfile.firstStartSendTime == 0)
            it->transferProfile.firstStartSendTime = ::gettickcount();
        it->transferProfile.startSendTime = ::gettickcount();
        xdebug2(TSF"taskId:%_, worker:%_, nStartSendTime:%_", it->task.taskId, worker,
                it->transferProfile.startSendTime / 1000);
    }
}

void ShortLinkTaskManager::__OnRecv(ShortLinkInterface *worker, unsigned int cachedSize, unsigned int totalSize) {

    xverbose_function();
    std::list<TaskProfile>::iterator it = __LocateBySeq((intptr_t) worker);

    if (mTaskList.end() != it) {
        if (it->transferProfile.lastReceivePkgTime == 0)
            WeakNetworkLogic::Singleton::Instance()->OnPkgEvent(true, (int) (::gettickcount() -
                                                                             it->transferProfile.startSendTime));
        else
            WeakNetworkLogic::Singleton::Instance()->OnPkgEvent(false, (int) (::gettickcount() -
                                                                              it->transferProfile.lastReceivePkgTime));
        it->transferProfile.lastReceivePkgTime = ::gettickcount();
        it->transferProfile.receivedSize = cachedSize;
        it->transferProfile.receiveDataSize = totalSize;
        xdebug2(TSF"worker:%_, last_recvtime:%_, cachedsize:%_, totalsize:%_", worker,
                it->transferProfile.lastReceivePkgTime / 1000, cachedSize, totalSize);
    } else {
        xwarn2(TSF"not found worker:%_", worker);
    }
}

void ShortLinkTaskManager::RedoTasks() {
    xinfo_function();

    std::list<TaskProfile>::iterator first = mTaskList.begin();
    std::list<TaskProfile>::iterator last = mTaskList.end();

    while (first != last) {
        std::list<TaskProfile>::iterator next = first;
        ++next;

        first->lastFailedDyntimeStatus = 0;

        if (first->runningId) {
            xinfo2(TSF"task redo, taskId:%_", first->task.taskId);
            __SingleRespHandle(first, kEctLocal, kEctLocalCancel, kTaskFailHandleDefault, 0,
                               ((ShortLinkInterface *) first->runningId)->Profile());
        }

        first = next;
    }

    __RunLoop();
}

void ShortLinkTaskManager::RetryTasks(ErrCmdType errType, int errCode, int failHandle, uint32_t srcTaskId) {
    xverbose_function();
    __BatchErrorRespHandle(errType, errCode, failHandle, srcTaskId);
    __RunLoop();
}

void ShortLinkTaskManager::__BatchErrorRespHandle(ErrCmdType errType, int errCode, int failHandle,
                                                  uint32_t srcTaskId, bool callbackRunningTaskOnly) {
    xassert2(kEctOK != errType);
    xdebug2(TSF"ect=%0, errcode=%1", errType, errCode);

    std::list<TaskProfile>::iterator first = mTaskList.begin();
    std::list<TaskProfile>::iterator last = mTaskList.end();

    while (first != last) {
        std::list<TaskProfile>::iterator next = first;
        ++next;

        if (callbackRunningTaskOnly && !first->runningId) {
            first = next;
            continue;
        }

        if (failHandle == kTaskFailHandleSessionTimeout && !first->task.needAuthed) {
            first = next;
            continue;
        }

        if (srcTaskId == Task::kInvalidTaskID || srcTaskId == first->task.taskId)
            __SingleRespHandle(first, errType, errCode, failHandle, 0,
                               first->runningId ? ((ShortLinkInterface *) first->runningId)->Profile()
                                                 : ConnectProfile());
        else
            __SingleRespHandle(first, errType, 0, failHandle, 0,
                               first->runningId ? ((ShortLinkInterface *) first->runningId)->Profile()
                                                 : ConnectProfile());

        first = next;
    }
}

bool ShortLinkTaskManager::__SingleRespHandle(std::list<TaskProfile>::iterator it, ErrCmdType errType, int errCode,
                                              int failHandle, size_t _resp_length,
                                              const ConnectProfile &connectProfile) {
    xverbose_function();
    xassert2(kEctServer != errType);
    xassert2(it != mTaskList.end());

    if (it == mTaskList.end()) return false;

    if (kEctOK == errType) {
        mTasksContinuousFailCount = 0;
        mDefaultUseProxy = it->useProxy;
    } else {
        ++mTasksContinuousFailCount;
    }

    uint64_t curtime = gettickcount();
    it->transferProfile.connectProfile = connectProfile;

    xassert2((kEctOK == errType) == (kTaskFailHandleNoError == failHandle), TSF"type:%_, handle:%_", errType, failHandle);

    if (it->forceNoRetry || 0 >= it->remainRetryCount || kEctOK == errType ||
        kTaskFailHandleTaskEnd == failHandle || kTaskFailHandleTaskTimeout == failHandle) {
        xlog2(kEctOK == errType ? kLevelInfo : kLevelWarn, TSF"task end callback short cmdid:%_, err(%_, %_, %_), ", it->task.cmdId, errType, errCode,
              failHandle)
            (TSF "svr(%_:%_, %_, %_), ", connectProfile.ip, connectProfile.port,
             IPSourceTypeString[connectProfile.ipType], connectProfile.host)
                    (TSF "cli(%_, %_, n:%_, sig:%_), ", it->transferProfile.externalIp, connectProfile.localIp,
                     connectProfile.netType, connectProfile.disconnSignal)
                    (TSF "cost(s:%_, r:%_%_%_, c:%_, rw:%_), all:%_, retry:%_, ", it->transferProfile.sendDataSize,
                     0 != _resp_length ? _resp_length : it->transferProfile.receiveDataSize,
                     0 != _resp_length ? "" : "/",
                     0 != _resp_length ? "" : string_cast(it->transferProfile.receivedSize).str(),
                     connectProfile.connRtt,
                     (it->transferProfile.startSendTime == 0 ? 0 : curtime - it->transferProfile.startSendTime),
                     (curtime - it->startTaskTime), it->remainRetryCount)
                    (TSF "cgi:%_, taskId:%_, worker:%_", it->task.cgi, it->task.taskId,
                     (ShortLinkInterface *) it->runningId);

        int cgi_retcode = CallResultHook(errType, errCode, failHandle, it->task,
                                        (unsigned int) (curtime - it->startTaskTime));
        int errcode = errCode;

        if (it->runningId) {
            if (kEctOK == errType) {
                errcode = cgi_retcode;
            }
        }

        it->endTaskTime = ::gettickcount();
        it->errType = errType;
        it->transferProfile.errorType = errType;
        it->errCode = errcode;
        it->transferProfile.errorCode = errCode;
        it->PushHistory();
        ReportTaskProfile(*it);
        WeakNetworkLogic::Singleton::Instance()->OnTaskEvent(*it);

        __DeleteShortLink(it->runningId);

        mTaskList.erase(it);

        return true;
    }


    xlog2(kEctOK == errType ? kLevelInfo : kLevelWarn, TSF"task end retry short cmdid:%_, err(%_, %_, %_), ", it->task.cmdId, errType, errCode, failHandle)
        (TSF "svr(%_:%_, %_, %_), ", connectProfile.ip, connectProfile.port,
         IPSourceTypeString[connectProfile.ipType], connectProfile.host)
                (TSF "cli(%_, n:%_, sig:%_), ", connectProfile.localIp, connectProfile.netType,
                 connectProfile.disconnSignal)
                (TSF "cost(s:%_, r:%_%_%_, c:%_, rw:%_), all:%_, retry:%_, ", it->transferProfile.sendDataSize,
                 0 != _resp_length ? _resp_length : it->transferProfile.receivedSize,
                 0 != _resp_length ? "" : "/",
                 0 != _resp_length ? "" : string_cast(it->transferProfile.receiveDataSize).str(),
                 connectProfile.connRtt,
                 (it->transferProfile.startSendTime == 0 ? 0 : curtime - it->transferProfile.startSendTime),
                 (curtime - it->startTaskTime), it->remainRetryCount)
                (TSF "cgi:%_, taskId:%_, worker:%_", it->task.cgi, it->task.taskId, (void *) it->runningId);

    it->remainRetryCount--;
    it->transferProfile.errorType = errType;
    it->transferProfile.errorCode = errCode;

    __DeleteShortLink(it->runningId);
    it->PushHistory();
    it->InitSendParam();

    it->retryStartTime = ::gettickcount();
    // session timeout 应该立刻重试
    if (kTaskFailHandleSessionTimeout == failHandle) {
        it->retryStartTime = 0;
    }

    it->retryTimeInterval = DEF_TASK_RETRY_INTERNAL;

    return false;
}

std::list<TaskProfile>::iterator ShortLinkTaskManager::__LocateBySeq(intptr_t runningId) {
    if (!runningId) return mTaskList.end();

    find_seq find_functor;
    find_functor.p_worker = (ShortLinkInterface *) runningId;
    std::list<TaskProfile>::iterator it = std::find_if(mTaskList.begin(), mTaskList.end(), find_functor);

    return it;
}

void ShortLinkTaskManager::__DeleteShortLink(intptr_t &runningId) {
    if (!runningId) return;
    ShortLinkInterface *p_shortlink = (ShortLinkInterface *) runningId;
    ShortLinkChannelFactory::Destory(p_shortlink);
    MessageQueue::CancelMessage(mAsyncReg.Get(), p_shortlink);
    p_shortlink = NULL;
}

ConnectProfile ShortLinkTaskManager::GetConnectProfile(uint32_t taskId) const {
    std::list<TaskProfile>::const_iterator first = mTaskList.begin();
    std::list<TaskProfile>::const_iterator last = mTaskList.end();

    while (first != last) {
        if (taskId == first->task.taskId) {
            return ((ShortLinkInterface *) (first->runningId))->Profile();
        }
        ++first;
    }
    return ConnectProfile();
}

