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
 * dns_profile.h
 *
 *  Created on: 2016年8月12日
 *      Author: caoshaokun
 */

#ifndef MARS_OPEN_MARS_STN_DNS_PROFILE_H_
#define MARS_OPEN_MARS_STN_DNS_PROFILE_H_

#include <string>

#include "mars/stn/stn.h"

namespace mars {
namespace stn {

enum DnsType {
	kType_NewDns = 1,
	kType_Dns = 2
};

struct DnsProfile {

	DnsProfile() {
		Reset();
	}

	void Reset() {
		startTime = gettickcount();
		endTime = 0;

		host.clear();

		errType = 0;
		errCode = 0;

		dnsType = kType_NewDns;
	}

	void OnFailed() {
		errType = kEctLocal;
		errCode = -1;
	}

	uint64_t startTime;
	uint64_t endTime;

	std::string host;

	int errType;
	int errCode;

	int dnsType;

};

}
}


#endif /* MARS_OPEN_MARS_STN_DNS_PROFILE_H_ */
