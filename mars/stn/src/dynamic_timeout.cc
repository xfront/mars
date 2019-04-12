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
*  dynamic_timeout.cc
*  network
*
*  Created by caoshaokun on 15/10/28.
*  Copyright © 2015 Tencent. All rights reserved.
*/

#include "dynamic_timeout.h"

#include <string>

#include "mars/comm/time_utils.h"
#include "mars/comm/platform_comm.h"
#include "mars/comm/xlogger/xlogger.h"
#include "mars/stn/config.h"

using namespace mars::stn;

DynamicTimeout::DynamicTimeout()
        : mDyncTimeStatus(kEValuating)
        , mDyncTimeContinuousGoodCount(0)
        , mDyncTimeLatestBigpkgGoodTime(0)
        , mDyncTimeFncountLastModifyTime(0)
        , mDyncTimeFncountPos(-1)
{
    mDyncTimeFailNormalCount.set();
}

DynamicTimeout::~DynamicTimeout() {
}

void DynamicTimeout::CgiTaskStatistic(std::string const& cgiUri, unsigned int totalSize, uint64_t costTime) {
    int task_status = (totalSize == kDynTimeTaskFailedPkgLen || costTime == 0) ? kDynTimeTaskFailedTag
                                                                                   : KDynTimeTaskNormalTag;

    if (task_status == KDynTimeTaskNormalTag) {

        unsigned int small_pkg_costtime =
                kMobile != getNetInfo() ? kDynTimeSmallPackageWifiCosttime : kDynTimeSmallPackageGPRSCosttime;
        unsigned int middle_pkg_costtime =
                kMobile != getNetInfo() ? kDynTimeMiddlePackageWifiCosttime : kDynTimeMiddlePackageGPRSCosttime;
        unsigned int big_pkg_costtime =
                kMobile != getNetInfo() ? kDynTimeBigPackageWifiCosttime : kDynTimeBigPackageGPRSCosttime;
        unsigned int bigger_pkg_costtime =
                kMobile != getNetInfo() ? kDynTimeBiggerPackageWifiCosttime : kDynTimeBiggerPackageGPRSCosttime;

        if (totalSize < kDynTimeSmallPackageLen) {
            if (costTime <= small_pkg_costtime) {
                task_status = kDynTimeTaskMeetExpectTag;
            }
        } else if (totalSize <= kDynTimeMiddlePackageLen) {
            if (costTime <= middle_pkg_costtime) {
                task_status = kDynTimeTaskMidPkgMeetExpectTag;
            }
        } else if (totalSize <= kDynTimeBigPackageLen) {
            if (costTime <= big_pkg_costtime) {
                task_status = kDynTimeTaskBigPkgMeetExpectTag;
            }
        } else if (costTime <= bigger_pkg_costtime) {
            task_status = kDynTimeTaskBiggerPkgMeetExpectTag;
        }
        /*else {
            task_status = DYNTIME_TASK_NORMAL_TAG;
             xdebug2(TSF"totalSize:%_, costTime:%_", totalSize, costTime);
        }*/
    }

    __StatusSwitch(cgiUri, task_status);
}

void DynamicTimeout::ResetStatus() {

    mDyncTimeStatus = kEValuating;
    mDyncTimeLatestBigpkgGoodTime = 0;
    mDyncTimeContinuousGoodCount = 0;
    mDyncTimeFailNormalCount.set();
    mDyncTimeFncountLastModifyTime = 0;
    mDyncTimeFncountPos = -1;
}

int DynamicTimeout::GetStatus() {
    return mDyncTimeStatus;
}

void DynamicTimeout::__StatusSwitch(std::string const& cgiUri, int taskStatus) {
    if (mDyncTimeFncountLastModifyTime == 0 ||
        (gettickcount() - mDyncTimeFncountLastModifyTime) > kDynTimeCountExpireTime) {
        mDyncTimeFncountLastModifyTime = gettickcount();
        mDyncTimeFncountPos = -1;
        if (mDyncTimeStatus == kBad) {
            mDyncTimeFailNormalCount.reset();
        } else {
            mDyncTimeFailNormalCount.set();
        }
    }

    mDyncTimeFncountPos = ++mDyncTimeFncountPos >= mDyncTimeFailNormalCount.size() ? 0 : mDyncTimeFncountPos;

    switch (taskStatus) {
        case kDynTimeTaskMidPkgMeetExpectTag:
        case kDynTimeTaskBigPkgMeetExpectTag:
        case kDynTimeTaskBiggerPkgMeetExpectTag: {
            if (mDyncTimeStatus == kEValuating) {
                mDyncTimeLatestBigpkgGoodTime = gettickcount();
            }
        }
            /* no break, next case*/
        case kDynTimeTaskMeetExpectTag: {
            if (mDyncTimeStatus == kEValuating) {
                mDyncTimeContinuousGoodCount++;
            }

            mDyncTimeFailNormalCount.set(mDyncTimeFncountPos);
        }
            break;
        case KDynTimeTaskNormalTag: {
            if (mDyncTimeStatus == kEValuating) {
                mDyncTimeContinuousGoodCount = 0;
                mDyncTimeLatestBigpkgGoodTime = 0;
            }

            mDyncTimeFailNormalCount.set(mDyncTimeFncountPos);
        }
            break;
        case kDynTimeTaskFailedTag: {
            mDyncTimeContinuousGoodCount = 0;
            mDyncTimeLatestBigpkgGoodTime = 0;
            mDyncTimeFailNormalCount.reset(mDyncTimeFncountPos);
        }
            break;
        default:
            break;
    }

    switch (mDyncTimeStatus) {
        case kEValuating: {
            if (mDyncTimeContinuousGoodCount >= kDynTimeMaxContinuousExcellentCount &&
                (gettickcount() - mDyncTimeLatestBigpkgGoodTime) <= kDynTimeCountExpireTime) {
                xassert2(kDynTimeMaxContinuousExcellentCount >= 10, TSF"max_continuous_good_count:%_", kDynTimeMaxContinuousExcellentCount);
                mDyncTimeStatus = kExcellent;
            } else if (mDyncTimeFailNormalCount.count() <= kDynTimeMinNormalPkgCount) {
                xassert2(kDynTimeMinNormalPkgCount < mDyncTimeFailNormalCount.size(), TSF"DYNTIME_MIN_NORMAL_PKG_COUNT:%_, mDyncTimeFailNormalCount:%_", kDynTimeMinNormalPkgCount,
                         mDyncTimeFailNormalCount.size());
                mDyncTimeStatus = kBad;
                mDyncTimeFncountLastModifyTime = 0;
            }
        }
            break;
        case kExcellent: {
            if (mDyncTimeContinuousGoodCount == 0 && mDyncTimeLatestBigpkgGoodTime == 0) {
                mDyncTimeStatus = kEValuating;
            }
        }
            break;
        case kBad: {
            if (mDyncTimeFailNormalCount.count() > kDynTimeMinNormalPkgCount) {
                xassert2(kDynTimeMinNormalPkgCount < mDyncTimeFailNormalCount.size(), TSF"DYNTIME_MIN_NORMAL_PKG_COUNT:%_, mDyncTimeFailNormalCount:%_", kDynTimeMinNormalPkgCount,
                         mDyncTimeFailNormalCount.size());
                mDyncTimeStatus = kEValuating;
                mDyncTimeFncountLastModifyTime = 0;
            }
        }
            break;
        default:
            break;
    }

    xdebug2(TSF"task_status:%_, good_count:%_, good_time:%_, dyntime_status:%_, dyntime_failed_normal_count_NORMAL:%_, cgi:%_",
            taskStatus, mDyncTimeContinuousGoodCount, mDyncTimeLatestBigpkgGoodTime, mDyncTimeStatus,
            mDyncTimeFailNormalCount.count(), cgiUri);
}
