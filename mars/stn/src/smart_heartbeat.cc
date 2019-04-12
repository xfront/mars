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
 * smart_heartbeat.cc
 *
 * description: Manage the heartbeat frequecy by Adaptive Computing for the current active network.
 *              The purpose is to decrease the heartbeat frequecy when our app in inactive state,
 *              And meanwhile keep the TCP alive as far as possible.
 * Created on: 2014-1-22
 * Author: phoenixzuo
 *
 */

#include "smart_heartbeat.h"

#include <time.h>
#include <unistd.h>

#include "boost/filesystem.hpp"

#include "mars/comm/time_utils.h"
#include "mars/comm/xlogger/xlogger.h"
#include "mars/comm/singleton.h"
#include "mars/comm/platform_comm.h"

#include "mars/baseevent/active_logic.h"
#include "mars/app/app.h"

#include "mars/stn/config.h"
#include <algorithm>

#define KV_KEY_SMARTHEART 11249

static const std::string kFileName = "Heartbeat.ini";

// INI key
static const char *const kKeyModifyTime = "modifyTime";
static const char *const kKeyCurHeart = "curHeart";
static const char *const kKeyFailHeartCount = "failHeartCount";
static const char *const kKeyStable = "stable";
static const char *const kKeyNetType = "netType";
static const char *const kKeyHeartType = "hearttype";
static const char *const kKeyMinHeartFail = "minheartfail";

SmartHeartbeat::SmartHeartbeat()
    : ReportSmartHeartHook(NULL)
    , mIsWaitHeartRsp(false)
    , mHeartSuccCount(0)
    , mLastHeart(MinHeartInterval)
    , mIni(mars::app::GetAppFilePath() + "/" + kFileName, false), mDozeModeCount(0)
    , mNormalModeCount(0), mNoopStartTick(false)
{
    xinfo_function();
    mIni.Parse();
}

SmartHeartbeat::~SmartHeartbeat() {
    xinfo_function();
    __SaveINI();
}

void SmartHeartbeat::OnHeartbeatStart() {
    xverbose_function();

    mNoopStartTick.gettickcount();
    mIsWaitHeartRsp = true;
}

void SmartHeartbeat::OnLongLinkEstablished() {
    xdebug_function();
    __LoadINI();
    mHeartSuccCount = 0;
}

void SmartHeartbeat::OnLongLinkDisconnect() {
    xinfo_function();

    OnHeartResult(false, false);

    if (!mCurrentHeartInfo.mIsStable) {
        xinfo2(TSF"%0 not stable last heart:%1", mCurrentHeartInfo.mNetDetail,
               mCurrentHeartInfo.mCurHeart);
        return;
    }

    mCurrentHeartInfo.mHeartSuccCount = 0;
    mLastHeart = MinHeartInterval;
}

#define ONE_DAY_SECONEDS (24 * 60 * 60)

void SmartHeartbeat::OnHeartResult(bool success, bool failTimeout) {
    if (!mIsWaitHeartRsp)
        return;

    if (ReportSmartHeartHook && !success && mHeartSuccCount >= NetStableTestCount &&
        mCurrentHeartInfo.mIsStable) {
        ReportSmartHeartHook(kActionDisconnect, mCurrentHeartInfo, failTimeout);
    }

    xdebug2(TSF"heart result:%0, timeout:%1", success, failTimeout);
    mIsWaitHeartRsp = false;

    xassert2(!mCurrentHeartInfo.mNetDetail.empty(), "something wrong,mNetDetail shoudn't be NULL");
    if (mCurrentHeartInfo.mNetDetail.empty()) return;
    if (success) mHeartSuccCount += 1;
    if (mHeartSuccCount < NetStableTestCount) {
        mCurrentHeartInfo.mHeartFailMinCount = success ? 0 : (mCurrentHeartInfo.mHeartFailMinCount +
                                                                       1);
        if (ReportSmartHeartHook && mCurrentHeartInfo.mHeartFailMinCount >= 6 && ::isNetworkConnected()) {
            ReportSmartHeartHook(kActionBadNetwork, mCurrentHeartInfo, false);
            mCurrentHeartInfo.mHeartFailMinCount = 0;
        }
        return;
    }

    if (mLastHeart != mCurrentHeartInfo.mCurHeart) {
        xdebug2(TSF"last heart & cur_heart not match, ignore");
        return;
    }

    if (success) {
        mCurrentHeartInfo.mHeartSuccCount += 1;
        mCurrentHeartInfo.mHeartFailCount = 0;
    } else {
        mCurrentHeartInfo.mHeartFailCount += 1;
    }

    if (success && mCurrentHeartInfo.mIsStable) {
        // has reach the Max value, no need to try bigger.
        if (mCurrentHeartInfo.mCurHeart >= MaxHeartInterval - SuccessStep) return;

        time_t cur_time = time(NULL);
        // heart info changed recently,Don't need probe
        // probe bigger heart on Wednesday
        if ((cur_time - mCurrentHeartInfo.mLastModifyTime) >= 7 * ONE_DAY_SECONEDS &&
            mCurrentHeartInfo.mCurHeart < (MaxHeartInterval - SuccessStep)) {
            xinfo2(TSF"tryProbeBiggerHeart. curHeart=%_, last modify:%_", mCurrentHeartInfo.mCurHeart,
                   mCurrentHeartInfo.mLastModifyTime);
            mCurrentHeartInfo.mCurHeart += SuccessStep;
            mCurrentHeartInfo.mHeartSuccCount = 0;
            mCurrentHeartInfo.mIsStable = false;
            mCurrentHeartInfo.mHeartFailCount = 0;
            if (ReportSmartHeartHook)
                ReportSmartHeartHook(kActionReCalc, mCurrentHeartInfo, false);
            __SaveINI();
        }
        return;
    }

    if (success) {
        if (mCurrentHeartInfo.mHeartSuccCount >= BaseSuccCount) {
            if (mCurrentHeartInfo.mCurHeart >= (MaxHeartInterval - SuccessStep)) {
                //already max, make stable
                mCurrentHeartInfo.mCurHeart = MaxHeartInterval - SuccessStep;
                mCurrentHeartInfo.mHeartSuccCount = 0;
                mCurrentHeartInfo.mIsStable = true;
                mCurrentHeartInfo.mHeartType = __IsDozeStyle() ? kDozeModeHeart : kSmartHeartBeat;
                xinfo2(TSF"%0 find the smart heart interval = %1", mCurrentHeartInfo.mNetDetail,
                       mCurrentHeartInfo.mCurHeart);
                if (ReportSmartHeartHook)
                    ReportSmartHeartHook(kActionCalcEnd, mCurrentHeartInfo, false);
            } else {
                mCurrentHeartInfo.mHeartSuccCount = 0;

                unsigned int old_heart = mCurrentHeartInfo.mCurHeart;
                if (__IsDozeStyle()) {
                    mCurrentHeartInfo.mCurHeart = MaxHeartInterval - SuccessStep;
                } else {
                    mCurrentHeartInfo.mCurHeart += HeartStep;
                    mCurrentHeartInfo.mCurHeart = std::min((unsigned int) (MaxHeartInterval - SuccessStep),
                                                                  mCurrentHeartInfo.mCurHeart);
                }

                xinfo2(TSF"increace curHeart from %_ to %_", old_heart, mCurrentHeartInfo.mCurHeart);
            }
        }
    } else {
        if (mLastHeart == MinHeartInterval) return;

        if (mCurrentHeartInfo.mHeartFailCount >= MaxHeartFailCount) {
            if (mCurrentHeartInfo.mIsStable) {
                mCurrentHeartInfo.mCurHeart = MinHeartInterval;
                mCurrentHeartInfo.mHeartSuccCount = 0;
                mCurrentHeartInfo.mIsStable = false;
                if (ReportSmartHeartHook)
                    ReportSmartHeartHook(kActionReCalc, mCurrentHeartInfo, true);
                //first report, then set fail count
                mCurrentHeartInfo.mHeartFailCount = 0;
                xinfo2(TSF"in stable sate,can't use old value to Keep TCP alive");
            } else {
                if (__IsDozeStyle()) {
                    mCurrentHeartInfo.mCurHeart = MinHeartInterval;
                } else if ((mCurrentHeartInfo.mCurHeart - HeartStep - SuccessStep) > MinHeartInterval) {
                    mCurrentHeartInfo.mCurHeart = mCurrentHeartInfo.mCurHeart - HeartStep - SuccessStep;
                } else {
                    mCurrentHeartInfo.mCurHeart = MinHeartInterval;
                }

                mCurrentHeartInfo.mHeartSuccCount = 0;
                mCurrentHeartInfo.mHeartFailCount = 0;
                mCurrentHeartInfo.mIsStable = true;
                mCurrentHeartInfo.mHeartType = __IsDozeStyle() ? kDozeModeHeart : kSmartHeartBeat;
                xinfo2(TSF"finish choose the proper value %0", mCurrentHeartInfo.mCurHeart);
                if (ReportSmartHeartHook)
                    ReportSmartHeartHook(kActionCalcEnd, mCurrentHeartInfo, false);
            }
        }
    }

    __DumpHeartInfo();
    __SaveINI();
}


#define MAX_JUDGE_TIMES (10)

void SmartHeartbeat::JudgeDozeStyle() {

    if (ActiveLogic::Singleton::Instance()->IsActive()) return;
    if (!mNoopStartTick.isValid()) return;
    if (kMobile != ::getNetInfo()) return;

    if (std::abs(mNoopStartTick.gettickspan() - mLastHeart) >= 20 * 1000) {
        mDozeModeCount++;
        mNormalModeCount = std::max(mNormalModeCount - 1, 0);
    } else {
        mNormalModeCount++;
        mDozeModeCount = std::max(mDozeModeCount - 1, 0);
    }
    mNoopStartTick = tickcount_t(false);
}


bool SmartHeartbeat::__IsDozeStyle() {
    return ((mDozeModeCount > (2 * mNormalModeCount)) && kMobile == ::getNetInfo());
}

unsigned int SmartHeartbeat::GetNextHeartbeatInterval() {  //
    // xinfo_function();

    if (ActiveLogic::Singleton::Instance()->IsActive()) {
        mLastHeart = MinHeartInterval;
        return MinHeartInterval;
    }

    if (mHeartSuccCount < NetStableTestCount || mCurrentHeartInfo.mNetDetail.empty()) {
        //        xdebug2(TSF"getNextHeartbeatInterval use MinHeartInterval. mHeartSuccCount=%0",mHeartSuccCount);
        mLastHeart = MinHeartInterval;
        return MinHeartInterval;
    }

    mLastHeart = mCurrentHeartInfo.mCurHeart;
    xassert2((mLastHeart < MaxHeartInterval && mLastHeart >= MinHeartInterval), "heart value invalid");

    if (__IsDozeStyle() && mCurrentHeartInfo.mHeartType != kDozeModeHeart &&
        mLastHeart != (MaxHeartInterval - SuccessStep)) {
        mCurrentHeartInfo.mCurHeart = mLastHeart = MinHeartInterval;
    }
    if (mLastHeart >= MaxHeartInterval || mLastHeart < MinHeartInterval) {
        mCurrentHeartInfo.mCurHeart = mLastHeart = MinHeartInterval;
    }
    return mLastHeart;
}

void SmartHeartbeat::__LoadINI() {
    xinfo_function();
    std::string net_info;
    int net_type = getCurrNetLabel(net_info);

    if (net_info.empty()) {
        mCurrentHeartInfo.Clear();
        xerror2("net_info NULL");
        return;
    }
    if (net_info == mCurrentHeartInfo.mNetDetail) return;

    mCurrentHeartInfo.Clear();
    mCurrentHeartInfo.mNetDetail = net_info;
    mCurrentHeartInfo.mNetType = net_type;

    if (mIni.Select(net_info)) {
        mCurrentHeartInfo.mLastModifyTime = mIni.Get(kKeyModifyTime, mCurrentHeartInfo.mLastModifyTime);
        mCurrentHeartInfo.mCurHeart = mIni.Get(kKeyCurHeart, mCurrentHeartInfo.mCurHeart);
        mCurrentHeartInfo.mHeartFailCount = mIni.Get(kKeyFailHeartCount,
                                                             mCurrentHeartInfo.mHeartFailCount);
        mCurrentHeartInfo.mIsStable = mIni.Get(kKeyStable, mCurrentHeartInfo.mIsStable);
        mCurrentHeartInfo.mNetType = mIni.Get(kKeyNetType, mCurrentHeartInfo.mNetType);
        mCurrentHeartInfo.mHeartType = (TSmartHeartBeatType) mIni.Get(kKeyHeartType, 0);
        mCurrentHeartInfo.mHeartFailMinCount = mIni.Get(kKeyMinHeartFail, 0);

        xassert2(net_type == mCurrentHeartInfo.mNetType, "cur:%d, INI:%d", net_type,
                 mCurrentHeartInfo.mNetType);

        if (mCurrentHeartInfo.mCurHeart < MinHeartInterval) {
            xerror2(TSF"mCurrentHeartInfo.mCurHeart:%_ < MinHeartInterval:%_",
                    mCurrentHeartInfo.mCurHeart, MinHeartInterval);
            mCurrentHeartInfo.mCurHeart = MinHeartInterval;
        }

        if (mCurrentHeartInfo.mCurHeart > MaxHeartInterval) {
            xerror2(TSF"mCurrentHeartInfo.mCurHeart:%_ > MaxHeartInterval:%_",
                    mCurrentHeartInfo.mCurHeart, MaxHeartInterval);
            mCurrentHeartInfo.mCurHeart = MaxHeartInterval - SuccessStep;
        }

        time_t cur_time = time(NULL);

        if (mCurrentHeartInfo.mLastModifyTime > cur_time) {
            xerror2(TSF"mCurrentHeartInfo.mLastModifyTime:%_ > cur_time:%_",
                    mCurrentHeartInfo.mLastModifyTime, cur_time);
            mCurrentHeartInfo.mLastModifyTime = cur_time;
        }
    } else {
        __LimitINISize();
        bool ret = mIni.Create(net_info);
        xassert2(ret);
        __SaveINI();
    }
    __DumpHeartInfo();
}

#define MAX_INI_SECTIONS (20)

void SmartHeartbeat::__LimitINISize() {
    xinfo_function();
    SpecialINI::sections_t &sections = mIni.Sections();

    if (mIni.Sections().size() <= MAX_INI_SECTIONS)
        return;

    xwarn2(TSF"sections.size=%0 > MAX_INI_SECTIONS=%1", sections.size(), MAX_INI_SECTIONS);

    time_t cur_time = time(NULL);

    time_t min_time = 0;
    SpecialINI::sections_t::iterator min_iter = sections.end();

    for (SpecialINI::sections_t::iterator iter = sections.begin(); iter != sections.end();) {
        SpecialINI::keys_t::iterator time_iter = iter->second.find(kKeyModifyTime);

        if (time_iter == iter->second.end()) {
            // remove dirty value
            sections.erase(iter++);
            xinfo2(TSF"remove dirty value because miss KEY_ModifyTime");
            continue;
        }

        time_t time_value = number_cast<time_t>(time_iter->second.c_str());

        if (time_value > cur_time) {
            // remove dirty value
            sections.erase(iter++);
            xinfo2(TSF"remove dirty value because Wrong ModifyTime ");
            continue;
        }

        if (0 == min_time || time_value < min_time) {
            min_iter = iter;
            min_time = time_value;
        }

        ++iter;
    }

    if (min_iter != sections.end()) sections.erase(min_iter);
}

void SmartHeartbeat::__SaveINI() {
    xdebug_function();
    if (mCurrentHeartInfo.mNetDetail.empty())return;

    mCurrentHeartInfo.mLastModifyTime = time(NULL);

    mIni.Set<time_t>(kKeyModifyTime, mCurrentHeartInfo.mLastModifyTime);
    mIni.Set(kKeyCurHeart, mCurrentHeartInfo.mCurHeart);
    mIni.Set(kKeyFailHeartCount, mCurrentHeartInfo.mHeartFailCount);
    mIni.Set(kKeyStable, mCurrentHeartInfo.mIsStable);
    mIni.Set(kKeyNetType, mCurrentHeartInfo.mNetType);
    mIni.Set(kKeyHeartType, mCurrentHeartInfo.mHeartType);
    mIni.Set(kKeyMinHeartFail, mCurrentHeartInfo.mHeartFailMinCount);
    mIni.Save();
}

void SmartHeartbeat::__DumpHeartInfo() {
    xinfo2(TSF"SmartHeartbeat Info mLastHeart:%0,successHeartCount:%1, currSuccCount:%2", mLastHeart,
           mHeartSuccCount, mCurrentHeartInfo.mHeartSuccCount);

    if (!mCurrentHeartInfo.mNetDetail.empty()) {
        xinfo2(TSF"currentNetHeartInfo detail:%0,curHeart:%1,isStable:%2,failcount:%3,modifyTime:%4,type:%5,min_fail:%6",
               mCurrentHeartInfo.mNetDetail, mCurrentHeartInfo.mCurHeart,
               mCurrentHeartInfo.mIsStable,
               mCurrentHeartInfo.mHeartFailCount, mCurrentHeartInfo.mLastModifyTime,
               (int) mCurrentHeartInfo.mHeartType, mCurrentHeartInfo.mHeartFailMinCount);
    }
}

// #pragma endregion

// #pragma region NetHeartbeatInfo

NetHeartbeatInfo::NetHeartbeatInfo() {
    Clear();
}

void NetHeartbeatInfo::Clear() {
    mNetDetail = "";
    mNetType = kNoNet;
    mLastModifyTime = 0;
    mCurHeart = MinHeartInterval;
    mHeartSuccCount = mHeartFailCount = mHeartFailMinCount = 0;
    mHeartType = kNoSmartHeartBeat;
    mIsStable = false;
}
