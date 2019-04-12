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
 * longlink_task_manager.cc
 *
 *  Created on: 2012-7-17
 *      Author: yerungui
 */

#include "longlink_task_manager.h"

#include <algorithm>

#include "boost/bind.hpp"

#include "mars/comm/thread/lock.h"
#include "mars/comm/xlogger/xlogger.h"
#include "mars/comm/time_utils.h"
#include "mars/comm/autobuffer.h"
#include "mars/comm/move_wrapper.h"
#include "mars/comm/platform_comm.h"

#ifdef ANDROID

#include "mars/comm/android/wakeuplock.h"

#endif

#include "mars/stn/config.h"
#include "mars/stn/task_profile.h"
#include "mars/stn/proto/longlink_packer.h"

#include "dynamic_timeout.h"
#include "net_channel_factory.h"
#include "weak_network_logic.h"

using namespace mars::stn;

#define AYNC_HANDLER mAsyncReg.Get()
#define RETURN_LONKLINK_SYNC2ASYNC_FUNC(func) RETURN_SYNC2ASYNC_FUNC(func, )

LongLinkTaskManager::LongLinkTaskManager(NetSource &netSource, ActiveLogic &activeLogic,
                                         DynamicTimeout &_dynamictimeout, MessageQueue::MessageQueue_t msgQueueId)
        : mAsyncReg(MessageQueue::InstallAsyncHandler(msgQueueId))
        , mLastBatchErrorTime(0)
        , mRetryInterval(0)
        , mTasksContinuousFailCount(0)
        , mLongLink(LongLinkChannelFactory::Create(msgQueueId, netSource))
        , mLongLinkConnectMon(new LongLinkConnectMonitor(activeLogic, *mLongLink, msgQueueId))
        , mDynamicTimeout(_dynamictimeout)
#ifdef ANDROID
        , mWakeupLock(new WakeUpLock())
#endif
{
    xinfo_function(TSF"handler:(%_,%_)", mAsyncReg.Get().queue, mAsyncReg.Get().seq);
    mLongLink->OnSend = boost::bind(&LongLinkTaskManager::__OnSend, this, _1);
    mLongLink->OnRecv = boost::bind(&LongLinkTaskManager::__OnRecv, this, _1, _2, _3);
    mLongLink->OnResponse = boost::bind(&LongLinkTaskManager::__OnResponse, this, _1, _2, _3, _4, _5, _6, _7);
    mLongLink->SignalConnection.connect(boost::bind(&LongLinkTaskManager::__SignalConnection, this, _1));
}

LongLinkTaskManager::~LongLinkTaskManager() {
    xinfo_function();
    mLongLink->SignalConnection.disconnect(boost::bind(&LongLinkTaskManager::__SignalConnection, this, _1));
    mAsyncReg.CancelAndWait();

    __BatchErrorRespHandle(kEctLocal, kEctLocalReset, kTaskFailHandleTaskEnd, Task::kInvalidTaskID,
                           mLongLink->Profile(), false);

    delete mLongLinkConnectMon;
    LongLinkChannelFactory::Destory(mLongLink);
#ifdef ANDROID
    delete mWakeupLock;
#endif
}

bool LongLinkTaskManager::StartTask(const Task &task0) {
    xverbose_function();
    xdebug2(TSF"taskId=%0", task0.taskId);

    TaskProfile task(task0);
    task.linkType = Task::kChannelLong;

    mTaskList.push_back(task);
    mTaskList.sort(__CompareTask);

    __RunLoop();
    return true;
}

bool LongLinkTaskManager::StopTask(uint32_t taskId) {
    xverbose_function();

    std::list<TaskProfile>::iterator first = mTaskList.begin();
    std::list<TaskProfile>::iterator last = mTaskList.end();

    while (first != last) {
        if (taskId == first->task.taskId) {
            xinfo2(TSF"find the task taskId:%0", taskId);

            mLongLink->Stop(first->task.taskId);
            mTaskList.erase(first);
            return true;
        }

        ++first;
    }

    return false;
}

bool LongLinkTaskManager::HasTask(uint32_t taskId) const {
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

void LongLinkTaskManager::ClearTasks() {
    xverbose_function();
    mLongLink->Disconnect(LongLink::kReset);
    MessageQueue::CancelMessage(mAsyncReg.Get(), 0);
    mTaskList.clear();
}

unsigned int LongLinkTaskManager::GetTaskCount() {
    return (unsigned int) mTaskList.size();
}

unsigned int LongLinkTaskManager::GetTasksContinuousFailCount() {
    return mTasksContinuousFailCount;
}

void LongLinkTaskManager::RedoTasks() {
    xinfo_function();

    std::list<TaskProfile>::iterator first = mTaskList.begin();
    std::list<TaskProfile>::iterator last = mTaskList.end();

    while (first != last) {
        std::list<TaskProfile>::iterator next = first;
        ++next;

        first->lastFailedDyntimeStatus = 0;
        if (first->runningId) {
            xinfo2(TSF"task redo, taskId:%_", first->task.taskId);
            __SingleRespHandle(first, kEctLocal, kEctLocalCancel, kTaskFailHandleDefault, mLongLink->Profile());
        }

        first = next;
    }

    mRetryInterval = 0;

    MessageQueue::CancelMessage(mAsyncReg.Get(), 0);
    __RunLoop();
}

void LongLinkTaskManager::RetryTasks(ErrCmdType errType, int errCode, int failHandle, uint32_t srcTaskId) {
    xverbose_function();
    __BatchErrorRespHandle(errType, errCode, failHandle, srcTaskId, mLongLink->Profile());
    __RunLoop();
}


void LongLinkTaskManager::__RunLoop() {

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
        mWakeupLock->Lock(30 * 1000);
#endif
        MessageQueue::FasterMessage(mAsyncReg.Get(),
                                    MessageQueue::Message((MessageQueue::MessageTitle_t) this,
                                                          boost::bind(&LongLinkTaskManager::__RunLoop, this),
                                                          "LongLinkTaskManager::__RunLoop"),
                                    MessageQueue::MessageTiming(1000));
    } else {
#ifdef ANDROID
        /*cancel the last wakeuplock*/
        mWakeupLock->Lock(500);
#endif
    }
}

void LongLinkTaskManager::__RunOnTimeout() {
    std::list<TaskProfile>::iterator first = mTaskList.begin();
    std::list<TaskProfile>::iterator last = mTaskList.end();

    uint64_t cur_time = ::gettickcount();
    int socket_timeout_code = 0;
    uint32_t src_taskid = Task::kInvalidTaskID;
    bool istasktimeout = false;

    while (first != last) {
        std::list<TaskProfile>::iterator next = first;
        ++next;

        if (first->runningId && 0 < first->transferProfile.startSendTime) {
            if (0 == first->transferProfile.lastReceivePkgTime &&
                cur_time - first->transferProfile.startSendTime >= first->transferProfile.firstPkgTimeout) {
                xerror2(TSF"task first-pkg timeout taskId:%_,  nStartSendTime=%_, nfirstpkgtimeout=%_",
                        first->task.taskId, first->transferProfile.startSendTime / 1000,
                        first->transferProfile.firstPkgTimeout / 1000);
                socket_timeout_code = kEctLongFirstPkgTimeout;
                src_taskid = first->task.taskId;
                __SetLastFailedStatus(first);
            }

            if (0 < first->transferProfile.lastReceivePkgTime &&
                cur_time - first->transferProfile.lastReceivePkgTime >=
                ((kMobile != getNetInfo()) ? kWifiPackageInterval : kGPRSPackageInterval)) {
                xerror2(TSF"task pkg-pkg timeout, taskId:%_, nLastRecvTime=%_, pkg-pkg timeout=%_",
                        first->task.taskId, first->transferProfile.lastReceivePkgTime / 1000,
                        ((kMobile != getNetInfo()) ? kWifiPackageInterval : kGPRSPackageInterval) / 1000);
                socket_timeout_code = kEctLongPkgPkgTimeout;
                src_taskid = first->task.taskId;
            }

            if (cur_time - first->transferProfile.startSendTime >= first->transferProfile.readWriteTimeout) {
                xerror2(TSF"task read-write timeout, taskId:%_, , nStartSendTime=%_, nReadWriteTimeOut=%_",
                        first->task.taskId, first->transferProfile.startSendTime / 1000,
                        first->transferProfile.readWriteTimeout / 1000);
                socket_timeout_code = kEctLongReadWriteTimeout;
                src_taskid = first->task.taskId;
            }
        }

        if (cur_time - first->startTaskTime >= first->taskTimeout) {
            xerror2(TSF"task timeout, taskId:%_, nStartSendTime=%_, cur_time=%_, timeout:%_",
                    first->task.taskId, first->transferProfile.startSendTime / 1000, cur_time / 1000,
                    first->taskTimeout / 1000);
            __SingleRespHandle(first, kEctLocal, kEctLocalTaskTimeout, kTaskFailHandleTaskTimeout,
                               mLongLink->Profile());
            istasktimeout = true;
        }

        first = next;
    }

    if (0 != socket_timeout_code) {
        mDynamicTimeout.CgiTaskStatistic("", kDynTimeTaskFailedPkgLen, 0);
        __BatchErrorRespHandle(kEctNetMsgXP, socket_timeout_code, kTaskFailHandleDefault, src_taskid,
                               mLongLink->Profile());
        xassert2(NotifyNetworkErrorHook);
        NotifyNetworkErrorHook(__LINE__, kEctNetMsgXP, socket_timeout_code, mLongLink->Profile().ip,
                                mLongLink->Profile().port);
    } else if (istasktimeout) {
        __BatchErrorRespHandle(kEctNetMsgXP, kEctLocalTaskTimeout, kTaskFailHandleDefault, src_taskid,
                               mLongLink->Profile());
    }
}

void LongLinkTaskManager::__RunOnStartTask() {
    std::list<TaskProfile>::iterator first = mTaskList.begin();
    std::list<TaskProfile>::iterator last = mTaskList.end();

    bool ismakesureauthruned = false;
    bool ismakesureauthsuccess = false;
    uint64_t curtime = ::gettickcount();

    bool canretry = curtime - mLastBatchErrorTime >= mRetryInterval;
    bool canprint = true;
    int sent_count = 0;

    while (first != last) {
        std::list<TaskProfile>::iterator next = first;
        ++next;

        if (first->runningId) {
            ++sent_count;
            first = next;
            continue;
        }

        //重试间隔, 不影响第一次发送的任务
        if (first->task.retryCount > first->remainRetryCount && !canretry) {
            xdebug2_if(canprint, TSF"retry interval:%0, curtime:%1, mLastBatchErrorTime:%2, curtime-m_lastbatcherrortime:%3",
                       mRetryInterval, curtime, mLastBatchErrorTime, curtime - mLastBatchErrorTime);

            canprint = false;
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
                xinfo2_if(curtime % 3 == 0, TSF"makeSureAuth retsult=%0", ismakesureauthsuccess);
                first = next;
                continue;
            }
        }

        AutoBuffer bufreq;
        AutoBuffer buffer_extension;
        int error_code = 0;

        if (!first->antiAvalancheChecked) {
            if (!Req2Buf(first->task.taskId, first->task.userContext, bufreq, buffer_extension, error_code,
                         Task::kChannelLong)) {
                __SingleRespHandle(first, kEctEnDecode, error_code, kTaskFailHandleTaskEnd, mLongLink->Profile());
                first = next;
                continue;
            }
            // 雪崩检测
            xassert2(AntiAvalancheCheckHook);
            if (!AntiAvalancheCheckHook(first->task, bufreq.Ptr(), (int) bufreq.Length())) {
                __SingleRespHandle(first, kEctLocal, kEctLocalAntiAvalanche, kTaskFailHandleTaskEnd,
                                   mLongLink->Profile());
                first = next;
                continue;
            }
            first->antiAvalancheChecked = true;
        }

        xassert2(first->antiAvalancheChecked);
        if (!mLongLinkConnectMon->MakeSureConnected()) {
            if (0 != first->task.channelId) {
                __SingleRespHandle(first, kEctLocal, kEctLocalChannelID, kTaskFailHandleTaskEnd, mLongLink->Profile());
            }

            first = next;
            continue;
        }

        if (0 != first->task.channelId && mLongLink->Profile().startTime != first->task.channelId) {
            __SingleRespHandle(first, kEctLocal, kEctLocalChannelID, kTaskFailHandleTaskEnd, mLongLink->Profile());
            first = next;
            continue;
        }

        if (0 == bufreq.Length()) {
            if (!Req2Buf(first->task.taskId, first->task.userContext, bufreq, buffer_extension, error_code,
                         Task::kChannelLong)) {
                __SingleRespHandle(first, kEctEnDecode, error_code, kTaskFailHandleTaskEnd, mLongLink->Profile());
                first = next;
                continue;
            }
            // 雪崩检测
            xassert2(AntiAvalancheCheckHook);
            if (!AntiAvalancheCheckHook(first->task, bufreq.Ptr(), (int) bufreq.Length())) {
                __SingleRespHandle(first, kEctLocal, kEctLocalAntiAvalanche, kTaskFailHandleTaskEnd,
                                   mLongLink->Profile());
                first = next;
                continue;
            }
        }

        first->transferProfile.loopStartTaskTime = ::gettickcount();
        first->transferProfile.firstPkgTimeout = __FirstPkgTimeout(first->task.serverProcessCost, bufreq.Length(),
                                                                      sent_count, mDynamicTimeout.GetStatus());
        first->currentDyntimeStatus = (first->task.serverProcessCost <= 0) ? mDynamicTimeout.GetStatus()
                                                                               : kEValuating;
        first->transferProfile.readWriteTimeout = __ReadWriteTimeout(first->transferProfile.firstPkgTimeout);
        first->transferProfile.sendDataSize = bufreq.Length();
        first->runningId = mLongLink->Send(bufreq, buffer_extension, first->task);

        if (!first->runningId) {
            xwarn2(TSF"task add into longlink readwrite fail cgi:%_, cmdid:%_, taskId:%_", first->task.cgi,
                   first->task.cmdId, first->task.taskId);
            first = next;
            continue;
        }

        xinfo2(TSF"task add into longlink readwrite suc cgi:%_, cmdid:%_, taskId:%_, size:%_, timeout(firstpkg:%_, rw:%_, task:%_), retry:%_, curtime:%_, start_send_time:%_,",
               first->task.cgi, first->task.cmdId, first->task.taskId, first->transferProfile.sendDataSize,
               first->transferProfile.firstPkgTimeout / 1000,
               first->transferProfile.readWriteTimeout / 1000, first->taskTimeout / 1000, first->remainRetryCount,
               curtime, first->startTaskTime);

        if (first->task.sendOnly) {
            __SingleRespHandle(first, kEctOK, 0, kTaskFailHandleNoError, mLongLink->Profile());
        }

        ++sent_count;
        first = next;
    }
}

bool LongLinkTaskManager::__SingleRespHandle(std::list<TaskProfile>::iterator it, ErrCmdType errType, int errCode,
                                             int failHandle, const ConnectProfile &connectProfile) {
    xverbose_function();
    xassert2(kEctServer != errType);
    xassert2(it != mTaskList.end());

    if (it == mTaskList.end())return false;

    it->transferProfile.connectProfile = connectProfile;

    if (kEctOK == errType) {
        mRetryInterval = 0;
        mTasksContinuousFailCount = 0;
    } else {
        ++mTasksContinuousFailCount;
    }

    uint64_t curtime = gettickcount();
    size_t receive_data_size = it->transferProfile.receiveDataSize;
    size_t received_size = it->transferProfile.receivedSize;

    xassert2((kEctOK == errType) == (kTaskFailHandleNoError == failHandle), TSF"type:%_, handle:%_", errType, failHandle);

    if (0 >= it->remainRetryCount || kEctOK == errType || kTaskFailHandleTaskEnd == failHandle ||
        kTaskFailHandleTaskTimeout == failHandle) {
        xlog2(kEctOK == errType ? kLevelInfo : kLevelWarn, TSF"task end callback  long cmdid:%_, err(%_, %_, %_), ", it->task.cmdId, errType, errCode,
              failHandle)
            (TSF "svr(%_:%_, %_, %_), ", connectProfile.ip, connectProfile.port,
             IPSourceTypeString[connectProfile.ipType], connectProfile.host)
                    (TSF "cli(%_, %_, n:%_, sig:%_), ", it->transferProfile.externalIp, connectProfile.localIp,
                     connectProfile.netType, connectProfile.disconnSignal)
                    (TSF "cost(s:%_, r:%_%_%_, c:%_, rw:%_), all:%_, retry:%_, ", it->transferProfile.sendDataSize,
                     receive_data_size - received_size ? string_cast(received_size).str() : "",
                     receive_data_size - received_size ? "/" : "", receive_data_size, connectProfile.connRtt,
                     (it->transferProfile.startSendTime == 0 ? 0 : curtime - it->transferProfile.startSendTime),
                     (curtime - it->startTaskTime), it->remainRetryCount)
                    (TSF "cgi:%_, taskId:%_, tid:%_", it->task.cgi, it->task.taskId, connectProfile.tid);

        int cgi_retcode = CallResultHook(errType, errCode, failHandle, it->task,
                                        (unsigned int) (curtime - it->startTaskTime));
        int errcode = errCode;

        if (!it->task.sendOnly && it->runningId) {
            if (kEctOK == errType) {
                errcode = cgi_retcode;
            }
        }

        it->endTaskTime = ::gettickcount();
        it->errCode = errcode;
        it->errType = errType;
        it->transferProfile.errorType = errType;
        it->transferProfile.errorCode = errCode;
        it->PushHistory();
        ReportTaskProfile(*it);
        WeakNetworkLogic::Singleton::Instance()->OnTaskEvent(*it);

        mTaskList.erase(it);
        return true;
    }

    xlog2(kEctOK == errType ? kLevelInfo : kLevelWarn, TSF"task end retry  long cmdid:%_, err(%_, %_, %_), ", it->task.cmdId, errType, errCode, failHandle)
        (TSF "svr(%_:%_, %_, %_), ", connectProfile.ip, connectProfile.port,
         IPSourceTypeString[connectProfile.ipType], connectProfile.host)
                (TSF "cli(%_, %_, n:%_, sig:%_), ", it->transferProfile.externalIp, connectProfile.localIp,
                 connectProfile.netType, connectProfile.disconnSignal)
                (TSF "cost(s:%_, r:%_%_%_, c:%_, rw:%_), all:%_, retry:%_, ", it->transferProfile.sendDataSize,
                 receive_data_size - received_size ? string_cast(received_size).str() : "",
                 receive_data_size - received_size ? "/" : "", receive_data_size, connectProfile.connRtt,
                 (it->transferProfile.startSendTime == 0 ? 0 : curtime - it->transferProfile.startSendTime),
                 (curtime - it->startTaskTime), it->remainRetryCount)
                (TSF "cgi:%_, taskId:%_, tid:%_", it->task.cgi, it->task.taskId, connectProfile.tid);

    it->remainRetryCount--;
    it->transferProfile.errorType = errType;
    it->transferProfile.errorCode = errCode;
    it->PushHistory();
    it->InitSendParam();

    return false;
}

void
LongLinkTaskManager::__BatchErrorRespHandle(ErrCmdType errType, int errCode, int failHandle, uint32_t srcTaskId,
                                            const ConnectProfile &connectProfile, bool callbackRunningTaskOnly) {
    xassert2(kEctOK != errType);
    xassert2(kTaskFailHandleTaskTimeout != failHandle);

    std::list<TaskProfile>::iterator first = mTaskList.begin();
    std::list<TaskProfile>::iterator last = mTaskList.end();

    while (first != last) {
        std::list<TaskProfile>::iterator next = first;
        ++next;

        if (callbackRunningTaskOnly && !first->runningId) {
            first = next;
            continue;
        }

        if (srcTaskId == Task::kInvalidTaskID || srcTaskId == first->task.taskId)
            __SingleRespHandle(first, errType, errCode, failHandle, connectProfile);
        else
            __SingleRespHandle(first, errType, 0, failHandle, connectProfile);

        first = next;
    }

    mLastBatchErrorTime = ::gettickcount();

    if (kEctLocal != errType && !mTaskList.empty()) {
        mRetryInterval = DEF_TASK_RETRY_INTERNAL;
    }

    if (kTaskFailHandleSessionTimeout == failHandle || kTaskFailHandleRetryAllTasks == failHandle) {
        mLongLink->Disconnect(LongLink::kDecodeErr);
        MessageQueue::CancelMessage(mAsyncReg.Get(), 0);
        mRetryInterval = 0;
    }

    if (kTaskFailHandleDefault == failHandle) {
        if (kEctDns != errType && kEctSocket != errType) {  // not longlink callback
            mLongLink->Disconnect(LongLink::kDecodeErr);
        }
        MessageQueue::CancelMessage(mAsyncReg.Get(), 0);
    }

    if (kEctNetMsgXP == errType) {
        mLongLink->Disconnect(LongLink::kTaskTimeout);
        MessageQueue::CancelMessage(mAsyncReg.Get(), 0);
    }
}

struct find_task {
public:
    bool operator()(const TaskProfile &value) { return taskid == value.task.taskId; }

public:
    uint32_t taskid;
};

std::list<TaskProfile>::iterator LongLinkTaskManager::__Locate(uint32_t taskId) {
    if (Task::kInvalidTaskID == taskId) return mTaskList.end();

    find_task find_functor;
    find_functor.taskid = taskId;
    std::list<TaskProfile>::iterator it = std::find_if(mTaskList.begin(), mTaskList.end(), find_functor);

    return it;
}

void LongLinkTaskManager::__OnResponse(ErrCmdType errorType, int errorCode, uint32_t cmdId, uint32_t taskId,
                                       AutoBuffer &bufBody, AutoBuffer &bufExt,
                                       const ConnectProfile &connectProfile) {
    move_wrapper<AutoBuffer> body(bufBody);
    move_wrapper<AutoBuffer> extension(bufExt);
    RETURN_LONKLINK_SYNC2ASYNC_FUNC(
            boost::bind(&LongLinkTaskManager::__OnResponse, this, errorType, errorCode, cmdId, taskId, body,
                        extension, connectProfile));
    // svr push notify

    if (kEctOK == errorType && ::longlink_ispush(cmdId, taskId, body, extension)) {
        xinfo2(TSF"task push seq:%_, cmdid:%_, len:(%_, %_)", taskId, cmdId, body->Length(),
               extension->Length());

        if (OnPushHook)
            OnPushHook(connectProfile.startTime, cmdId, taskId, body, extension);
        else xassert2(false);
        return;
    }

    if (kEctOK != errorType) {
        xwarn2(TSF"task error, taskId:%_, cmdid:%_, error_type:%_, error_code:%_", taskId, cmdId, errorType,
               errorCode);
        __BatchErrorRespHandle(errorType, errorCode, kTaskFailHandleDefault, 0, connectProfile);
        return;
    }

    std::list<TaskProfile>::iterator it = __Locate(taskId);

    if (mTaskList.end() == it) {
        xwarn2_if(Task::kInvalidTaskID != taskId, TSF"task no found task:%0, cmdid:%1, ect:%2, errcode:%3",
                  taskId, cmdId, errorType, errorCode);
        return;
    }

    it->transferProfile.receivedSize = body->Length();
    it->transferProfile.receiveDataSize = body->Length();
    it->transferProfile.lastReceivePkgTime = ::gettickcount();

    int err_code = 0;
    int handle_type = Buf2Resp(it->task.taskId, it->task.userContext, body, extension, err_code, Task::kChannelLong);

    switch (handle_type) {
        case kTaskFailHandleNoError: {
            mDynamicTimeout.CgiTaskStatistic(it->task.cgi, (unsigned int) it->transferProfile.sendDataSize +
                                                            (unsigned int) body->Length(),
                                              ::gettickcount() - it->transferProfile.startSendTime);
            __SingleRespHandle(it, kEctOK, err_code, handle_type, connectProfile);
            xassert2(NotifyNetworkErrorHook);
            NotifyNetworkErrorHook(__LINE__, kEctOK, err_code, connectProfile.ip, connectProfile.port);
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
            xwarn2(TSF"task decode error taskId:%_, cmdid:%_, handle_type:%_", it->task.taskId, it->task.cmdId,
                   handle_type);
            __SingleRespHandle(it, kEctEnDecode, err_code, handle_type, connectProfile);
        }
            break;
        case kTaskFailHandleDefault: {
            xerror2(TSF"task decode error taskId:%_, handle_type:%_, err_code:%_, body dump:%_", it->task.taskId,
                    handle_type, err_code, xdump(body->Ptr(), body->Length()));
            __BatchErrorRespHandle(kEctEnDecode, err_code, handle_type, it->task.taskId, connectProfile);
            xassert2(NotifyNetworkErrorHook);
            NotifyNetworkErrorHook(__LINE__, kEctEnDecode, err_code, connectProfile.ip, connectProfile.port);
        }
            break;
        default: {
            xassert2(false, TSF"task decode error fail_handle:%_, taskId:%_", handle_type, it->task.taskId);
            __BatchErrorRespHandle(kEctEnDecode, err_code, handle_type, it->task.taskId, connectProfile);
            xassert2(NotifyNetworkErrorHook);
            NotifyNetworkErrorHook(__LINE__, kEctEnDecode, handle_type, connectProfile.ip, connectProfile.port);
            break;
        }
    }

}

void LongLinkTaskManager::__OnSend(uint32_t taskId) {
    RETURN_LONKLINK_SYNC2ASYNC_FUNC(boost::bind(&LongLinkTaskManager::__OnSend, this, taskId));
    xverbose_function();

    std::list<TaskProfile>::iterator it = __Locate(taskId);

    if (mTaskList.end() != it) {
        if (it->transferProfile.firstStartSendTime == 0)
            it->transferProfile.firstStartSendTime = ::gettickcount();
        it->transferProfile.startSendTime = ::gettickcount();
        xdebug2(TSF"taskId:%_, starttime:%_", it->task.taskId, it->transferProfile.startSendTime / 1000);
    }
}

void LongLinkTaskManager::__OnRecv(uint32_t taskId, size_t _cachedsize, size_t _totalsize) {
    RETURN_LONKLINK_SYNC2ASYNC_FUNC(
            boost::bind(&LongLinkTaskManager::__OnRecv, this, taskId, _cachedsize, _totalsize));
    xverbose_function();
    std::list<TaskProfile>::iterator it = __Locate(taskId);

    if (mTaskList.end() != it) {
        if (it->transferProfile.lastReceivePkgTime == 0)
            WeakNetworkLogic::Singleton::Instance()->OnPkgEvent(true, (int) (::gettickcount() -
                                                                             it->transferProfile.startSendTime));
        else
            WeakNetworkLogic::Singleton::Instance()->OnPkgEvent(false, (int) (::gettickcount() -
                                                                              it->transferProfile.lastReceivePkgTime));
        it->transferProfile.receivedSize = _cachedsize;
        it->transferProfile.receiveDataSize = _totalsize;
        it->transferProfile.lastReceivePkgTime = ::gettickcount();
        xdebug2(TSF"taskId:%_, cachedsize:%_, _totalsize:%_", it->task.taskId, _cachedsize, _totalsize);
    } else {
        xwarn2(TSF"not found taskId:%_ cachedsize:%_, _totalsize:%_", taskId, _cachedsize, _totalsize);
    }
}

void LongLinkTaskManager::__SignalConnection(LongLink::TLongLinkStatus _connect_status) {
    if (LongLink::kConnected == _connect_status)
        __RunLoop();
}

