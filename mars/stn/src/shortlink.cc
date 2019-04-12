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
 * shortlink.cc
 *
 *  Created on: 2012-8-22
 *      Author: zhouzhijie
 */

#include "shortlink.h"

#include "boost/bind.hpp"

#include "mars/comm/xlogger/xlogger.h"
#include "mars/comm/socket/complexconnect.h"
#include "mars/comm/socket/unix_socket.h"
#include "mars/comm/socket/socket_address.h"
#include "mars/comm/socket/local_ipstack.h"
#include "mars/comm/socket/block_socket.h"
#include "mars/comm/strutil.h"
#include "mars/comm/time_utils.h"
#include "mars/comm/http.h"
#include "mars/comm/platform_comm.h"
#include "mars/app/app.h"
#include "mars/comm/crypt/ibase64.h"
#include "mars/baseevent/baseprjevent.h"
#include "mars/comm/move_wrapper.h"

#if defined(__ANDROID__) || defined(__APPLE__)

#include "mars/comm/socket/getsocktcpinfo.h"

#endif

#include "mars/stn/proto/shortlink_packer.h"

#include "weak_network_logic.h"


#define AYNC_HANDLER mAsyncReg.Get()
#define STATIC_RETURN_SYNC2ASYNC_FUNC(func) RETURN_SYNC2ASYNC_FUNC(func, )

using namespace mars::stn;
using namespace mars::app;
using namespace http;

static unsigned int KBufferSize = 8 * 1024;

namespace mars {
namespace stn {

class ShortLinkConnectObserver : public IComplexConnect {
public:
    ShortLinkConnectObserver(ShortLink &shortLink)
    : mShortLink(shortLink)
    , mRtt(0)
    , mLastError(-1)
    {
        memset(ConnectingIndex, 0, sizeof(ConnectingIndex));
    };

    virtual void OnCreated(unsigned int index, const socket_address &addr, SOCKET socket) {}

    virtual void OnConnect(unsigned int index, const socket_address &addr, SOCKET socket) {
        ConnectingIndex[index] = 1;
    }

    virtual void OnConnected(unsigned int index, const socket_address &addr, SOCKET socket, int error, int rtt) {
        ConnectingIndex[index] = 0;

        if (0 != error) {
//            xassert2(mShortLink.OnNetReport);

            if (index < mShortLink.Profile().ipList.size() && mShortLink.OnNetReport)
                mShortLink.OnNetReport(__LINE__, kEctSocket, error, addr.ip(),
                                               mShortLink.Profile().ipList[index].host, addr.port());
        }

        if (mLastError != 0) {
            mLastError = error;
            mRtt = rtt;
        }
    }

    int LastErrorCode() const { return mLastError; }

    int Rtt() const { return mRtt; }

    char ConnectingIndex[32];

private:
    ShortLinkConnectObserver(const ShortLinkConnectObserver &);

    ShortLinkConnectObserver &operator=(const ShortLinkConnectObserver &);

private:
    ShortLink &mShortLink;
    int mRtt;
    int mLastError;
};

}
}
///////////////////////////////////////////////////////////////////////////////////////

ShortLink::ShortLink(MessageQueue::MessageQueue_t msgQueueId, NetSource &netSource, const Task &task,
                     bool useProxy)
        : mAsyncReg(MessageQueue::InstallAsyncHandler(msgQueueId)), mNetSource(netSource), mTask(task),
          mThread(boost::bind(&ShortLink::__Run, this), XLOGGER_TAG "::shortlink"), mUseProxy(useProxy),
          mTracker(shortlink_tracker::Create()) {
    xinfo2(TSF"%_, handler:(%_,%_)", XTHIS, mAsyncReg.Get().queue, mAsyncReg.Get().seq);
    xassert2(mBreaker.IsCreateSuc(), "Create Breaker Fail!!!");
}

ShortLink::~ShortLink() {
    xinfo_function(TSF"taskId:%_, cgi:%_, @%_", mTask.taskId, mTask.cgi, this);
    __CancelAndWaitWorkerThread();
    mAsyncReg.CancelAndWait();
}

void ShortLink::SendRequest(AutoBuffer &bufReq, AutoBuffer &bufExt) {
    xverbose_function();
    xdebug2(XTHIS)(TSF "bufReq.size:%_", bufReq.Length());
    mSendBody.Attach(bufReq);
    mSendExtend.Attach(bufExt);
    mThread.start();
}

void ShortLink::__Run() {
    xmessage2_define(message, TSF"taskId:%_, cgi:%_, @%_", mTask.taskId, mTask.cgi, this);
    xinfo_function(TSF"%_, net:%_", message.String(), getNetInfo());

    ConnectProfile conn_profile;
    getCurrNetLabel(conn_profile.netType);
    conn_profile.startTime = ::gettickcount();
    conn_profile.tid = xlogger_tid();
    __UpdateProfile(conn_profile);

    SOCKET fd_socket = __RunConnect(conn_profile);

    if (INVALID_SOCKET == fd_socket) return;
    if (OnSend) {
        OnSend(this);
    } else {
        xwarn2(TSF"OnSend NULL.");
    }
    int errtype = 0;
    int errcode = 0;
    __RunReadWrite(fd_socket, errtype, errcode, conn_profile);

    conn_profile.disconnSignal = ::getSignal(::getNetInfo() == kWifi);
    __UpdateProfile(conn_profile);

    socket_close(fd_socket);
}


SOCKET ShortLink::__RunConnect(ConnectProfile &connProfile) {
    xmessage2_define(message)(TSF "taskId:%_, cgi:%_, @%_", mTask.taskId, mTask.cgi, this);

    std::vector<socket_address> vecaddr;

    connProfile.dnsTime = ::gettickcount();
    __UpdateProfile(connProfile);

    if (!mTask.shortLinkHostList.empty()) connProfile.host = mTask.shortLinkHostList.front();

    if (mUseProxy) {
        connProfile.proxyInfo = mars::app::GetProxyInfo(connProfile.host);
    }

    bool use_proxy = mUseProxy && connProfile.proxyInfo.IsValid();
    bool isnat64 = ELocalIPStack_IPv6 == local_ipstack_detect();

    if (use_proxy && mars::comm::kProxyHttp == connProfile.proxyInfo.type &&
        mNetSource.GetShortLinkDebugIP().empty()) {
        connProfile.ip = connProfile.proxyInfo.ip;
        connProfile.port = connProfile.proxyInfo.port;
        connProfile.ipType = kIPSourceProxy;
        IPPortItem item = {connProfile.ip, mNetSource.GetShortLinkPort(), connProfile.ipType, connProfile.host};
        connProfile.ipList.push_back(item);
        __UpdateProfile(connProfile);
    } else {
        if (!mOutterAddrs.empty()) {
            connProfile.ipList = mOutterAddrs;
        } else {
            mNetSource.GetShortLinkItems(mTask.shortLinkHostList, connProfile.ipList, mDnsUtil);
        }

        if (!connProfile.ipList.empty()) {
            connProfile.host = connProfile.ipList[0].host;
            connProfile.ipType = connProfile.ipList[0].sourceType;
            connProfile.ip = connProfile.ipList[0].ip;
            connProfile.port = connProfile.ipList[0].port;
            __UpdateProfile(connProfile);
        }
    }

    std::string proxy_ip;
    if (use_proxy && mars::comm::kProxyNone != connProfile.proxyInfo.type) {
        std::vector<std::string> proxy_ips;
        if (connProfile.proxyInfo.ip.empty() && !connProfile.proxyInfo.host.empty()) {
            if (!mDnsUtil.GetDNS().GetHostByName(connProfile.proxyInfo.host, proxy_ips) || proxy_ips.empty()) {
                xwarn2(TSF"dns %_ error", connProfile.proxyInfo.host);
                return INVALID_SOCKET;
            }
            proxy_ip = proxy_ips.front();
        } else {
            proxy_ip = connProfile.proxyInfo.ip;
        }
    }

    if (use_proxy && mars::comm::kProxyHttp == connProfile.proxyInfo.type) {
        vecaddr.push_back(socket_address(proxy_ip.c_str(), connProfile.proxyInfo.port).v4tov6_address(isnat64));
    } else {
        for (size_t i = 0; i < connProfile.ipList.size(); ++i) {
            if (!use_proxy || mars::comm::kProxyNone == connProfile.proxyInfo.type) {
                vecaddr.push_back(socket_address(connProfile.ipList[i].ip.c_str(),
                                                 connProfile.ipList[i].port).v4tov6_address(isnat64));
            } else {
                vecaddr.push_back(
                        socket_address(connProfile.ipList[i].ip.c_str(), connProfile.ipList[i].port));
            }
        }
    }

    socket_address *proxy_addr = NULL;
    if (use_proxy && (mars::comm::kProxyHttpTunel == connProfile.proxyInfo.type ||
                      mars::comm::kProxySocks5 == connProfile.proxyInfo.type)) {
        proxy_addr = &((new socket_address(proxy_ip.c_str(), connProfile.proxyInfo.port))->v4tov6_address(isnat64));
        connProfile.ipType = kIPSourceProxy;
    }

    xinfo2(TSF"task socket dns sock %_ proxy:%_, host:%_, ip list:%_", message.String(),
           kIPSourceProxy == connProfile.ipType, connProfile.host, NetSource::DumpTable(connProfile.ipList));

    if (vecaddr.empty()) {
        xerror2(TSF"task socket connect fail %_ vecaddr empty", message.String());
        __RunResponseError(kEctDns, kEctDnsMakeSocketPrepared, connProfile, false);
        delete proxy_addr;
        return INVALID_SOCKET;
    }

    connProfile.host = connProfile.ipList[0].host;
    connProfile.ipType = connProfile.ipList[0].sourceType;
    connProfile.ip = connProfile.ipList[0].ip;
    connProfile.port = connProfile.ipList[0].port;
    connProfile.nat64 = isnat64;
    connProfile.dnsEndtime = ::gettickcount();
    getCurrNetLabel(connProfile.netType);
    __UpdateProfile(connProfile);

    // set the first ip info to the profiler, after connect, the ip info will be overwrriten by the real one

    ShortLinkConnectObserver connect_observer(*this);
    ComplexConnect conn(kShortlinkConnTimeout, kShortlinkConnInterval);

    SOCKET sock = conn.ConnectImpatient(vecaddr, mBreaker, &connect_observer, connProfile.proxyInfo.type, proxy_addr,
                                        connProfile.proxyInfo.username, connProfile.proxyInfo.password);
    delete proxy_addr;

    connProfile.connRtt = conn.IndexRtt();
    connProfile.ipIndex = conn.Index();
    connProfile.connCost = conn.TotalCost();

    __UpdateProfile(connProfile);

    WeakNetworkLogic::Singleton::Instance()->OnConnectEvent(sock != INVALID_SOCKET, conn.IndexRtt(), conn.Index());

    if (INVALID_SOCKET == sock) {
        xwarn2(TSF"task socket connect fail sock %_, net:%_", message.String(), getNetInfo());
        connProfile.connErrcode = conn.ErrorCode();

        if (!mBreaker.IsBreak()) {
            __RunResponseError(kEctSocket, kEctSocketMakeSocketPrepared, connProfile, false);
        } else {
            connProfile.disconnErrType = kEctCanceld;
            __UpdateProfile(connProfile);
        }

        return INVALID_SOCKET;
    }

    xassert2(0 <= conn.Index() && (unsigned int) conn.Index() < connProfile.ipList.size());

    for (int i = 0; i < conn.Index(); ++i) {
        if (1 == connect_observer.ConnectingIndex[i] && OnNetReport)
            OnNetReport(__LINE__, kEctSocket, SOCKET_ERRNO(ETIMEDOUT), connProfile.ipList[i].ip,
                                connProfile.ipList[i].host, connProfile.ipList[i].port);
    }

    connProfile.host = connProfile.ipList[conn.Index()].host;
    connProfile.ipType = connProfile.ipList[conn.Index()].sourceType;
    connProfile.ip = connProfile.ipList[conn.Index()].ip;
    connProfile.connTime = gettickcount();
    connProfile.localIp = socket_address::getsockname(sock).ip();
    connProfile.localPort = socket_address::getsockname(sock).port();
    __UpdateProfile(connProfile);

    xinfo2(TSF"task socket connect success sock:%_, %_ host:%_, ip:%_, port:%_, local_ip:%_, local_port:%_, iptype:%_, net:%_",
           sock, message.String(), connProfile.host, connProfile.ip, connProfile.port, connProfile.localIp,
           connProfile.localPort, IPSourceTypeString[connProfile.ipType], connProfile.netType);


//    struct linger so_linger;
//    so_linger.l_onoff = 1;
//    so_linger.l_linger = 0;

//    xerror2_if(0 != setsockopt(sock, SOL_SOCKET, SO_LINGER, (const char*)&so_linger, sizeof(so_linger)), TSF"SO_LINGER %_(%_)", socket_errno, socket_strerror(socket_errno));
    return sock;
}

void ShortLink::__RunReadWrite(SOCKET socket, int &errType, int &errCode, ConnectProfile &connProfile) {
    xmessage2_define(message)(TSF "taskId:%_, cgi:%_, @%_", mTask.taskId, mTask.cgi, this);

    std::string url;
    std::map<std::string, std::string> headers;
#ifdef WIN32
    std::string replace_host = connProfile.host;
    if (kIPSourceProxy == connProfile.ip_type) {
        url += "http://";
        url += connProfile.host;
    } else {
        replace_host = connProfile.ip.empty() ? connProfile.host : connProfile.ip;
    }
    url += task_.cgi;

    headers[http::HeaderFields::KStringHost] = replace_host;
    headers["X-Online-Host"] = replace_host;
#else
    if (kIPSourceProxy == connProfile.ipType) {
        url += "http://";
        url += connProfile.host;
    }
    url += mTask.cgi;

    headers[http::HeaderFields::KStringHost] = connProfile.host;
#endif // WIN32

    if (connProfile.proxyInfo.IsValid() && mars::comm::kProxyHttp == connProfile.proxyInfo.type
        && !connProfile.proxyInfo.username.empty() && !connProfile.proxyInfo.password.empty()) {
        std::string account_info = connProfile.proxyInfo.username + ":" + connProfile.proxyInfo.password;
        size_t dstlen = modp_b64_encode_len(account_info.length());

        char *dstbuf = (char *) malloc(dstlen);
        memset(dstbuf, 0, dstlen);

        int retsize = Comm::EncodeBase64((unsigned char *) account_info.c_str(), (unsigned char *) dstbuf,
                                         (int) account_info.length());
        dstbuf[retsize] = '\0';

        char auth_info[1024] = {0};
        snprintf(auth_info, sizeof(auth_info), "Basic %s", dstbuf);
        headers[http::HeaderFields::kStringProxyAuthorization] = auth_info;
        free(dstbuf);
    }

    AutoBuffer out_buff;

    shortlink_pack(url, headers, mSendBody, mSendExtend, out_buff, mTracker.get());

    // send request
    xgroup2_define(group_send);
    xinfo2(TSF"task socket send sock:%_, %_ http len:%_, ", socket, message.String(), out_buff.Length())
        >> group_send;

    int send_ret = block_socket_send(socket, (const unsigned char *) out_buff.Ptr(), (unsigned int) out_buff.Length(),
                                     mBreaker, errCode);

    if (send_ret < 0) {
        xerror2(TSF"Send Request Error, ret:%0, errno:%1, nread:%_, nwrite:%_", send_ret, strerror(errCode),
                socket_nread(socket), socket_nwrite(socket)) >> group_send;
        __RunResponseError(kEctSocket, (errCode == 0) ? kEctSocketWritenWithNonBlock : errCode, connProfile,
                           true);
        return;
    }

    GetSignalOnNetworkDataChange()(XLOGGER_TAG, send_ret, 0);

    if (mBreaker.IsBreak()) {
        xwarn2(TSF"Send Request break, sent:%_ nread:%_, nwrite:%_", send_ret, socket_nread(socket),
               socket_nwrite(socket)) >> group_send;
        return;
    }

    xgroup2() << group_send;

    xgroup2_define(group_close);
    xgroup2_define(group_recv);

    xinfo2(TSF"task socket close sock:%_, %_, ", socket, message.String()) >> group_close;
    xinfo2(TSF"task socket recv sock:%_,  %_, ", socket, message.String()) >> group_recv;

    //recv response
    AutoBuffer body;
    AutoBuffer recv_buf;
    AutoBuffer extension;
    int status_code = -1;
    off_t recv_pos = 0;
    MemoryBodyReceiver *receiver = new MemoryBodyReceiver(body);
    http::Parser parser(receiver, true);

    while (true) {
        int recv_ret = block_socket_recv(socket, recv_buf, KBufferSize, mBreaker, errCode, 5000);

        if (recv_ret < 0) {
            xerror2(TSF"read block socket return false, error:%0, nread:%_, nwrite:%_", strerror(errCode),
                    socket_nread(socket), socket_nwrite(socket)) >> group_close;
            __RunResponseError(kEctSocket, (errCode == 0) ? kEctSocketReadOnce : errCode, connProfile, true);
            break;
        }

        if (mBreaker.IsBreak()) {
            xinfo2(TSF"user cancel, nread:%_, nwrite:%_", socket_nread(socket), socket_nwrite(socket))
                >> group_close;
            connProfile.disconnErrType = kEctCanceld;
            break;
        }

        if (recv_ret == 0 && SOCKET_ERRNO(ETIMEDOUT) == errCode) {
            xerror2(TSF"read timeout error:(%_,%_), nread:%_, nwrite:%_ ", errCode, strerror(errCode),
                    socket_nread(socket), socket_nwrite(socket)) >> group_close;
            continue;
        }
        if (recv_ret == 0) {
            xerror2(TSF"remote disconnect, nread:%_, nwrite:%_", errCode, strerror(errCode),
                    socket_nread(socket), socket_nwrite(socket)) >> group_close;
            __RunResponseError(kEctSocket, kEctSocketShutdown, connProfile, true);
            break;
        }

        if (recv_ret > 0) {
            GetSignalOnNetworkDataChange()(XLOGGER_TAG, 0, recv_ret);

            xinfo2(TSF"recv len:%_ ", recv_ret) >> group_recv;
            if (OnRecv)
                OnRecv(this, (unsigned int) (recv_buf.Length() - recv_pos), (unsigned int) recv_buf.Length());
            else xwarn2(TSF"OnRecv NULL.");
            recv_pos = recv_buf.Pos();
        }

        Parser::TRecvStatus parse_status = parser.Recv(recv_buf.Ptr(recv_buf.Length() - recv_ret), recv_ret);
        if (parser.FirstLineReady()) {
            status_code = parser.Status().StatusCode();
        }

        if (parse_status == http::Parser::kFirstLineError) {
            xerror2(TSF"http head not receive yet,but socket closed, length:%0, nread:%_, nwrite:%_ ",
                    recv_buf.Length(), socket_nread(socket), socket_nwrite(socket)) >> group_close;
            __RunResponseError(kEctHttp, kEctHttpParseStatusLine, connProfile, true);
            break;
        } else if (parse_status == http::Parser::kHeaderFieldsError) {
            xerror2(TSF"parse http head failed, but socket closed, length:%0, nread:%_, nwrite:%_ ",
                    recv_buf.Length(), socket_nread(socket), socket_nwrite(socket)) >> group_close;
            __RunResponseError(kEctHttp, kEctHttpSplitHttpHeadAndBody, connProfile, true);
            break;
        } else if (parse_status == http::Parser::kBodyError) {
            xerror2(TSF"content_length_ != body.Lenght(), Head:%0, http dump:%1 \n headers size:%2",
                    parser.Fields().ContentLength(), xdump(recv_buf.Ptr(), recv_buf.Length()),
                    parser.Fields().GetHeaders().size()) >> group_close;
            __RunResponseError(kEctHttp, kEctHttpSplitHttpHeadAndBody, connProfile, true);
            break;
        } else if (parse_status == http::Parser::kEnd) {
            if (status_code != 200) {
                xerror2(TSF"@%0, status_code != 200, code:%1, http dump:%2 \n headers size:%3", this, status_code,
                        xdump(recv_buf.Ptr(), recv_buf.Length()), parser.Fields().GetHeaders().size()) >> group_close;
                __RunResponseError(kEctHttp, status_code, connProfile, true);
            } else {
                xinfo2(TSF"@%0, headers size:%_, ", this, parser.Fields().GetHeaders().size()) >> group_recv;
                __OnResponse(kEctOK, status_code, body, extension, connProfile, true);
            }
            break;
        } else {
            xdebug2(TSF"http parser status:%_ ", parse_status);
        }
    }

    xdebug2(TSF"read with nonblock socket http response, length:%_, ", recv_buf.Length()) >> group_recv;

    xgroup2() << group_recv;
#if defined(__ANDROID__) || defined(__APPLE__)
    struct tcp_info _info;
    if (getsocktcpinfo(socket, &_info) == 0) {
        char tcp_info_str[1024] = {0};
        xinfo2(TSF"task socket close getsocktcpinfo:%_", tcpinfo2str(&_info, tcp_info_str, sizeof(tcp_info_str)))
            >> group_close;
    }
#endif
    xgroup2() << group_close;
}

void ShortLink::__UpdateProfile(const ConnectProfile &connProfile) {
    STATIC_RETURN_SYNC2ASYNC_FUNC(boost::bind(&ShortLink::__UpdateProfile, this, connProfile));
    mConnProfile = connProfile;
}

void ShortLink::__RunResponseError(ErrCmdType type, int errCode, ConnectProfile &connProfile, bool report) {
    AutoBuffer buf;
    AutoBuffer extension;
    __OnResponse(type, errCode, buf, extension, connProfile, report);
}

void ShortLink::__OnResponse(ErrCmdType _errType, int status, AutoBuffer &bufBody, AutoBuffer &bufExt,
                             ConnectProfile &connProfile, bool report) {
    connProfile.disconnErrType = _errType;
    connProfile.disconnErrCode = status;
    __UpdateProfile(connProfile);

    //   xassert2(!mBreaker.IsBreak());

    if (kEctOK != _errType) {
//        xassert2(OnNetReport);

        if (report && OnNetReport)
            OnNetReport(__LINE__, _errType, status, connProfile.ip, connProfile.host, connProfile.port);
    }

    if (OnResponse) {
        move_wrapper<AutoBuffer> body(bufBody);
        move_wrapper<AutoBuffer> extension(bufExt);
        OnResponse(this, _errType, status, body, extension, false, connProfile);
    } else xwarn2(TSF"OnResponse NULL.");
}

void ShortLink::FillOutterIPAddr(const std::vector<IPPortItem> &outAddrs) {
    if (!outAddrs.empty()) {
        mOutterAddrs = outAddrs;
    }
}

void ShortLink::__CancelAndWaitWorkerThread() {
    xdebug_function();

    if (!mThread.isRunning()) return;

    xassert2(mBreaker.IsCreateSuc());

    if (!mBreaker.Break()) {
        xassert2(false, "breaker fail");
        mBreaker.Close();
    }

    mDnsUtil.Cancel();
    mThread.join();
}
