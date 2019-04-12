// Tencent is pleased to support the open source community by making Mars available.
// Copyright (C) 2016 THL A29 Limited, a Tencent company. All rights reserved.

// Licensed under the MIT License (the "License"); you may not use this file except in 
// compliance with the License. You may obtain a copy of the License at
// http://opensource.org/licenses/MIT

// Unless required by applicable law or agreed to in writing, software distributed under the License is
// distributed on an "AS IS" basis, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
// either express or implied. See the License for the specific language governing permissions and
// limitations under the License.

//
//  task_profile.h
//  stn
//
//  Created by yerungui on 16/3/23.
//  Copyright © 2016年 Tencent. All rights reserved.
//

#ifndef TASK_PROFILE_H_
#define TASK_PROFILE_H_

#include <list>

#include "boost/shared_ptr.hpp"

#include "mars/comm/time_utils.h"
#include "mars/comm/comm_data.h"
#include "mars/stn/stn.h"
#include "mars/stn/config.h"

namespace mars {
namespace stn  {

struct ProfileExtension {

	ProfileExtension() {}
	virtual ~ProfileExtension() {}

	virtual void Reset() {}
};

struct NoopProfile {

    NoopProfile() {
    	Reset();
    }

    void Reset() {
        success = false;
        noop_internal = 0;
        noop_actual_internal = 0;
    	noop_cost = 0;
        noop_starttime = 0;
    }

    bool     success;
    uint64_t noop_internal;
    uint64_t noop_actual_internal;
    uint64_t noop_cost;
    uint64_t noop_starttime;
};


struct ConnectProfile {
    
    ConnectProfile() {
        Reset();
    }
    
    void Reset(){
        netType.clear();
        tid = 0;
        
        startTime = 0;
        dnsTime = 0;
        dnsEndtime = 0;
        //todo
        ipList.clear();
        
        connReason = 0;
        connTime = 0;
        connErrcode = 0;
        ip.clear();
        port = 0;
        host.clear();
        ipType = kIPSourceNULL;
        connRtt = 0;
        connCost = 0;
        tryIpCount = 0;
        sendRequestCost = 0;
        recvReponseCost = 0;

        localIp.clear();
        localPort = 0;
        ipIndex = -1;
        
        disconnTime = 0;
        disconnErrType = kEctOK;
        disconnErrCode = 0;
        disconnSignal = 0;

        nat64 = false;

        noopProfiles.clear();
        if (profileExt)
        		profileExt->Reset();
    }
    
    std::string netType;
    intmax_t tid;
    
    uint64_t startTime;
    uint64_t dnsTime;
    uint64_t dnsEndtime;
    std::vector<IPPortItem> ipList;
    
    int connReason;
    uint64_t connTime;
    int connErrcode;
    unsigned int connRtt;
    unsigned long connCost;
    int tryIpCount;
    uint64_t sendRequestCost;
    uint64_t recvReponseCost;

    std::string ip;
    uint16_t port;
    std::string host;
    IPSourceType ipType;
    std::string localIp;
    uint16_t localPort;
    int ipIndex;
    
    uint64_t disconnTime;
    ErrCmdType disconnErrType;
    int disconnErrCode;
    unsigned int disconnSignal;

    bool nat64;

    std::vector<NoopProfile> noopProfiles;

    boost::shared_ptr<ProfileExtension> profileExt;
    mars::comm::ProxyInfo proxyInfo;
};

        
struct TransferProfile {
    TransferProfile(const Task& task):task(task){
        Reset();
    }
    
    void Reset() {
        connectProfile.Reset();
        loopStartTaskTime = 0;
        firstStartSendTime = 0;
        startSendTime = 0;
        lastReceivePkgTime = 0;
        readWriteTimeout = 0;
        firstPkgTimeout = 0;
        
        sentSize = 0;
        sendDataSize = 0;
        receivedSize = 0;
        receiveDataSize = 0;
        
        externalIp.clear();
        
        errorType = 0;
        errorCode = 0;
    }
    
    const Task& task;
    ConnectProfile connectProfile;
    
    uint64_t loopStartTaskTime;  // ms
    uint64_t firstStartSendTime; //ms
    uint64_t startSendTime;    // ms
    uint64_t lastReceivePkgTime;  // ms
    uint64_t readWriteTimeout;    // ms
    uint64_t firstPkgTimeout;  // ms
    
    size_t sentSize;
    size_t sendDataSize;
    size_t receivedSize;
    size_t receiveDataSize;
    
    std::string externalIp;
    
    int errorType;
    int errorCode;
};
    
//do not insert or delete
enum TaskFailStep {
    kStepSucc = 0,
    kStepDns,
    kStepConnect,
    kStepFirstPkg,
    kStepPkgPkg,
    kStepDecode,
    kStepOther,
    kStepTimeout,
    kStepServer,
};
        
struct TaskProfile {
    
    static uint64_t ComputeTaskTimeout(const Task& task) {
        uint64_t readwritetimeout = 15 * 1000;
        
        if (0 < task.serverProcessCost)
            readwritetimeout = task.serverProcessCost + 15 * 1000;
        
        int trycount = 0;// DEF_TASK_RETRY_COUNT;
        
        if (0 <= task.retryCount)
            trycount = task.retryCount;
        
        trycount++;
        
        uint64_t task_timeout = (readwritetimeout + 5 * 1000) * trycount;
        
        if (0 < task.totalTimetout &&  (uint64_t)task.totalTimetout < task_timeout)
            task_timeout = task.totalTimetout;
        
        return  task_timeout;
    }
    
    TaskProfile(const Task& task):task(task), transferProfile(task), taskTimeout(ComputeTaskTimeout(task)), startTaskTime(::gettickcount()){
        
        remainRetryCount = task.retryCount;
        forceNoRetry = false;
        
        runningId = 0;
        
        endTaskTime = 0;
        retryStartTime = 0;

        lastFailedDyntimeStatus = 0;
        currentDyntimeStatus = 0;
        
        antiAvalancheChecked = false;
        
        useProxy = false;
        retryTimeInterval = 0;

        errType = kEctOK;
        errCode = 0;
        linkType = 0;
    }
    
    void InitSendParam() {
        transferProfile.Reset();
        runningId = 0;
    }
    
    void PushHistory() {
        historyTransferProfiles.push_back(transferProfile);
    }
    
    TaskFailStep GetFailStep() const {
        if(kEctOK == errType && 0 == errCode) return kStepSucc;
        if(kEctDns == errType) return kStepDns;
        if(transferProfile.connectProfile.ipIndex == -1) return kStepConnect;
        if(transferProfile.lastReceivePkgTime == 0) return kStepFirstPkg;
        if(kEctEnDecode == errType)    return kStepDecode;
        if(kEctSocket == errType || kEctHttp == errType || kEctNetMsgXP == errType)  return kStepPkgPkg;
        if(kEctLocalTaskTimeout == errCode)    return kStepTimeout;
        if(kEctServer == errType || (kEctOK == errType && errCode != 0))    return kStepServer;
        return kStepOther;
    }

    const Task task;
    TransferProfile transferProfile;
    intptr_t runningId;
    
    const uint64_t taskTimeout;
    const uint64_t startTaskTime;  // ms
    uint64_t endTaskTime;	//ms
    uint64_t retryStartTime;

    int remainRetryCount;
    bool forceNoRetry;
    
    int lastFailedDyntimeStatus;
    int currentDyntimeStatus;
    
    bool antiAvalancheChecked;
    
    bool useProxy;
    uint64_t retryTimeInterval;    // ms

    ErrCmdType errType;
    int errCode;
    int linkType;

    std::vector<TransferProfile> historyTransferProfiles;
};
        

void __SetLastFailedStatus(std::list<TaskProfile>::iterator it);
uint64_t __ReadWriteTimeout(uint64_t  _first_pkg_timeout);
uint64_t  __FirstPkgTimeout(int64_t  initFirstPkgTimeout, size_t sendLen, int sendCount, int dynamicTimeoutStatus);
bool __CompareTask(const TaskProfile& _first, const TaskProfile& _second);
}}

#endif

