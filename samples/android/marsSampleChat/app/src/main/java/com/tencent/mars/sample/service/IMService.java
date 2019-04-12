package com.tencent.mars.sample.service;

import com.tencent.mars.sample.Conversation;
import com.tencent.mars.sample.SampleApplicaton;
import com.tencent.mars.sample.chat.proto.Chat;
import com.tencent.mars.sample.proto.Main;
import com.tencent.mars.sample.wrapper.remote.MarsServiceProxy;

import java.util.LinkedList;
import java.util.List;

import io.reactivex.Observable;

public class IMService {
    final static String CONVERSATION_HOST = "marsopen.cn"; // using preset ports
    final static long DEFAULT_REQ_TIMEOUT = 30000;

    public static Observable<List<Conversation>> getConvList(final int conversationFilterType) {
        //1.将参数set到PB的Builder类里
        Main.ConversationListRequest.Builder req = Main.ConversationListRequest.newBuilder();
        req.setAccessToken("");
        req.setType(conversationFilterType);

        //2.用PBLiteRxTask封装请求响应体
        PBLiteRxTask<Main.ConversationListRequest.Builder, Main.ConversationListResponse.Builder> rxTask = new PBLiteRxTask(req, Main.ConversationListResponse.newBuilder());
        rxTask.setHttpRequest(CONVERSATION_HOST, "/mars/getconvlist");//设Task其它属性

        //3.发送请求
        MarsServiceProxy.instance().send(rxTask);

        //4.提取结果到最后实用类型(而不仅是PB)
        return rxTask.observe(DEFAULT_REQ_TIMEOUT)
                .map(rsp -> {
                    List<Conversation> dataList = new LinkedList<>();
                    for (Main.Conversation conv : rsp.getListList()) {
                        dataList.add(new Conversation(conv.getName(), conv.getTopic(), conv.getNotice()));
                    }
                    return dataList;
                });
    }

    public static Observable<Chat.SendMessageResponse> sendTextMessage(String topicName, String message) {
        //1.将参数set到PB的Builder类里
        Chat.SendMessageRequest.Builder req = Chat.SendMessageRequest.newBuilder();
        req.setAccessToken("test_token");
        req.setFrom(SampleApplicaton.accountInfo.userName);
        req.setTo("all");
        req.setText(message);
        req.setTopic(topicName);

        //2.用PBLiteRxTask封装请求响应体
        PBLiteRxTask<Chat.SendMessageRequest.Builder, Chat.SendMessageResponse.Builder> rxTask = new PBLiteRxTask(req, Chat.SendMessageResponse.newBuilder());
        rxTask.setHttpRequest(CONVERSATION_HOST, "/mars/sendmessage");//设Task其它属性
        rxTask.setCmdID(Main.CmdID.CMD_ID_SEND_MESSAGE_VALUE);
        rxTask.setLongChannelSupport(true);
        rxTask.setShortChannelSupport(false);

        //3.发送请求
        MarsServiceProxy.instance().send(rxTask);

        //4.提取结果到最后实用类型(而不仅是PB)
        return rxTask.observe(DEFAULT_REQ_TIMEOUT)
                .map(rsp -> rsp.build());
    }

    public static Observable<Chat.SendMessageResponse> sendTextMessageOld(String topicName, String message) {
        TextMessageTask txtMsg = new TextMessageTask(topicName, message);
        MarsServiceProxy.instance().send(txtMsg);
        return txtMsg.observe();
    }

}
