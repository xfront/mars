/*
 * Tencent is pleased to support the open source community by making Mars available.
 * Copyright (C) 2016 THL A29 Limited, a Tencent company. All rights reserved.
 *
 * Licensed under the MIT License (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://opensource.org/licenses/MIT
 *
 * Unless required by applicable law or agreed to in writing, software distributed under the License is
 * distributed on an "AS IS" basis, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied. See the License for the specific language governing permissions and
 * limitations under the License.
 */

package com.tencent.mars.sample.wrapper.remote;

import android.app.Service;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.IBinder;
import android.os.Looper;
import android.os.RemoteException;

import com.tencent.mars.app.AppLogic;
import com.tencent.mars.xlog.Log;

import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.LinkedBlockingQueue;

/**
 * Mars Service Proxy for component callers
 * <p></p>
 * Created by zhaoyuan on 16/2/26.
 */
public class MarsServiceProxy implements ServiceConnection {
    private static class SingletonClassHolder {
        static final MarsServiceProxy SINGLE_INSTANCE = new MarsServiceProxy();
    }

    public static MarsServiceProxy instance() {
        return SingletonClassHolder.SINGLE_INSTANCE;
    }

    private static final String TAG = "Mars.Sample.MarsServiceProxy";
    public static final String SERVICE_DEFAULT_CLASSNAME = "com.tencent.mars.sample.wrapper.service.MarsServiceNative";

    private Context mContext;
    private String mPackageName;
    private String mClassName;
    private MarsService mService = null;
    private Worker mWorker;
    public AppLogic.AccountInfo mAccountInfo;
    private LinkedBlockingQueue<MarsTaskWrapper> mTaskQueue = new LinkedBlockingQueue<>();
    private ConcurrentHashMap<String, Integer> mPath2IdMap = new ConcurrentHashMap<>();
    private Map<Integer, PushMessageHandler> mPushMsgHandlerMap = new ConcurrentHashMap<>();

    private MarsPushMessageFilter mPushMsgFilter = new MarsPushMessageFilter.Stub() {
        @Override
        public boolean onRecv(int cmdId, byte[] buffer) throws RemoteException {
            PushMessageHandler handler = mPushMsgHandlerMap.get(cmdId);
            if (handler != null) {
                Log.i(TAG, "processing push message, cmdid = %d", cmdId);
                PushMessage message = new PushMessage(cmdId, buffer);
                handler.process(message);
                return true;

            } else {
                Log.i(TAG, "no push message listener set for cmdid = %d, just ignored", cmdId);
            }

            return false;
        }
    };

    private MarsServiceProxy() {
        mWorker = new Worker();
        mWorker.start();
    }

    public void init(Context context, Looper looper, String packageName) {
        mContext = context.getApplicationContext();
        mPackageName = (packageName == null ? context.getPackageName() : packageName);
        mClassName = SERVICE_DEFAULT_CLASSNAME;
    }

    public void setOnPushMessageListener(int cmdId, PushMessageHandler handler) {
        if (handler == null) {
            mPushMsgHandlerMap.remove(cmdId);
        } else {
            mPushMsgHandlerMap.put(cmdId, handler);
        }
    }

    public void send(MarsTaskWrapper taskWrapper) {
        mTaskQueue.offer(taskWrapper);
    }

    public void cancel(MarsTaskWrapper taskWrapper) {
        cancelSpecifiedTaskWrapper(taskWrapper);
    }

    public void setForeground(boolean isForeground) {
        try {
            if (mService == null) {
                Log.d(TAG, "try to bind remote mars service, packageName: %s, className: %s", mPackageName, mClassName);
                Intent i = new Intent().setClassName(mPackageName, mClassName);
                mContext.startService(i);
                if (!mContext.bindService(i, instance(), Service.BIND_AUTO_CREATE)) {
                    Log.e(TAG, "remote mars service bind failed");
                }

                return;
            }
            mService.setForeground(isForeground ? 1 : 0);
        } catch (RemoteException e) {
            e.printStackTrace();
        }
    }

    @Override
    public void onServiceConnected(ComponentName componentName, IBinder binder) {
        Log.d(TAG, "remote mars service connected");

        try {
            mService = MarsService.Stub.asInterface(binder);
            mService.registerPushMessageFilter(mPushMsgFilter);
            mService.setAccountInfo(mAccountInfo.uin, mAccountInfo.userName);

        } catch (Exception e) {
            mService = null;
        }
    }

    @Override
    public void onServiceDisconnected(ComponentName componentName) {
        try {
            mService.unregisterPushMessageFilter(mPushMsgFilter);
        } catch (RemoteException e) {
            e.printStackTrace();
        }
        mService = null;

        // TODO: need reconnect ?
        Log.d(TAG, "remote mars service disconnected");
    }

    private void cancelSpecifiedTaskWrapper(MarsTaskWrapper marsTaskWrapper) {
        if (mTaskQueue.remove(marsTaskWrapper)) {
            // Remove from queue, not exec yet, call MarsTaskWrapper::onTaskEnd
            try {
                marsTaskWrapper.onTaskEnd(-1, -1);

            } catch (RemoteException e) {
                // Called in client, ignore RemoteException
                e.printStackTrace();
                Log.e(TAG, "cancel mars task wrapper in client, should not catch RemoteException");
            }

        } else {
            // Already sent to remote service, need to cancel it
            try {
                mService.cancel(marsTaskWrapper.getProperties().getInt(MarsTaskProperty.OPTIONS_TASK_ID));
            } catch (RemoteException e) {
                e.printStackTrace();
                Log.w(TAG, "cancel mars task wrapper in remote service failed, I'll make marsTaskWrapper.onTaskEnd");
            }
        }
    }

    private void continueProcessTaskWrappers() {
        try {
            if (mService == null) {
                Log.d(TAG, "try to bind remote mars service, packageName: %s, className: %s", mPackageName, mClassName);
                Intent i = new Intent().setClassName(mPackageName, mClassName);
                mContext.startService(i);
                if (!mContext.bindService(i, instance(), Service.BIND_AUTO_CREATE)) {
                    Log.e(TAG, "remote mars service bind failed");
                }

                // Waiting for service connected
                return;
            }

            MarsTaskWrapper taskWrapper = mTaskQueue.take();
            if (taskWrapper == null) {
                // Stop, no more task
                return;
            }

            try {
                Log.d(TAG, "sending task = %s", taskWrapper);
                final String cgiPath = taskWrapper.getProperties().getString(MarsTaskProperty.OPTIONS_CGI_PATH);
                final Integer globalCmdID = mPath2IdMap.get(cgiPath);
                if (globalCmdID != null) {
                    taskWrapper.getProperties().putInt(MarsTaskProperty.OPTIONS_CMD_ID, globalCmdID);
                    Log.i(TAG, "overwrite cmdID with global cmdID Map: %s -> %d", cgiPath, globalCmdID);
                }

                final int taskID = mService.send(taskWrapper, taskWrapper.getProperties());
                // NOTE: Save taskID to taskWrapper here
                taskWrapper.getProperties().putInt(MarsTaskProperty.OPTIONS_TASK_ID, taskID);

            } catch (Exception e) { // RemoteExceptionHandler
                e.printStackTrace();
            }

        } catch (Exception e) {
            //
        }
    }

    private class Worker extends Thread {
        @Override
        public void run() {
            while (true) {
                continueProcessTaskWrappers();

                try {
                    Thread.sleep(50);
                } catch (InterruptedException e) {
                    //
                }
            }
        }
    }
}
