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
 * net_core.cc
 *
 *  Created on: 2012-7-18
 *      Author: yerungui
 */

#include "net_core.h"

#include <stdlib.h>

#include "boost/bind.hpp"
#include "boost/ref.hpp"


#include "mars/comm/messagequeue/message_queue.h"
#include "mars/comm/network/netinfo_util.h"
#include "mars/comm/socket/local_ipstack.h"
#include "mars/comm/xlogger/xlogger.h"
#include "mars/comm/singleton.h"
#include "mars/comm/platform_comm.h"

#include "mars/app/app.h"
#include "mars/baseevent/active_logic.h"
#include "mars/baseevent/baseprjevent.h"
#include "mars/stn/config.h"
#include "mars/stn/task_profile.h"
#include "mars/stn/proto/longlink_packer.h"

#include "net_source.h"
#include "net_check_logic.h"
#include "anti_avalanche.h"
#include "shortlink_task_manager.h"
#include "dynamic_timeout.h"

#ifdef USE_LONG_LINK

#include "longlink_task_manager.h"
#include "netsource_timercheck.h"
#include "timing_sync.h"

#endif

#include "signalling_keeper.h"
#include "zombie_task_manager.h"

using namespace mars::stn;
using namespace mars::app;

inline static bool __ValidAndInitDefault(Task &task, XLogger &_group) {
    if (2 * 60 * 1000 < task.serverProcessCost) {
        xerror2(TSF"server_process_cost invalid:%_ ", task.serverProcessCost) >> _group;
        return false;
    }

    if (30 < task.retryCount) {
        xerror2(TSF"retrycount invalid:%_ ", task.retryCount) >> _group;
        return false;
    }

    if (10 * 60 * 1000 < task.totalTimetout) {
        xerror2(TSF"total_timetout invalid:%_ ", task.totalTimetout) >> _group;
        return false;
    }

    if (task.channelSelect & Task::kChannelLong) {
        if (0 == task.cmdId) {
            xwarn2(" use longlink, but 0 == task.cmdid ") >> _group;
            task.channelSelect &= ~Task::kChannelLong;
        }
    }

    if (task.channelSelect & Task::kChannelShort) {
        xassert2(!task.cgi.empty());
        if (task.cgi.empty()) {
            xerror2("use shortlink, but cgi is empty ") >> _group;
            task.channelSelect &= ~Task::kChannelShort;
        }
    }

    if (0 > task.retryCount) {
        task.retryCount = DEF_TASK_RETRY_COUNT;
    }

    return true;
}

#define AYNC_HANDLER mAsyncReg.Get()

static const int kShortlinkErrTime = 3;


NetCore::NetCore()
        : mMsgQueueCreator(true, XLOGGER_TAG)
        , mAsyncReg(MessageQueue::InstallAsyncHandler(mMsgQueueCreator.CreateMessageQueue()))
        , mNetSource(new NetSource(*ActiveLogic::Singleton::Instance()))
        , mNetcheckLogic(new NetCheckLogic())
        , mAntiAvalanche(new AntiAvalanche(ActiveLogic::Singleton::Instance()->IsActive()))
        , mDynamicTimeout(new DynamicTimeout)
        , mShortLinkTaskManager(new ShortLinkTaskManager(*mNetSource, *mDynamicTimeout, mMsgQueueCreator.GetMessageQueue()))
        , mShortLinkErrorCount(0)
#ifdef USE_LONG_LINK
        , mZombieTaskManager(new ZombieTaskManager(mMsgQueueCreator.GetMessageQueue()))
        , mLongLinkTaskManager(new LongLinkTaskManager(*mNetSource, *ActiveLogic::Singleton::Instance(), *mDynamicTimeout, mMsgQueueCreator.GetMessageQueue()))
        , mSignalKeeper(new SignallingKeeper(mLongLinkTaskManager->LongLinkChannel(), mMsgQueueCreator.GetMessageQueue()))
        , mNetsourceTimercheck(new NetSourceTimerCheck(mNetSource, *ActiveLogic::Singleton::Instance(), mLongLinkTaskManager->LongLinkChannel(), mMsgQueueCreator.GetMessageQueue()))
        , mTimingSync(new TimingSync(*ActiveLogic::Singleton::Instance()))
#endif
        , mShortLinkTryFlag(false) {
    xwarn2(TSF"publiccomponent version: %0 %1", __DATE__, __TIME__);
    xassert2(mMsgQueueCreator.GetMessageQueue() != MessageQueue::KInvalidQueueID,
             "CreateNewMessageQueue Error!!!");
    xinfo2(TSF"netcore messagequeue_id=%_, handler:(%_,%_)", mMsgQueueCreator.GetMessageQueue(),
           mAsyncReg.Get().queue, mAsyncReg.Get().seq);

    std::string printinfo;

    SIMInfo info;
    getCurSIMInfo(info);
    printinfo = printinfo + "ISP_NAME : " + info.isp_name + "\n";
    printinfo = printinfo + "ISP_CODE : " + info.isp_code + "\n";

    AccountInfo account = ::GetAccountInfo();

    if (0 != account.uin) {
        char uinBuffer[64] = {0};
        snprintf(uinBuffer, sizeof(uinBuffer), "%u", (unsigned int) account.uin);
        printinfo = printinfo + "Uin :" + uinBuffer + "\n";
    }

    if (!account.username.empty()) {
        printinfo = printinfo + "UserName :" + account.username + "\n";
    }

    char version[256] = {0};
    snprintf(version, sizeof(version), "0x%X", mars::app::GetClientVersion());
    printinfo = printinfo + "ClientVersion :" + version + "\n";

    xwarn2(TSF"\n%0", printinfo.c_str());

    {
        //note: iOS getwifiinfo may block for 10+ seconds sometimes
        ASYNC_BLOCK_START

                            xinfo2(TSF"net info:%_", GetDetailNetInfo());

        ASYNC_BLOCK_END
    }

    xinfo_function();

    ActiveLogic::Singleton::Instance()->SignalActive.connect(boost::bind(&NetCore::__OnSignalActive, this, _1));


#ifdef USE_LONG_LINK
    mZombieTaskManager->StartTaskHook = boost::bind(&NetCore::StartTask, this, _1);
    mZombieTaskManager->CallResultHook = boost::bind(&NetCore::__CallBack, this, (int) kCallFromZombie, _1, _2, _3, _4,
                                                      _5);

    // async
    mLongLinkTaskManager->CallResultHook = boost::bind(&NetCore::__CallBack, this, (int) kCallFromLong, _1, _2, _3, _4,
                                                        _5);

    // sync
    mLongLinkTaskManager->NotifyRetryAllTasksHook = boost::bind(&NetCore::RetryTasks, this, _1, _2, _3, _4);
    mLongLinkTaskManager->NotifyNetworkErrorHook = boost::bind(&NetCore::__OnLongLinkNetworkError, this, _1, _2, _3,
                                                                  _4, _5);
    mLongLinkTaskManager->AntiAvalancheCheckHook = boost::bind(&AntiAvalanche::Check, mAntiAvalanche, _1, _2, _3);
    mLongLinkTaskManager->LongLinkChannel().fun_network_report_ = boost::bind(&NetCore::__OnLongLinkNetworkError,
                                                                                this, _1, _2, _3, _4, _5);

    mLongLinkTaskManager->LongLinkChannel().SignalConnection.connect(
            boost::bind(&TimingSync::OnLongLinkStatuChanged, mTimingSync, _1));
    mLongLinkTaskManager->LongLinkChannel().SignalConnection.connect(
            boost::bind(&NetCore::__OnLongLinkConnStatusChange, this, _1));

    mLongLinkTaskManager->OnPushHook = boost::bind(&NetCore::__OnPush, this, _1, _2, _3, _4, _5);
#ifdef __APPLE__
    mLongLinkTaskManager->getLongLinkConnectMonitor().LongLinkResetHook = boost::bind(&NetCore::__ResetLongLink, this);
#endif

    mNetsourceTimercheck->TimeCheckSuccHook = boost::bind(&NetCore::__OnTimerCheckSuc, this);

#endif

    // async
    mShortLinkTaskManager->CallResultHook = boost::bind(&NetCore::__CallBack, this, (int) kCallFromShort, _1, _2, _3,
                                                         _4, _5);

    // sync
    mShortLinkTaskManager->NotifyRetryAllTasksHook = boost::bind(&NetCore::RetryTasks, this, _1, _2, _3, _4);
    mShortLinkTaskManager->NotifyNetworkErrorHook = boost::bind(&NetCore::__OnShortLinkNetworkError, this, _1, _2,
                                                                   _3, _4, _5, _6);
    mShortLinkTaskManager->AntiAvalancheCheckHook = boost::bind(&AntiAvalanche::Check, mAntiAvalanche, _1, _2,
                                                                     _3);
    mShortLinkTaskManager->ShortLinkRspHook = boost::bind(&NetCore::__OnShortLinkResponse, this, _1);


#ifdef USE_LONG_LINK
    GetSignalOnNetworkDataChange().connect(
            boost::bind(&SignallingKeeper::OnNetWorkDataChanged, mSignalKeeper, _1, _2, _3));
    mSignalKeeper->SendBufferHook = boost::bind(&LongLink::SendWhenNoData,
                                                                  &mLongLinkTaskManager->LongLinkChannel(), _1, _2,
                                                                  _3, Task::kSignallingKeeperTaskID);
#endif

}

NetCore::~NetCore() {
    xinfo_function();

    ActiveLogic::Singleton::Instance()->SignalActive.disconnect(boost::bind(&NetCore::__OnSignalActive, this, _1));
    mAsyncReg.Cancel();


#ifdef USE_LONG_LINK
    GetSignalOnNetworkDataChange().disconnect(
            boost::bind(&SignallingKeeper::OnNetWorkDataChanged, mSignalKeeper, _1, _2, _3));

    mLongLinkTaskManager->LongLinkChannel().SignalConnection.disconnect_all_slots();
    mLongLinkTaskManager->LongLinkChannel().broadcast_linkstatus_signal_.disconnect_all_slots();

    OnPushHook.disconnect_all_slots();

    delete mNetsourceTimercheck;
    delete mSignalKeeper;
    delete mLongLinkTaskManager;
    delete mTimingSync;
    delete mZombieTaskManager;
#endif

    delete mShortLinkTaskManager;
    delete mDynamicTimeout;

    delete mAntiAvalanche;
    delete mNetcheckLogic;
    delete mNetSource;

    MessageQueue::MessageQueueCreater::ReleaseNewMessageQueue(MessageQueue::Handler2Queue(mAsyncReg.Get()));
}

void NetCore::__Release(NetCore *_instance) {
    if (MessageQueue::CurrentThreadMessageQueue() != MessageQueue::Handler2Queue(_instance->mAsyncReg.Get())) {
        WaitMessage(AsyncInvoke((MessageQueue::AsyncInvokeFunction) boost::bind(&NetCore::__Release, _instance),
                                _instance->mAsyncReg.Get(), "NetCore::__Release"));
        return;
    }

    delete _instance;
}


void NetCore::StartTask(const Task &task0) {

    ASYNC_BLOCK_START

                        xgroup2_define(group);
                xinfo2(TSF"task start long short taskId:%0, cmdid:%1, need_authed:%2, cgi:%3, channelSelect:%4, limit_flow:%5, ",
                       task0.taskId, task0.cmdId, task0.needAuthed, task0.cgi.c_str(), task0.channelSelect,
                       task0.limitFlow) >> group;
                xinfo2(TSF"host:%_, send_only:%_, cmdid:%_, server_process_cost:%_, retrycount:%_,  channel_strategy:%_, ",
                       task0.shortLinkHostList.empty() ? "" : task0.shortLinkHostList.front(), task0.sendOnly,
                       task0.cmdId, task0.serverProcessCost, task0.retryCount, task0.channelStrategy) >> group;
                xinfo2(TSF" total_timetout:%_, network_status_sensitive:%_, priority:%_, report_arg:%_",
                       task0.totalTimetout, task0.networkStatusSensitive, task0.priority, task0.reportArg) >> group;

                Task task = task0;
                if (!__ValidAndInitDefault(task, group)) {
                    OnTaskEnd(task.taskId, task.userContext, kEctLocal, kEctLocalTaskParam);
                    return;
                }

                if (TaskProcessHook) {
                    TaskProcessHook(task);
                }

                if (0 == task.channelSelect) {
                    xerror2(TSF"error channelType (%_, %_), ", kEctLocal, kEctLocalChannelSelect) >> group;

                    OnTaskEnd(task.taskId, task.userContext, kEctLocal, kEctLocalChannelSelect);
                    return;
                }

                if (task.networkStatusSensitive && kNoNet == ::getNetInfo()
                    #ifdef USE_LONG_LINK
                    && LongLink::kConnected != mLongLinkTaskManager->LongLinkChannel().ConnectStatus()
#endif
                        ) {
                    xerror2(TSF"error no net (%_, %_), ", kEctLocal, kEctLocalNoNet) >> group;
                    OnTaskEnd(task.taskId, task.userContext, kEctLocal, kEctLocalNoNet);
                    return;
                }

                bool start_ok = false;

#ifdef USE_LONG_LINK

                if (LongLink::kConnected != mLongLinkTaskManager->LongLinkChannel().ConnectStatus()
                    && (Task::kChannelLong & task.channelSelect) && ActiveLogic::Singleton::Instance()->IsForeground()

                    &&
                    (15 * 60 * 1000 >= gettickcount() - ActiveLogic::Singleton::Instance()->LastForegroundChangeTime()))
                    mLongLinkTaskManager->getLongLinkConnectMonitor().MakeSureConnected();

#endif

                xgroup2() << group;

                switch (task.channelSelect) {
                    case Task::kChannelBoth: {

#ifdef USE_LONG_LINK
                        bool bUseLongLink =
                                LongLink::kConnected == mLongLinkTaskManager->LongLinkChannel().ConnectStatus();

                        if (bUseLongLink && task.channelStrategy == Task::kChannelFastStrategy) {
                            xinfo2(TSF"long link task count:%0, ", mLongLinkTaskManager->GetTaskCount());
                            bUseLongLink = bUseLongLink &&
                                           (mLongLinkTaskManager->GetTaskCount() <= kFastSendUseLonglinkTaskCntLimit);
                        }

                        if (bUseLongLink)
                            start_ok = mLongLinkTaskManager->StartTask(task);
                        else
#endif
                            start_ok = mShortLinkTaskManager->StartTask(task);
                    }
                        break;
#ifdef USE_LONG_LINK

                    case Task::kChannelLong:
                        start_ok = mLongLinkTaskManager->StartTask(task);
                        break;
#endif

                    case Task::kChannelShort:
                        start_ok = mShortLinkTaskManager->StartTask(task);
                        break;

                    default:
                        xassert2(false);
                        break;
                }

                if (!start_ok) {
                    xerror2(TSF"taskId:%_, error starttask (%_, %_)", task.taskId, kEctLocal,
                            kEctLocalStartTaskFail);
                    OnTaskEnd(task.taskId, task.userContext, kEctLocal, kEctLocalStartTaskFail);
                } else {
#ifdef USE_LONG_LINK
                    mZombieTaskManager->OnNetCoreStartTask();
#endif
                }

    ASYNC_BLOCK_END
}

void NetCore::StopTask(uint32_t taskId) {
    ASYNC_BLOCK_START

#ifdef USE_LONG_LINK
                if (mLongLinkTaskManager->StopTask(taskId)) return;
                if (mZombieTaskManager->StopTask(taskId)) return;
#endif

                if (mShortLinkTaskManager->StopTask(taskId)) return;

                xerror2(TSF"task no found taskId:%0", taskId);

    ASYNC_BLOCK_END
}

bool NetCore::HasTask(uint32_t taskId) const {
    WAIT_SYNC2ASYNC_FUNC(boost::bind(&NetCore::HasTask, this, taskId));

#ifdef USE_LONG_LINK
    if (mLongLinkTaskManager->HasTask(taskId)) return true;
    if (mZombieTaskManager->HasTask(taskId)) return true;
#endif
    if (mShortLinkTaskManager->HasTask(taskId)) return true;

    return false;
}

void NetCore::ClearTasks() {
    ASYNC_BLOCK_START

#ifdef USE_LONG_LINK
                mLongLinkTaskManager->ClearTasks();
                mZombieTaskManager->ClearTasks();
#endif
                mShortLinkTaskManager->ClearTasks();

    ASYNC_BLOCK_END
}

void NetCore::OnNetworkChange() {

    SYNC2ASYNC_FUNC(boost::bind(&NetCore::OnNetworkChange, this));  //if already messagequeue, no need to async

    xinfo_function();

    std::string ip_stack_log;
    TLocalIPStack ip_stack = local_ipstack_detect_log(ip_stack_log);

    switch (::getNetInfo()) {
        case kNoNet:
            xinfo2(TSF"task network change current network:no network");
            break;

        case kWifi: {
            WifiInfo info;
            getCurWifiInfo(info);
            xinfo2(TSF"task network change current network:wifi, ssid:%_, ip_stack:%_, log:%_", info.ssid,
                   TLocalIPStackStr[ip_stack], ip_stack_log);
        }
            break;

        case kMobile: {
            SIMInfo info;
            getCurSIMInfo(info);
            RadioAccessNetworkInfo raninfo;
            getCurRadioAccessNetworkInfo(raninfo);
            xinfo2(TSF"task network change current network:mobile, ispname:%_, ispcode:%_, ran:%_, ip_stack:%_, log:%_",
                   info.isp_name, info.isp_code, raninfo.radio_access_network, TLocalIPStackStr[ip_stack],
                   ip_stack_log);
        }
            break;

        case kOtherNet:
            xinfo2(TSF"task network change current network:other, ip_stack:%_, log:%_", TLocalIPStackStr[ip_stack],
                   ip_stack_log);
            break;

        default:
            xassert2(false);
            break;
    }

#ifdef USE_LONG_LINK
    mNetsourceTimercheck->CancelConnect();
#endif

    mNetSource->ClearCache();

    mDynamicTimeout->ResetStatus();
#ifdef USE_LONG_LINK
    mTimingSync->OnNetworkChange();
    if (mLongLinkTaskManager->getLongLinkConnectMonitor().NetworkChange())
        mLongLinkTaskManager->RedoTasks();
    mZombieTaskManager->RedoTasks();
#endif

    mShortLinkTaskManager->RedoTasks();

    mShortLinkTryFlag = false;
    mShortLinkErrorCount = 0;

}

void NetCore::KeepSignal() {
    ASYNC_BLOCK_START
                        mSignalKeeper->Keep();
    ASYNC_BLOCK_END
}

void NetCore::StopSignal() {
    ASYNC_BLOCK_START
                        mSignalKeeper->Stop();
    ASYNC_BLOCK_END
}

#ifdef USE_LONG_LINK

LongLink &NetCore::Longlink() { return mLongLinkTaskManager->LongLinkChannel(); }

#ifdef __APPLE__
void NetCore::__ResetLongLink() {
    SYNC2ASYNC_FUNC(boost::bind(&NetCore::__ResetLongLink, this));

    mLongLinkTaskManager->LongLinkChannel().Disconnect(LongLink::kNetworkChange);
    mLongLinkTaskManager->RedoTasks();
    
}
#endif
#endif  // apple

void NetCore::RedoTasks() {
    ASYNC_BLOCK_START

                        xinfo_function();

#ifdef USE_LONG_LINK
                mNetsourceTimercheck->CancelConnect();
#endif

                mNetSource->ClearCache();

#ifdef USE_LONG_LINK
                mLongLinkTaskManager->LongLinkChannel().Disconnect(LongLink::kReset);
                mLongLinkTaskManager->LongLinkChannel().MakeSureConnected();
                mLongLinkTaskManager->RedoTasks();
                mZombieTaskManager->RedoTasks();
#endif
                mShortLinkTaskManager->RedoTasks();

    ASYNC_BLOCK_END
}

void NetCore::RetryTasks(ErrCmdType errType, int errCode, int failHandle, uint32_t srcTaskId) {

    mShortLinkTaskManager->RetryTasks(errType, errCode, failHandle, srcTaskId);
#ifdef USE_LONG_LINK
    mLongLinkTaskManager->RetryTasks(errType, errCode, failHandle, srcTaskId);
#endif
}

void NetCore::MakeSureLongLinkConnect() {
#ifdef USE_LONG_LINK
    ASYNC_BLOCK_START
                        mLongLinkTaskManager->LongLinkChannel().MakeSureConnected();
    ASYNC_BLOCK_END
#endif
}

bool NetCore::LongLinkIsConnected() {
#ifdef USE_LONG_LINK
    return LongLink::kConnected == mLongLinkTaskManager->LongLinkChannel().ConnectStatus();
#endif
    return false;
}

int NetCore::__CallBack(int _from, ErrCmdType errType, int errCode, int failHandle, const Task &task,
                        unsigned int taskCostTime) {

    if (CallResultHook && 0 == CallResultHook(_from, errType, errCode, failHandle, task)) {
        xwarn2(TSF"task_callback_hook let task return. taskId:%_, cgi%_.", task.taskId, task.cgi);
        return 0;
    }

    if (kEctOK == errType || kTaskFailHandleTaskEnd == failHandle)
        return OnTaskEnd(task.taskId, task.userContext, errType, errCode);

    if (kCallFromZombie == _from) return OnTaskEnd(task.taskId, task.userContext, errType, errCode);

#ifdef USE_LONG_LINK
    if (!mZombieTaskManager->SaveTask(task, taskCostTime))
#endif
        return OnTaskEnd(task.taskId, task.userContext, errType, errCode);

    return 0;
}

void NetCore::__OnShortLinkResponse(int _status_code) {
    if (_status_code == 301 || _status_code == 302 || _status_code == 307) {

#ifdef USE_LONG_LINK
        LongLink::TLongLinkStatus longLinkStatus = mLongLinkTaskManager->LongLinkChannel().ConnectStatus();
        unsigned int continues_fail_count = mLongLinkTaskManager->GetTasksContinuousFailCount();
        xinfo2(TSF"status code:%0, long link status:%1, longlink task continue fail count:%2", _status_code,
               longLinkStatus, continues_fail_count);

        if (LongLink::kConnected == longLinkStatus && continues_fail_count == 0) {
            return;
        }
#endif
        // TODO callback
    }
}

#ifdef USE_LONG_LINK

void NetCore::__OnPush(uint64_t channelId, uint32_t cmdId, uint32_t taskId, const AutoBuffer &body,
                       const AutoBuffer &bufExt) {
    xinfo2(TSF"task push seq:%_, cmdid:%_, len:%_", taskId, cmdId, body.Length());
    OnPushHook(cmdId, body);
    OnPush(channelId, cmdId, taskId, body, bufExt);
}

void NetCore::__OnLongLinkNetworkError(int _line, ErrCmdType errType, int errCode, const std::string &ip,
                                       uint16_t port) {
    SYNC2ASYNC_FUNC(boost::bind(&NetCore::__OnLongLinkNetworkError, this, _line, errType, errCode, ip, port));
    xassert2(MessageQueue::CurrentThreadMessageQueue() == mMsgQueueCreator.GetMessageQueue());

    mNetcheckLogic->UpdateLongLinkInfo(mLongLinkTaskManager->GetTasksContinuousFailCount(), errType == kEctOK);
    OnLongLinkNetworkError(errType, errCode, ip, port);

    if (kEctOK == errType) mZombieTaskManager->RedoTasks();

    if (kEctDial == errType) return;

    if (kEctHttp == errType) return;

    if (kEctServer == errType) return;

    if (kEctLocal == errType) return;

    mNetSource->ReportLongIP(errType == kEctOK, ip, port);

}

#endif

void NetCore::__OnShortLinkNetworkError(int _line, ErrCmdType errType, int errCode, const std::string &ip,
                                        const std::string &host, uint16_t port) {
    SYNC2ASYNC_FUNC(
            boost::bind(&NetCore::__OnShortLinkNetworkError, this, _line, errType, errCode, ip, host, port));
    xassert2(MessageQueue::CurrentThreadMessageQueue() == mMsgQueueCreator.GetMessageQueue());

    mNetcheckLogic->UpdateShortLinkInfo(mShortLinkTaskManager->GetTasksContinuousFailCount(), errType == kEctOK);
    OnShortLinkNetworkError(errType, errCode, ip, host, port);

    mShortLinkTryFlag = true;

    if (errType == kEctOK) {
        mShortLinkErrorCount = 0;
    } else {
        ++mShortLinkErrorCount;
    }

    __ConnStatusCallBack();

#ifdef USE_LONG_LINK
    if (kEctOK == errType) mZombieTaskManager->RedoTasks();
#endif

    if (kEctDial == errType) return;

    if (kEctNetMsgXP == errType) return;

    if (kEctServer == errType) return;

    if (kEctLocal == errType) return;

    mNetSource->ReportShortIP(errType == kEctOK, ip, host, port);

}


#ifdef USE_LONG_LINK

void NetCore::__OnLongLinkConnStatusChange(LongLink::TLongLinkStatus status) {
    if (LongLink::kConnected == status) mZombieTaskManager->RedoTasks();

    __ConnStatusCallBack();
    OnLongLinkStatusChange(status);
}

#endif

void NetCore::__ConnStatusCallBack() {

    int all_connstatus = kNetworkUnavailable;

    if (mShortLinkTryFlag) {
        if (mShortLinkErrorCount >= kShortlinkErrTime) {
            all_connstatus = kServerFailed;
        } else if (0 == mShortLinkErrorCount) {
            all_connstatus = kConnected;
        } else {
            all_connstatus = kNetworkUnkown;
        }
    } else {
        all_connstatus = kNetworkUnkown;
    }

    int longlink_connstatus = kNetworkUnkown;
#ifdef USE_LONG_LINK
    longlink_connstatus = mLongLinkTaskManager->LongLinkChannel().ConnectStatus();
    switch (longlink_connstatus) {
        case LongLink::kDisConnected:
            return;
        case LongLink::kConnectFailed:
            if (mShortLinkTryFlag) {
                if (0 == mShortLinkErrorCount) {
                    all_connstatus = kConnected;
                } else if (mShortLinkErrorCount >= kShortlinkErrTime) {
                    all_connstatus = kServerFailed;
                } else {
                    all_connstatus = kNetworkUnkown;
                }
            } else {
                all_connstatus = kNetworkUnkown;
            }
            longlink_connstatus = kServerFailed;
            break;

        case LongLink::kConnectIdle:
        case LongLink::kConnecting:
            if (mShortLinkTryFlag) {
                if (0 == mShortLinkErrorCount) {
                    all_connstatus = kConnected;
                } else if (mShortLinkErrorCount >= kShortlinkErrTime) {
                    all_connstatus = kServerFailed;
                } else {
                    all_connstatus = kConnecting;
                }
            } else {
                all_connstatus = kConnecting;
            }

            longlink_connstatus = kConnecting;
            break;

        case LongLink::kConnected:
            all_connstatus = kConnected;
            mShortLinkErrorCount = 0;
            mShortLinkTryFlag = false;
            longlink_connstatus = kConnected;
            break;

        default:
            xassert2(false);
            break;
    }
#else
    if (mShortLinkErrorCount >= kShortlinkErrTime) {
        all_connstatus = kServerFailed;
    } else if (0 == mShortLinkErrorCount) {
        all_connstatus = kConnected;
    } else {
        all_connstatus = kConnected;
    }
#endif

    xinfo2(TSF"reportNetConnectInfo all_connstatus:%_, longlink_connstatus:%_", all_connstatus,
           longlink_connstatus);
    ReportConnectStatus(all_connstatus, longlink_connstatus);
}

void NetCore::__OnTimerCheckSuc() {
    SYNC2ASYNC_FUNC(boost::bind(&NetCore::__OnTimerCheckSuc, this));

#ifdef USE_LONG_LINK
    xinfo2("netsource timercheck disconnect longlink");
    mLongLinkTaskManager->LongLinkChannel().Disconnect(LongLink::kTimeCheckSucc);

#endif

}

void NetCore::__OnSignalActive(bool isActive) {
    ASYNC_BLOCK_START

                        mAntiAvalanche->OnSignalActive(isActive);

    ASYNC_BLOCK_END
}

void NetCore::AddServerBan(const std::string &ip) {
    mNetSource->AddServerBan(ip);
}

ConnectProfile NetCore::GetConnectProfile(uint32_t taskId, int channelSelect) {
    if (channelSelect == Task::kChannelShort) {
        return mShortLinkTaskManager->GetConnectProfile(taskId);
    }
#ifdef USE_LONG_LINK
    else if (channelSelect == Task::kChannelLong) {
        return mLongLinkTaskManager->LongLinkChannel().Profile();
    }
#endif
    return ConnectProfile();
}