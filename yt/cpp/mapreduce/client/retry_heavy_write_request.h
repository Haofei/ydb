#pragma once

#include <yt/cpp/mapreduce/client/transaction.h>

#include <yt/cpp/mapreduce/common/fwd.h>

#include <yt/cpp/mapreduce/http/context.h>
#include <yt/cpp/mapreduce/http/requests.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

class THeavyRequestRetrier
{
public:
    struct TParameters
    {
        IRawClientPtr RawClientPtr;
        IClientRetryPolicyPtr ClientRetryPolicy;
        ITransactionPingerPtr TransactionPinger;
        TClientContext Context;
        TTransactionId TransactionId;
        THttpHeader Header;
    };

    using TStreamFactory = std::function<std::unique_ptr<IInputStream>()>;

public:
    explicit THeavyRequestRetrier(TParameters parameters);
    ~THeavyRequestRetrier();

    void Update(TStreamFactory streamFactory);
    void Finish();

private:
    void Retry(const std::function<void()>& function);

    void TryStartAttempt();

private:
    const TParameters Parameters_;
    const IRequestRetryPolicyPtr RequestRetryPolicy_;

    struct TAttempt
    {
        std::unique_ptr<TPingableTransaction> Transaction;
        TString RequestId;
        NHttpClient::IHttpRequestPtr Request;
        ssize_t Offset = 0;
    };
    std::unique_ptr<TAttempt> Attempt_;

    TStreamFactory StreamFactory_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
