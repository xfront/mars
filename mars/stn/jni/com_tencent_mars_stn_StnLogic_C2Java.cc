// Tencent is pleased to support the open source community by making Mars available.
// Copyright (C) 2016 THL A29 Limited, a Tencent company. All rights reserved.

// Licensed under the MIT License (the "License"); you may not use this file except in 
// compliance with the License. You may obtain a copy of the License at
// http://opensource.org/licenses/MIT

// Unless required by applicable law or agreed to in writing, software distributed under the License is
// distributed on an "AS IS" basis, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
// either express or implied. See the License for the specific language governing permissions and
// limitations under the License.

/**
 * created on : 2012-10-19
 * author : yanguoyue
 */

#include <jni.h>
#include <vector>

#include "mars/comm/autobuffer.h"
#include "mars/comm/xlogger/xlogger.h"
#include "mars/comm/jni/util/var_cache.h"
#include "mars/comm/jni/util/scope_jenv.h"
#include "mars/comm/jni/util/comm_function.h"
#include "mars/comm/jni/util/scoped_jstring.h"
#include "mars/comm/compiler_util.h"
#include "mars/comm/platform_comm.h"
#include "mars/stn/stn.h"
#include "mars/stn/task_profile.h"
#include "mars/boost/signals2.hpp"

DEFINE_FIND_CLASS(KC2Java, "com/tencent/mars/stn/StnLogic")

namespace mars {
namespace stn {

extern boost::signals2::signal<void (ErrCmdType errType, int errCode, const std::string& ip, uint16_t port)> SignalOnLongLinkNetworkError;
extern boost::signals2::signal<void (ErrCmdType errType, int errCode, const std::string& ip, const std::string& host, uint16_t port)> SignalOnShortLinkNetworkError;
    
DEFINE_FIND_STATIC_METHOD(KC2Java_onTaskEnd, KC2Java, "onTaskEnd", "(ILjava/lang/Object;II)I")
int (*OnTaskEnd)(uint32_t taskId, void* const userContext, int errorType, int errorCode)
= [](uint32_t taskId, void* const userContext, int errorType, int errorCode) {

    xverbose_function();

	VarCache* cache_instance = VarCache::Singleton();

	ScopeJEnv scope_jenv(cache_instance->GetJvm());
	JNIEnv *env = scope_jenv.GetEnv();

	int ret = (int)JNU_CallStaticMethodByMethodInfo(env, KC2Java_onTaskEnd, (jint)taskId, userContext, (jint)errorType, (jint)errorCode).i;

	return ret;
};

DEFINE_FIND_STATIC_METHOD(KC2Java_onPush, KC2Java, "onPush", "(I[B)V")
void (*OnPush)(uint64_t channelId, uint32_t cmdId, uint32_t taskId, const AutoBuffer& body, const AutoBuffer& bufExt)
= [](uint64_t channelId, uint32_t cmdId, uint32_t taskId, const AutoBuffer& body, const AutoBuffer& bufExt) {

    xverbose_function();

	VarCache* cache_instance = VarCache::Singleton();

	ScopeJEnv scope_jenv(cache_instance->GetJvm());
	JNIEnv *env = scope_jenv.GetEnv();

	jbyteArray data_jba = NULL;

	if (body.Length() > 0) {
		data_jba = JNU_Buffer2JbyteArray(env, body);
	} else {
		xdebug2(TSF"the data.Lenght() < = 0");
	}

	JNU_CallStaticMethodByMethodInfo(env, KC2Java_onPush, (jint)cmdId, data_jba);

	if (data_jba != NULL) {
		JNU_FreeJbyteArray(env, data_jba);
	}

};

DEFINE_FIND_STATIC_METHOD(KC2Java_onNewDns, KC2Java, "onNewDns", "(Ljava/lang/String;)[Ljava/lang/String;")
std::vector<std::string> (*OnNewDns)(const std::string& host)
= [](const std::string& host) {
	xverbose_function();

	VarCache* cache_instance = VarCache::Singleton();
	ScopeJEnv scope_jenv(cache_instance->GetJvm());
	JNIEnv *env = scope_jenv.GetEnv();
	std::vector<std::string> iplist;

	if (!host.empty()) {
		jobjectArray ip_strs = (jobjectArray)JNU_CallStaticMethodByMethodInfo(env, KC2Java_onNewDns, ScopedJstring(env, host.c_str()).GetJstr()).l;
		if (ip_strs != NULL) {
			jsize size = env->GetArrayLength(ip_strs);
			for (int i = 0; i < size; i++) {
				jstring ip = (jstring)env->GetObjectArrayElement(ip_strs, i);
				if (ip != NULL) {
					iplist.push_back(ScopedJstring(env, ip).GetChar());
				}
				JNU_FreeJstring(env, ip);
			}
			env->DeleteLocalRef(ip_strs);
		}
	}
	else {
		xerror2(TSF"host is empty");
	}

	return iplist;
};

DEFINE_FIND_STATIC_METHOD(KC2Java_req2Buf, KC2Java, "req2Buf", "(ILjava/lang/Object;Ljava/io/ByteArrayOutputStream;[II)Z")
bool (*Req2Buf)(uint32_t taskId,  void* const userContext, AutoBuffer& _outbuffer,  AutoBuffer& bufExt, int& errorCode, const int channelSelect)
= [](uint32_t taskId,  void* const userContext, AutoBuffer& _outbuffer,  AutoBuffer& bufExt, int& errorCode, const int channelSelect) -> bool {

    xverbose_function();

	VarCache* cache_instance = VarCache::Singleton();
	ScopeJEnv scope_jenv(cache_instance->GetJvm());
	JNIEnv *env = scope_jenv.GetEnv();

	//obtain the class ByteArrayOutputStream
	jclass byte_array_outputstream_clz = cache_instance->GetClass(env, "java/io/ByteArrayOutputStream");

	//obtain the method id of construct method
	jmethodID construct_mid = cache_instance->GetMethodId(env, byte_array_outputstream_clz, "<init>", "()V");

	//construct  the object of ByteArrayOutputStream
	jobject byte_array_output_stream_obj = env->NewObject(byte_array_outputstream_clz, construct_mid);


	jintArray errcode_array = env->NewIntArray(2);

	jboolean ret = JNU_CallStaticMethodByMethodInfo(env, KC2Java_req2Buf, (jint)taskId, userContext, byte_array_output_stream_obj, errcode_array, channelSelect).z;

	if (ret) {
		jbyteArray ret_byte_array = (jbyteArray)JNU_CallMethodByName(env, byte_array_output_stream_obj, "toByteArray", "()[B").l;
		if (ret_byte_array != NULL) {
			jsize alen = env->GetArrayLength(ret_byte_array);
			jbyte* ba = env->GetByteArrayElements(ret_byte_array, NULL);
			_outbuffer.Write(ba, alen);
			env->ReleaseByteArrayElements(ret_byte_array, ba, 0);
			env->DeleteLocalRef(ret_byte_array);
		} else {
			xdebug2(TSF"the retByteArray is null");
		}
	}
	env->DeleteLocalRef(byte_array_output_stream_obj);

	jint* errcode = env->GetIntArrayElements(errcode_array, NULL);
	errorCode = errcode[0];
	env->ReleaseIntArrayElements(errcode_array, errcode, 0);
	env->DeleteLocalRef(errcode_array);

	return ret;
};

DEFINE_FIND_STATIC_METHOD(KC2Java_buf2Resp, KC2Java, "buf2Resp", "(ILjava/lang/Object;[B[II)I")
int (*Buf2Resp)(uint32_t taskId, void* const userContext, const AutoBuffer& _inbuffer, const AutoBuffer& bufExt, int& errorCode, const int channelSelect)
= [](uint32_t taskId, void* const userContext, const AutoBuffer& _inbuffer, const AutoBuffer& bufExt, int& errorCode, const int channelSelect) {

    xverbose_function();

	VarCache* cache_instance = VarCache::Singleton();
	ScopeJEnv scope_jenv(cache_instance->GetJvm());
	JNIEnv *env = scope_jenv.GetEnv();

	jbyteArray resp_buf_jba = NULL;

	if (_inbuffer.Length() > 0) {
		resp_buf_jba =  JNU_Buffer2JbyteArray(env, _inbuffer);
	} else {
		xdebug2(TSF"the decodeBuffer.Lenght() <= 0");
	}

	jintArray errcode_array = env->NewIntArray(1);

	jint ret = JNU_CallStaticMethodByMethodInfo(env, KC2Java_buf2Resp, (jint)taskId, userContext, resp_buf_jba, errcode_array, channelSelect).i;

	if (resp_buf_jba != NULL) {
		env->DeleteLocalRef(resp_buf_jba);
	}

    jint* errcode = env->GetIntArrayElements(errcode_array, NULL);
    errorCode = errcode[0];
    env->ReleaseIntArrayElements(errcode_array, errcode, 0);
    env->DeleteLocalRef(errcode_array);

	return ret;
};

DEFINE_FIND_STATIC_METHOD(KC2Java_makesureAuthed, KC2Java, "makesureAuthed", "()Z")
bool (*MakesureAuthed)()
= []() -> bool {
    xverbose_function();

    VarCache* cache_instance = VarCache::Singleton();
	ScopeJEnv scope_jenv(cache_instance->GetJvm());
	JNIEnv *env = scope_jenv.GetEnv();

	jboolean ret = JNU_CallStaticMethodByMethodInfo(env, KC2Java_makesureAuthed).z;

	return ret;
};

DEFINE_FIND_STATIC_METHOD(KC2Java_getLongLinkIdentifyCheckBuffer, KC2Java, "getLongLinkIdentifyCheckBuffer", "(Ljava/io/ByteArrayOutputStream;Ljava/io/ByteArrayOutputStream;[I)I")
int (*GetLonglinkIdentifyCheckBuffer)(AutoBuffer& _identify_buffer, AutoBuffer& _buffer_hash, int32_t& cmdId)
= [](AutoBuffer& _identify_buffer, AutoBuffer& _buffer_hash, int32_t& cmdId) {
    xverbose_function();
    
    VarCache* cache_instance = VarCache::Singleton();
	ScopeJEnv scope_jenv(cache_instance->GetJvm());
	JNIEnv *env = scope_jenv.GetEnv();
    
	//obtain the class ByteArrayOutputStream
	jclass byte_array_output_stream_clz = cache_instance->GetClass(env, "java/io/ByteArrayOutputStream");
    
	//obtain the method id of construct method
	jmethodID construct_mid = cache_instance->GetMethodId(env, byte_array_output_stream_clz, "<init>", "()V");
    
	//construct  the object of ByteArrayOutputStream
	jobject byte_array_outputstream_obj = env->NewObject(byte_array_output_stream_clz, construct_mid);
	jobject byte_array_outputstream_hash = env->NewObject(byte_array_output_stream_clz, construct_mid);

    jintArray jcmdid_array = env->NewIntArray(2);
    
	jint ret = 0;
	ret = JNU_CallStaticMethodByMethodInfo(env, KC2Java_getLongLinkIdentifyCheckBuffer, byte_array_outputstream_obj, byte_array_outputstream_hash, jcmdid_array).i;
	if (ret == kCheckNext || ret == kCheckNever)
	{
		xwarn2(TSF"getLongLinkIdentifyCheckBuffer uin == 0, not ready");
		env->DeleteLocalRef(byte_array_outputstream_obj);
		env->DeleteLocalRef(byte_array_outputstream_hash);
        env->DeleteLocalRef(jcmdid_array);
		return ret;
	}
    
	jbyteArray ret_byte_array = NULL;
	ret_byte_array = (jbyteArray)JNU_CallMethodByName(env, byte_array_outputstream_obj, "toByteArray", "()[B").l;
    
	jbyteArray ret_byte_hash = NULL;
	ret_byte_hash = (jbyteArray)JNU_CallMethodByName(env, byte_array_outputstream_hash, "toByteArray", "()[B").l;
    
    
    jint* jcmdids = env->GetIntArrayElements(jcmdid_array, NULL);
    cmdId = (int)jcmdids[0];
    env->ReleaseIntArrayElements(jcmdid_array, jcmdids, 0);
    env->DeleteLocalRef(jcmdid_array);


	if (ret_byte_hash != NULL) {
		jsize alen2 = env->GetArrayLength(ret_byte_hash);
		jbyte* ba2 = env->GetByteArrayElements(ret_byte_hash, NULL);
		_buffer_hash.Write(ba2, alen2);
		env->ReleaseByteArrayElements(ret_byte_hash, ba2, 0);
		env->DeleteLocalRef(ret_byte_hash);
	}
	if (ret_byte_array != NULL) {
		jsize alen = env->GetArrayLength(ret_byte_array);
		jbyte* ba = env->GetByteArrayElements(ret_byte_array, NULL);
		_identify_buffer.Write(ba, alen);
		env->ReleaseByteArrayElements(ret_byte_array, ba, 0);
		env->DeleteLocalRef(ret_byte_array);
        
	} else {
		xdebug2(TSF"the retByteArray is NULL");
	}
    
	//free the local reference
	env->DeleteLocalRef(byte_array_outputstream_obj);
	env->DeleteLocalRef(byte_array_outputstream_hash);

	return ret;
};

DEFINE_FIND_STATIC_METHOD(KC2Java_onLongLinkIdentifyResp, KC2Java, "onLongLinkIdentifyResp", "([B[B)Z")
bool (*OnLonglinkIdentifyResponse)(const AutoBuffer& _response_buffer, const AutoBuffer& _identify_buffer_hash)
= [](const AutoBuffer& _response_buffer, const AutoBuffer& _identify_buffer_hash) {
    xverbose_function();

	VarCache* cache_instance = VarCache::Singleton();

	ScopeJEnv scope_jenv(cache_instance->GetJvm());
	JNIEnv *env = scope_jenv.GetEnv();

	jbyteArray data_jba = NULL;

	if (_response_buffer.Length() > 0) {
		data_jba = JNU_Buffer2JbyteArray(env, _response_buffer);
	} else {
		xdebug2(TSF"the respbuffer.Lenght() < = 0");
	}

	jbyteArray hash_jba = NULL;
	if (_identify_buffer_hash.Length() > 0) {
		hash_jba = JNU_Buffer2JbyteArray(env, _identify_buffer_hash);
	} else {
		xdebug2(TSF"the hashCodeBuffer.Lenght() < = 0");
	}

	jboolean ret = JNU_CallStaticMethodByMethodInfo(env, KC2Java_onLongLinkIdentifyResp, data_jba, hash_jba).z;

	if (data_jba != NULL) {
		JNU_FreeJbyteArray(env, data_jba);
	}

	if (hash_jba != NULL) {
		JNU_FreeJbyteArray(env, hash_jba);
	}

    return ret != 0;
};


DEFINE_FIND_STATIC_METHOD(KC2Java_trafficData, KC2Java, "trafficData", "(II)V")
void (*TrafficData)(ssize_t _send, ssize_t _recv) 
= [](ssize_t _send, ssize_t _recv) {

	VarCache* cache_instance = VarCache::Singleton();
	ScopeJEnv scope_jenv(cache_instance->GetJvm());
	JNIEnv *env = scope_jenv.GetEnv();

	JNU_CallStaticMethodByMethodInfo(env, KC2Java_trafficData, (jint)_send, (jint)_recv);

};

DEFINE_FIND_STATIC_METHOD(KC2Java_reportNetConnectInfo, KC2Java, "reportConnectStatus", "(II)V")
void (*ReportConnectStatus)(int _all_connstatus, int _longlink_connstatus)
= [](int _all_connstatus, int _longlink_connstatus) {
    xverbose_function();

    VarCache* cache_instance = VarCache::Singleton();
    ScopeJEnv scope_jenv(cache_instance->GetJvm());
    JNIEnv *env = scope_jenv.GetEnv();
    JNU_CallStaticMethodByMethodInfo(env, KC2Java_reportNetConnectInfo, (jint)_all_connstatus, (jint)_longlink_connstatus);
    xdebug2(TSF"all_connstatus = %0, longlink_connstatus = %_", _all_connstatus, _longlink_connstatus);
};

//DEFINE_FIND_STATIC_METHOD(KC2Java_reportCrashStatistics, KC2Java, "reportCrashStatistics", "(Ljava/lang/String;Ljava/lang/String;)V")
void reportCrashStatistics(const char* _raw, const char* type)
{
}

DEFINE_FIND_STATIC_METHOD(KC2Java_requestSync, KC2Java, "requestDoSync", "()V")
void (*RequestSync)()
= []() {
    xverbose_function();

    VarCache* cache_instance = VarCache::Singleton();
    ScopeJEnv scope_jenv(cache_instance->GetJvm());
    JNIEnv *env = scope_jenv.GetEnv();
    JNU_CallStaticMethodByMethodInfo(env, KC2Java_requestSync);

};

DEFINE_FIND_STATIC_METHOD(KC2Java_requestNetCheckShortLinkHosts, KC2Java, "requestNetCheckShortLinkHosts", "()[Ljava/lang/String;")
void (*RequestNetCheckShortLinkHosts)(std::vector<std::string>& hostList)
= [](std::vector<std::string>& hostList) {
	xverbose_function();

	VarCache* cache_instance = VarCache::Singleton();
	ScopeJEnv scope_jenv(cache_instance->GetJvm());
	JNIEnv *env = scope_jenv.GetEnv();

	jobjectArray jobj_arr = (jobjectArray)JNU_CallStaticMethodByMethodInfo(env, KC2Java_requestNetCheckShortLinkHosts).l;

	if (jobj_arr != NULL) {
		jsize size = env->GetArrayLength(jobj_arr);
		for (int i = 0; i < size; i++) {
			jstring host = (jstring)env->GetObjectArrayElement(jobj_arr, i);
			if (host != NULL) {
				hostList.push_back(ScopedJstring(env, host).GetChar());
			}
			JNU_FreeJstring(env, host);
		}

		env->DeleteLocalRef(jobj_arr);
	}
};

DEFINE_FIND_STATIC_METHOD(KC2Java_reportTaskProfile, KC2Java, "reportTaskProfile", "(Ljava/lang/String;)V")
void (*ReportTaskProfile)(const TaskProfile& taskProfile)
= [](const TaskProfile& taskProfile) {
	xverbose_function();

	VarCache* cache_instance = VarCache::Singleton();
	ScopeJEnv scope_jenv(cache_instance->GetJvm());
	JNIEnv *env = scope_jenv.GetEnv();

	XMessage profile_json;
	profile_json << "{";
	profile_json << "\"taskId\":" << taskProfile.task.taskId;
	profile_json << ",\"cmdId\":" << taskProfile.task.cmdId;
	profile_json << ",\"cgi\":\"" << taskProfile.task.cgi << "\"";
	profile_json << ",\"startTaskTime\":" << taskProfile.startTaskTime;
	profile_json << ",\"endTaskTime\":" << taskProfile.endTaskTime;
	profile_json << ",\"dyntimeStatus\":" << taskProfile.currentDyntimeStatus;
	profile_json << ",\"errCode\":" << taskProfile.errCode;
	profile_json << ",\"errType\":" << taskProfile.errType;
	profile_json << ",\"channelSelect\":" << taskProfile.linkType;
	profile_json << ",\"historyNetLinkers\":[";
	std::vector<TransferProfile>::const_iterator iter = taskProfile.historyTransferProfiles.begin();
	for (; iter != taskProfile.historyTransferProfiles.end(); ) {
		const ConnectProfile& connect_profile = iter->connectProfile;
		profile_json << "{";
		profile_json << "\"startTime\":" << connect_profile.startTime;
		profile_json << ",\"dnsTime\":" << connect_profile.dnsTime;
		profile_json << ",\"dnsEndTime\":" << connect_profile.dnsEndtime;
		profile_json << ",\"connTime\":" << connect_profile.connTime;
		profile_json << ",\"connErrCode\":" << connect_profile.connErrcode;
		profile_json << ",\"tryIPCount\":" << connect_profile.tryIpCount;
		profile_json << ",\"ip\":\"" << connect_profile.ip << "\"";
		profile_json << ",\"port\":" << connect_profile.port;
		profile_json << ",\"host\":\"" << connect_profile.host << "\"";
		profile_json << ",\"ipType\":" << connect_profile.ipType;
		profile_json << ",\"disconnTime\":" << connect_profile.disconnTime;
		profile_json << ",\"disconnErrType\":" << connect_profile.disconnErrType;
		profile_json << ",\"disconnErrCode\":" << connect_profile.disconnErrCode;

		profile_json << "}";
		if (++iter != taskProfile.historyTransferProfiles.end()) {
			profile_json << ",";
		}
		else {
			break;
		}
	}
	profile_json << "]}";
	std::string report_task_str = profile_json.String();

	JNU_CallStaticMethodByMethodInfo(env, KC2Java_reportTaskProfile, ScopedJstring(env, report_task_str.c_str()).GetJstr());
};

void (*ReportTaskLimited)(int checkType, const Task& task, unsigned int& param)
= [](int checkType, const Task& task, unsigned int& param) {

};

void (*ReportDnsProfile)(const DnsProfile& dnsProfile)
= [](const DnsProfile& dnsProfile) {
};

void (*OnLongLinkNetworkError)(ErrCmdType errType, int errCode, const std::string& ip, uint16_t port)
= [](ErrCmdType errType, int errCode, const std::string& ip, uint16_t port) {
    SignalOnLongLinkNetworkError(errType, errCode, ip, port);
};
    
void (*OnShortLinkNetworkError)(ErrCmdType errType, int errCode, const std::string& ip, const std::string& host, uint16_t port)
= [](ErrCmdType errType, int errCode, const std::string& ip, const std::string& host, uint16_t port) {
    SignalOnShortLinkNetworkError(errType, errCode, ip, host, port);
};
void (*OnLongLinkStatusChange)(int status)
= [](int status) {

};
}
}


