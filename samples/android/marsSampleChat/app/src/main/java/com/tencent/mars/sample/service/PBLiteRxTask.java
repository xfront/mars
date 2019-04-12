package com.tencent.mars.sample.service;

import com.google.protobuf.MessageLite;
import com.tencent.mars.sample.wrapper.remote.MarsServiceProxy;
import com.tencent.mars.sample.wrapper.remote.PBLiteMarsTaskWrapper;

import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;

import io.reactivex.Observable;
import io.reactivex.subjects.PublishSubject;

public class PBLiteRxTask<T extends MessageLite.Builder, R extends MessageLite.Builder> extends PBLiteMarsTaskWrapper<T, R> {
    private PublishSubject<R> mEmitter = PublishSubject.create();

    public PBLiteRxTask(T request, R resp) {
        super(request, resp);
    }

    public Observable<R> observe() {
        return mEmitter;
    }

    public Observable<R> observe(long timeout) {
        return mEmitter.timeout(timeout, TimeUnit.MILLISECONDS, observer -> {
            MarsServiceProxy.instance().cancel(this);
            observer.onError(new TimeoutException());
        });
    }

    @Override
    public void onPreEncode(T req) {
    }

    @Override
    public void onPostDecode(R response) {

    }

    @Override
    public void onTaskEnd(int errType, int errCode) {
        if (errCode == 0) {
            mEmitter.onNext(mResponse);
            mEmitter.onComplete();
        } else {
            mEmitter.onError(new Exception(String.format("errType=%d, errCode=%d", errType, errCode)));
        }
    }

}
