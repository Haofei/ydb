#include "yql_yt_qplayer_gateway.h"

#include <yql/essentials/core/file_storage/storage.h>
#include <library/cpp/yson/node/node_io.h>
#include <library/cpp/yson/node/node_builder.h>
#include <yt/cpp/mapreduce/interface/serialize.h>

#include <util/stream/file.h>

#include <openssl/sha.h>

namespace NYT {
    //TODO: use from header
    void Deserialize(TReadRange& readRange, const TNode& node);
}

namespace NYql {

namespace {

const TString YtGateway_CanonizePaths = "YtGateway_CanonizePaths";
const TString YtGateway_GetTableInfo = "YtGateway_GetTableInfo";
const TString YtGateway_GetTableRange = "YtGateway_GetTableRange";
const TString YtGateway_GetFolder = "YtGateway_GetFolder";
const TString YtGateway_GetFolders = "YtGateway_GetFolders";
const TString YtGateway_ResolveLinks = "YtGateway_ResolveLinks";
const TString YtGateway_PathStat = "YtGateway_PathStat";
const TString YtGateway_PathStatMissing = "YtGateway_PathStatMissing";

TString MakeHash(const TString& str) {
    SHA256_CTX sha;
    SHA256_Init(&sha);
    SHA256_Update(&sha, str.data(), str.size());
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_Final(hash, &sha);
    return TString((const char*)hash, sizeof(hash));
}

class TGateway : public IYtGateway {
public:
    TGateway(IYtGateway::TPtr inner, const TQContext& qContext,
        const TIntrusivePtr<IRandomProvider>& randomProvider,
        const TFileStoragePtr& fileStorage)
        : Inner_(inner)
        , QContext_(qContext)
        , RandomProvider_(randomProvider)
        , FileStorage_(fileStorage)
    {}

    void OpenSession(TOpenSessionOptions&& options) final {
        SessionGenerations_[options.SessionId()] = 0;
        return Inner_->OpenSession(std::move(options));
    }

    NThreading::TFuture<void> CloseSession(TCloseSessionOptions&& options) final {
        return Inner_->CloseSession(std::move(options));
    }

    NThreading::TFuture<void> CleanupSession(TCleanupSessionOptions&& options) final {
        ++SessionGenerations_[options.SessionId()];
        return Inner_->CleanupSession(std::move(options));
    }

    NThreading::TFuture<TFinalizeResult> Finalize(TFinalizeOptions&& options) final {
        if (QContext_.CanRead()) {
            TFinalizeResult res;
            res.SetSuccess();
            return NThreading::MakeFuture<TFinalizeResult>(res);
        }

        return Inner_->Finalize(std::move(options));
    }

    static TString MakeCanonizePathKey(const TCanonizeReq& req) {
        auto node = NYT::TNode()
            ("Cluster", req.Cluster())
            ("Path", req.Path());

        return MakeHash(NYT::NodeToCanonicalYsonString(node));
    }

    NThreading::TFuture<TCanonizePathsResult> CanonizePaths(TCanonizePathsOptions&& options) final {
        if (QContext_.CanRead()) {
            TCanonizePathsResult res;
            res.SetSuccess();
            for (const auto& req : options.Paths()) {
                auto key = MakeCanonizePathKey(req);
                auto item = QContext_.GetReader()->Get({YtGateway_CanonizePaths, key}).GetValueSync();
                if (!item) {
                    throw yexception() << "Missing replay data";
                }

                auto valueNode = NYT::NodeFromYsonString(item->Value);
                TCanonizedPath p;
                p.Path = valueNode["Path"].AsString();
                if (valueNode.HasKey("Columns")) {
                    p.Columns.ConstructInPlace();
                    for (const auto& c : valueNode["Columns"].AsList()) {
                        p.Columns->push_back(c.AsString());
                    }
                }

                if (valueNode.HasKey("Ranges")) {
                    p.Ranges.ConstructInPlace();
                    for (const auto& r : valueNode["Ranges"].AsList()) {
                        NYT::TReadRange range;
                        NYT::Deserialize(range, r);
                        p.Ranges->push_back(range);
                    }
                }

                if (valueNode.HasKey("AdditionalAttributes")) {
                    p.AdditionalAttributes = valueNode["AdditionalAttributes"].AsString();
                }

                res.Data.push_back(p);
            }

            return NThreading::MakeFuture<TCanonizePathsResult>(res);
        }

        auto optionsDup = options;
        return Inner_->CanonizePaths(std::move(options))
            .Subscribe([qContext = QContext_, optionsDup](const NThreading::TFuture<TCanonizePathsResult>& future) {
                if (!qContext.CanWrite() || future.HasException()) {
                    return;
                }

                const auto& res = future.GetValueSync();
                if (!res.Success()) {
                    return;
                }

                Y_ENSURE(res.Data.size() == optionsDup.Paths().size());
                for (size_t i = 0; i < res.Data.size(); ++i) {
                    auto key = MakeCanonizePathKey(optionsDup.Paths()[i]);
                    auto valueNode = NYT::TNode();
                    const auto& canon = res.Data[i];
                    valueNode("Path", canon.Path);
                    if (canon.Columns) {
                        NYT::TNode columnsNode = NYT::TNode::CreateList();
                        for (const auto& c : *canon.Columns) {
                            columnsNode.Add(NYT::TNode(c));
                        }

                        valueNode("Columns", columnsNode);
                    }

                    if (canon.Ranges) {
                        NYT::TNode rangesNode = NYT::TNode::CreateList();
                        for (const auto& r : *canon.Ranges) {
                            NYT::TNode rangeNode;
                            NYT::TNodeBuilder builder(&rangeNode);
                            NYT::Serialize(r, &builder);
                            rangesNode.Add(rangeNode);
                        }

                        valueNode("Ranges", rangesNode);
                    }

                    if (canon.AdditionalAttributes) {
                        valueNode("AdditionalAttributes", NYT::TNode(*canon.AdditionalAttributes));
                    }

                    auto value = NYT::NodeToYsonString(valueNode, NYT::NYson::EYsonFormat::Binary);
                    qContext.GetWriter()->Put({YtGateway_CanonizePaths, key}, value).GetValueSync();
                }
            });
    }

    static TString MakeGetTableInfoKey(const TTableReq& req, ui32 epoch, ui64 generation) {
        auto tableNode = NYT::TNode()
            ("Generation", generation)
            ("Cluster", req.Cluster())
            ("Table", req.Table());

        if (req.InferSchemaRows() != 0) {
            tableNode("InferSchemaRows", req.InferSchemaRows());
        }

        if (req.ForceInferSchema()) {
            tableNode("ForceInferSchema", req.ForceInferSchema());
        }

        if (req.Anonymous()) {
            tableNode("Anonymous", req.Anonymous());
        }

        if (req.IgnoreYamrDsv()) {
            tableNode("IgnoreYamrDsv", req.IgnoreYamrDsv());
        }

        if (req.IgnoreWeakSchema()) {
            tableNode("IgnoreWeakSchema", req.IgnoreWeakSchema());
        }

        auto node = NYT::TNode()
            ("Table", tableNode)
            ("Epoch", epoch);

        return MakeHash(NYT::NodeToCanonicalYsonString(node));
    }

    NThreading::TFuture<TTableInfoResult> GetTableInfo(TGetTableInfoOptions&& options) final {
        ui64 generation = SessionGenerations_[options.SessionId()];
        if (QContext_.CanRead()) {
            TTableInfoResult res;
            res.SetSuccess();
            for (const auto& req : options.Tables()) {
                TTableInfoResult::TTableData data;
                auto key = MakeGetTableInfoKey(req, options.Epoch(), generation);
                auto item = QContext_.GetReader()->Get({YtGateway_GetTableInfo, key}).GetValueSync();
                if (!item) {
                    throw yexception() << "Missing replay data";
                }

                auto valueNode = NYT::NodeFromYsonString(item->Value);
                if (valueNode.HasKey("Meta")) {
                    data.Meta = MakeIntrusive<TYtTableMetaInfo>();
                    auto metaNode = valueNode["Meta"];

                    data.Meta->CanWrite = metaNode["CanWrite"].AsBool();
                    data.Meta->DoesExist = metaNode["DoesExist"].AsBool();
                    data.Meta->YqlCompatibleScheme = metaNode["YqlCompatibleScheme"].AsBool();
                    data.Meta->InferredScheme = metaNode["InferredScheme"].AsBool();
                    data.Meta->IsDynamic = metaNode["IsDynamic"].AsBool();
                    data.Meta->SqlView = metaNode["SqlView"].AsString();
                    data.Meta->SqlViewSyntaxVersion = metaNode["SqlViewSyntaxVersion"].AsUint64();
                    for (const auto& x : metaNode["Attrs"].AsMap()) {
                        data.Meta->Attrs[x.first] = x.second.AsString();
                    }
                }
                if (valueNode.HasKey("Stat")) {
                    data.Stat = MakeIntrusive<TYtTableStatInfo>();
                    auto statNode = valueNode["Stat"];
                    data.Stat->Id = statNode["Id"].AsString();
                    data.Stat->RecordsCount = statNode["RecordsCount"].AsUint64();
                    data.Stat->DataSize = statNode["DataSize"].AsUint64();
                    data.Stat->ChunkCount = statNode["ChunkCount"].AsUint64();
                    data.Stat->ModifyTime = statNode["ModifyTime"].AsUint64();
                    data.Stat->Revision = statNode["Revision"].AsUint64();
                    data.Stat->TableRevision = statNode["TableRevision"].AsUint64();
                    if (statNode.HasKey("SecurityTags")) {
                        data.Stat->SecurityTags.clear();
                        for (auto &e: statNode["SecurityTags"].AsList()) {
                            data.Stat->SecurityTags.emplace(e.AsString());
                        }
                    }
                }
                data.WriteLock = valueNode["WriteLock"].AsBool();
                res.Data.push_back(data);
            }

            return NThreading::MakeFuture<TTableInfoResult>(res);
        }

        auto optionsDup = options;
        return Inner_->GetTableInfo(std::move(options))
            .Subscribe([optionsDup, qContext = QContext_, generation](const NThreading::TFuture<TTableInfoResult>& future) {
                if (!qContext.CanWrite() || future.HasException()) {
                    return;
                }

                const auto& res = future.GetValueSync();
                if (!res.Success()) {
                    return;
                }

                Y_ENSURE(res.Data.size() == optionsDup.Tables().size());
                for (size_t i = 0; i < res.Data.size(); ++i) {
                    const auto& req = optionsDup.Tables()[i];
                    const auto& data = res.Data[i];
                    auto key = MakeGetTableInfoKey(req, optionsDup.Epoch(), generation);

                    auto attrsNode = NYT::TNode::CreateMap();
                    if (data.Meta) {
                        for (const auto& a : data.Meta->Attrs)  {
                            attrsNode(a.first, a.second);
                        }
                    }

                    auto metaNode = data.Meta ? NYT::TNode()
                        ("CanWrite",data.Meta->CanWrite)
                        ("DoesExist",data.Meta->DoesExist)
                        ("YqlCompatibleScheme",data.Meta->YqlCompatibleScheme)
                        ("InferredScheme",data.Meta->InferredScheme)
                        ("IsDynamic",data.Meta->IsDynamic)
                        ("SqlView",data.Meta->SqlView)
                        ("SqlViewSyntaxVersion",ui64(data.Meta->SqlViewSyntaxVersion))
                        ("Attrs",attrsNode) : NYT::TNode();


                    NYT::TNode securityTags = NYT::TNode::CreateList();
                    if (data.Stat) {
                        for (const auto& c : data.Stat->SecurityTags) {
                            securityTags.Add(NYT::TNode(c));
                        }
                    }

                    auto statNode = data.Stat ? NYT::TNode()
                        ("Id", data.Stat->Id)
                        ("RecordsCount", data.Stat->RecordsCount)
                        ("DataSize", data.Stat->DataSize)
                        ("ChunkCount", data.Stat->ChunkCount)
                        ("ModifyTime", data.Stat->ModifyTime)
                        ("Revision", data.Stat->Revision)
                        ("TableRevision", data.Stat->TableRevision)
                        ("SecurityTags", securityTags) : NYT::TNode();

                    auto valueNode = NYT::TNode::CreateMap();
                    if (data.Meta) {
                        valueNode("Meta", metaNode);
                    }
                    if (data.Stat) {
                        valueNode("Stat", statNode);
                    }
                    valueNode("WriteLock", data.WriteLock);

                    auto value = NYT::NodeToYsonString(valueNode, NYT::NYson::EYsonFormat::Binary);
                    qContext.GetWriter()->Put({YtGateway_GetTableInfo, key},value).GetValueSync();
                }
            });
    }

    static TString MakeGetTableRangeKey(const TTableRangeOptions& options, ui64 generation) {
        auto keyNode = NYT::TNode()
            ("Generation", generation)
            ("Cluster", options.Cluster())
            ("Prefix", options.Prefix())
            ("Suffix", options.Suffix());

        if (options.Filter()) {
            keyNode("Filter", MakeCacheKey(*options.Filter()));
        }

        return MakeHash(NYT::NodeToCanonicalYsonString(keyNode, NYT::NYson::EYsonFormat::Binary));
    }

    NThreading::TFuture<TTableRangeResult> GetTableRange(TTableRangeOptions&& options) final {
        ui64 generation = SessionGenerations_[options.SessionId()];
        TString key;
        if (QContext_) {
            key = MakeGetTableRangeKey(options, generation);
        }

        if (QContext_.CanRead()) {
            TTableRangeResult res;
            res.SetSuccess();
            auto item = QContext_.GetReader()->Get({YtGateway_GetTableRange, key}).GetValueSync();
            if (!item) {
                throw yexception() << "Missing replay data";
            }

            auto listNode = NYT::NodeFromYsonString(item->Value);
            for (const auto& valueNode : listNode.AsList()) {
                TCanonizedPath p;
                p.Path = valueNode["Path"].AsString();
                if (valueNode.HasKey("Columns")) {
                    p.Columns.ConstructInPlace();
                    for (const auto& c : valueNode["Columns"].AsList()) {
                        p.Columns->push_back(c.AsString());
                    }
                }

                if (valueNode.HasKey("Ranges")) {
                    p.Ranges.ConstructInPlace();
                    for (const auto& r : valueNode["Ranges"].AsList()) {
                        NYT::TReadRange range;
                        NYT::Deserialize(range, r);
                        p.Ranges->push_back(range);
                    }
                }

                if (valueNode.HasKey("AdditionalAttributes")) {
                    p.AdditionalAttributes = valueNode["AdditionalAttributes"].AsString();
                }

                res.Tables.push_back(p);
            }

            return NThreading::MakeFuture<TTableRangeResult>(res);
        }

        return Inner_->GetTableRange(std::move(options))
            .Subscribe([key, qContext = QContext_](const NThreading::TFuture<TTableRangeResult>& future) {
                if (!qContext.CanWrite() || future.HasException()) {
                    return;
                }

                const auto& res = future.GetValueSync();
                if (!res.Success()) {
                    return;
                }

                auto listNode = NYT::TNode::CreateList();
                for (const auto& t : res.Tables) {
                    listNode.Add();
                    auto& valueNode = listNode.AsList().back();
                    valueNode("Path", t.Path);
                    if (t.Columns) {
                        NYT::TNode columnsNode = NYT::TNode::CreateList();
                        for (const auto& c : *t.Columns) {
                            columnsNode.Add(NYT::TNode(c));
                        }

                        valueNode("Columns", columnsNode);
                    }

                    if (t.Ranges) {
                        NYT::TNode rangesNode = NYT::TNode::CreateList();
                        for (const auto& r : *t.Ranges) {
                            NYT::TNode rangeNode;
                            NYT::TNodeBuilder builder(&rangeNode);
                            NYT::Serialize(r, &builder);
                            rangesNode.Add(rangeNode);
                        }

                        valueNode("Ranges", rangesNode);
                    }

                    if (t.AdditionalAttributes) {
                        valueNode("AdditionalAttributes", NYT::TNode(*t.AdditionalAttributes));
                    }
                }

                auto value = NYT::NodeToYsonString(listNode, NYT::NYson::EYsonFormat::Binary);
                qContext.GetWriter()->Put({YtGateway_GetTableRange, key}, value).GetValueSync();
        });
    }

    static TString MakeGetFolderKey(const TFolderOptions& options, ui64 generation) {
        auto attrNode = NYT::TNode::CreateList();
        for (const auto& attr : options.Attributes()) {
            attrNode.Add(NYT::TNode(attr));
        }

        auto keyNode = NYT::TNode()
            ("Generation", generation)
            ("Cluster", options.Cluster())
            ("Prefix", options.Prefix())
            ("Attributes", attrNode);

        return MakeHash(NYT::NodeToCanonicalYsonString(keyNode, NYT::NYson::EYsonFormat::Binary));
    }

    static TString MakeResolveLinksKey(const TResolveOptions& options, ui64 generation) {
        auto itemsNode = NYT::TNode::CreateList();
        for (const auto& item : options.Items()) {
            auto attrNode = NYT::TNode::CreateList();
            for (const auto& attr : item.AttrKeys) {
                attrNode.Add(NYT::TNode(attr));
            }

            itemsNode.Add(NYT::TNode()
                ("AttrKeys", attrNode)
                ("FolderItem", NYT::TNode()
                    ("Path", item.Item.Path)
                    ("Type", item.Item.Type)
                    ("Attributes", item.Item.Attributes)));
        }

        auto keyNode = NYT::TNode()
            ("Generation", generation)
            ("Cluster", options.Cluster())
            ("Items", itemsNode);

        return MakeHash(NYT::NodeToCanonicalYsonString(keyNode, NYT::NYson::EYsonFormat::Binary));
    }

    static TString MakeGetFoldersKey(const TBatchFolderOptions& options, ui64 generation) {
        auto itemsNode = NYT::TNode();
        TMap<TString, size_t> order;
        for (size_t i = 0; i < options.Folders().size(); ++i) {
            order[options.Folders()[i].Prefix] = i;
        }

        for (const auto& o : order) {
            const auto& folder = options.Folders()[o.second];
            auto attrNode = NYT::TNode::CreateList();
            for (const auto& attr : folder.AttrKeys) {
                attrNode.Add(NYT::TNode(attr));
            }

            itemsNode.Add(NYT::TNode()
                ("Prefix", folder.Prefix)
                ("Attrs", attrNode));
        }

        auto keyNode = NYT::TNode()
            ("Generation", generation)
            ("Cluster", options.Cluster())
            ("Items", itemsNode);

        return MakeHash(NYT::NodeToCanonicalYsonString(keyNode, NYT::NYson::EYsonFormat::Binary));
    }

    template <typename T>
    static NYT::TNode SerializeFolderItem(const T& item) {
        return NYT::TNode()
            ("Path", item.Path)
            ("Type", item.Type)
            ("Attributes", item.Attributes);
    }

    template <typename T>
    static void DeserializeFolderItem(T& item, const NYT::TNode& node) {
        item.Path = node["Path"].AsString();
        item.Type = node["Type"].AsString();
        if constexpr (std::is_same_v<decltype(item.Attributes), TString>) {
            item.Attributes = node["Attributes"].AsString();
        } else {
            item.Attributes = node["Attributes"];
        }
    }

    NThreading::TFuture<TFolderResult> GetFolder(TFolderOptions&& options) final {
        ui64 generation = SessionGenerations_[options.SessionId()];
        if (QContext_.CanRead()) {
            const auto& key = MakeGetFolderKey(options, generation);
            auto item = QContext_.GetReader()->Get({YtGateway_GetFolder, key}).GetValueSync();
            if (!item) {
                throw yexception() << "Missing replay data";
            }

            TFolderResult res;
            res.SetSuccess();
            auto valueNode = NYT::NodeFromYsonString(TStringBuf(item->Value));
            if (valueNode.IsString()) {
                const TString file = FileStorage_->GetTemp() / GetGuidAsString(RandomProvider_->GenGuid());
                auto out = MakeHolder<TOFStream>(file);
                out->Write(valueNode.AsString());
                res.ItemsOrFileLink = CreateFakeFileLink(file, "", true);
            } else {
                TVector<TFolderResult::TFolderItem> items;
                for (const auto& child : valueNode.AsList()) {
                    TFolderResult::TFolderItem item;
                    DeserializeFolderItem(item, child);
                    items.push_back(item);
                }

                res.ItemsOrFileLink = items;
            }
            return NThreading::MakeFuture<TFolderResult>(res);
        }

        auto optionsDup = options;
        return Inner_->GetFolder(std::move(options))
            .Subscribe([optionsDup, qContext = QContext_, generation](const NThreading::TFuture<TFolderResult>& future) {
                if (!qContext.CanWrite() || future.HasException()) {
                    return;
                }

                const auto& res = future.GetValueSync();
                if (!res.Success()) {
                    return;
                }

                const auto& key = MakeGetFolderKey(optionsDup, generation);
                auto valueNode = NYT::TNode();

                if (std::holds_alternative<TFileLinkPtr>(res.ItemsOrFileLink)) {
                    const auto& file = std::get<TFileLinkPtr>(res.ItemsOrFileLink);
                    valueNode = NYT::TNode(TFileInput(file->GetPath()).ReadAll());
                } else {
                    valueNode = NYT::TNode::CreateList();
                    const auto& items = std::get<TVector<TFolderResult::TFolderItem>>(res.ItemsOrFileLink);
                    for (const auto& item: items) {
                        valueNode.Add(SerializeFolderItem(item));
                    }
                }

                auto value = NYT::NodeToYsonString(valueNode, NYT::NYson::EYsonFormat::Binary);
                qContext.GetWriter()->Put({YtGateway_GetFolder, key}, value).GetValueSync();
        });
    }

    NThreading::TFuture<TBatchFolderResult> ResolveLinks(TResolveOptions&& options) final {
        ui64 generation = SessionGenerations_[options.SessionId()];
        if (QContext_.CanRead()) {
            TBatchFolderResult res;
            res.SetSuccess();
            const auto& key = MakeResolveLinksKey(options, generation);
            auto item = QContext_.GetReader()->Get({YtGateway_ResolveLinks, key}).GetValueSync();
            if (!item) {
                throw yexception() << "Missing replay data";
            }

            auto valueNode = NYT::NodeFromYsonString(TStringBuf(item->Value));
            for (const auto& child : valueNode.AsList()) {
                TBatchFolderResult::TFolderItem folderItem;
                DeserializeFolderItem(folderItem, child);
                res.Items.push_back(folderItem);
            }

            return NThreading::MakeFuture<TBatchFolderResult>(res);
        }

        auto optionsDup = options;
        return Inner_->ResolveLinks(std::move(options))
            .Subscribe([optionsDup, qContext = QContext_, generation](const NThreading::TFuture<TBatchFolderResult>& future) {
                if (!qContext.CanWrite() || future.HasException()) {
                    return;
                }

                const auto& res = future.GetValueSync();
                if (!res.Success()) {
                    return;
                }

                const auto& key = MakeResolveLinksKey(optionsDup, generation);
                NYT::TNode valueNode = NYT::TNode::CreateList();
                for (const auto& item : res.Items) {
                    valueNode.Add(SerializeFolderItem(item));
                }

                auto value = NYT::NodeToYsonString(valueNode, NYT::NYson::EYsonFormat::Binary);
                qContext.GetWriter()->Put({YtGateway_ResolveLinks, key}, value).GetValueSync();
            });
    }

    NThreading::TFuture<TBatchFolderResult> GetFolders(TBatchFolderOptions&& options) final {
        ui64 generation = SessionGenerations_[options.SessionId()];
        if (QContext_.CanRead()) {
            TBatchFolderResult res;
            res.SetSuccess();
            const auto& key = MakeGetFoldersKey(options, generation);
            auto item = QContext_.GetReader()->Get({YtGateway_GetFolders, key}).GetValueSync();
            if (!item) {
                throw yexception() << "Missing replay data";
            }

            auto valueNode = NYT::NodeFromYsonString(TStringBuf(item->Value));
            for (const auto& child : valueNode.AsList()) {
                TBatchFolderResult::TFolderItem folderItem;
                DeserializeFolderItem(folderItem, child);
                res.Items.push_back(folderItem);
            }

            return NThreading::MakeFuture<TBatchFolderResult>(res);
        }

        auto optionsDup = options;
        return Inner_->GetFolders(std::move(options))
            .Subscribe([optionsDup, qContext = QContext_, generation](const NThreading::TFuture<TBatchFolderResult>& future) {
                if (!qContext.CanWrite() || future.HasException()) {
                    return;
                }

                const auto& res = future.GetValueSync();
                if (!res.Success()) {
                    return;
                }

                const auto& key = MakeGetFoldersKey(optionsDup, generation);
                NYT::TNode valueNode = NYT::TNode::CreateList();
                for (const auto& item : res.Items) {
                    valueNode.Add(SerializeFolderItem(item));
                }

                auto value = NYT::NodeToYsonString(valueNode, NYT::NYson::EYsonFormat::Binary);
                qContext.GetWriter()->Put({YtGateway_GetFolders, key}, value).GetValueSync();
        });
    }

    NThreading::TFuture<TResOrPullResult> ResOrPull(const TExprNode::TPtr& node, TExprContext& ctx, TResOrPullOptions&& options) final {
        if (QContext_.CanRead()) {
            throw yexception() << "Can't replay ResOrPull";
        }

        return Inner_->ResOrPull(node, ctx, std::move(options));
    }

    NThreading::TFuture<TRunResult> Run(const TExprNode::TPtr& node, TExprContext& ctx, TRunOptions&& options) final {
        if (QContext_.CanRead()) {
            throw yexception() << "Can't replay Run";
        }

        return Inner_->Run(node, ctx, std::move(options));
    }

    NThreading::TFuture<TRunResult> Prepare(const TExprNode::TPtr& node, TExprContext& ctx, TPrepareOptions&& options) const final {
        if (QContext_.CanRead()) {
            throw yexception() << "Can't replay Prepare";
        }

        return Inner_->Prepare(node, ctx, std::move(options));
    }

    NThreading::TFuture<TRunResult> GetTableStat(const TExprNode::TPtr& node, TExprContext& ctx, TPrepareOptions&& options) final {
        if (QContext_.CanRead()) {
            throw yexception() << "Can't replay GetTableStat";
        }

        return Inner_->GetTableStat(node, ctx, std::move(options));
    }

    NThreading::TFuture<TCalcResult> Calc(const TExprNode::TListType& nodes, TExprContext& ctx, TCalcOptions&& options) final {
        if (QContext_.CanRead()) {
            throw yexception() << "Can't replay Calc";
        }

        return Inner_->Calc(nodes, ctx, std::move(options));
    }

    NThreading::TFuture<TPublishResult> Publish(const TExprNode::TPtr& node, TExprContext& ctx, TPublishOptions&& options) final {
        if (QContext_.CanRead()) {
            throw yexception() << "Can't replay Publish";
        }

        return Inner_->Publish(node, ctx, std::move(options));
    }

    NThreading::TFuture<TCommitResult> Commit(TCommitOptions&& options) final {
        if (QContext_.CanRead()) {
            throw yexception() << "Can't replay Commit";
        }

        return Inner_->Commit(std::move(options));
    }

    NThreading::TFuture<TDropTrackablesResult> DropTrackables(TDropTrackablesOptions&& options) final {
        if (QContext_.CanRead()) {
            throw yexception() << "Can't replay DropTrackables";
        }

        return Inner_->DropTrackables(std::move(options));
    }

    static TString MakePathStatKey(const TString& cluster, bool extended, const TPathStatReq& req, ui64 generation) {
        auto node = NYT::TNode()
            ("Generation", generation)
            ("Cluster", cluster)
            ("Extended", extended);

        NYT::TNode pathNode;
        NYT::TNodeBuilder builder(&pathNode);
        NYT::Serialize(req.Path(), &builder);
        auto path = NYT::TNode()
            ("Path", pathNode)
            ("IsTemp", req.IsTemp())
            ("IsAnonymous", req.IsAnonymous())
            ("Epoch", req.Epoch());

        node("Path", path);
        return MakeHash(NYT::NodeToCanonicalYsonString(node));
    }

    static TString SerializePathStat(const TPathStatResult& stat, ui32 index) {
        Y_ENSURE(index < stat.DataSize.size());
        Y_ENSURE(index < stat.Extended.size());
        auto xNode = NYT::TNode();
        if (!stat.Extended[index].Defined()) {
            xNode = NYT::TNode::CreateEntity();
        } else {
            auto dataWeightMap = NYT::TNode::CreateMap();
            for (const auto& d : stat.Extended[index]->DataWeight) {
                dataWeightMap(d.first, d.second);
            }

            auto uniqCountsMap = NYT::TNode::CreateMap();
            for (const auto& e : stat.Extended[index]->EstimatedUniqueCounts) {
                uniqCountsMap(e.first, e.second);
            }

            xNode = NYT::TNode()
                ("DataWeight", dataWeightMap)
                ("EstimatedUniqueCounts", uniqCountsMap);
        }

        auto node = NYT::TNode()
            ("DataSize", stat.DataSize[index])
            ("Extended", xNode);

        return NYT::NodeToYsonString(node, NYT::NYson::EYsonFormat::Binary);
    }

    static void DeserializePathStat(const NYT::TNode& node, TPathStatResult& stat, ui32 index) {
        Y_ENSURE(index < stat.DataSize.size());
        Y_ENSURE(index < stat.Extended.size());
        stat.DataSize[index] = node["DataSize"].AsUint64();
        stat.Extended[index] = Nothing();
        const auto& x  = node["Extended"];
        if (!x.IsEntity()) {
            auto& xValue = stat.Extended[index];
            xValue.ConstructInPlace();
            for (const auto& d : x["DataWeight"].AsMap()) {
                xValue->DataWeight[d.first] = d.second.AsInt64();
            }

            for (const auto& e : x["EstimatedUniqueCounts"].AsMap()) {
                xValue->EstimatedUniqueCounts[e.first] = e.second.AsUint64();
            }
        }
    }

    NThreading::TFuture<TPathStatResult> PathStat(TPathStatOptions&& options) final {
        ui64 generation = SessionGenerations_[options.SessionId()];
        if (QContext_.CanRead()) {
            TPathStatResult res;
            res.DataSize.resize(options.Paths().size(), 0);
            res.Extended.resize(options.Paths().size());

            for (ui32 index = 0; index < options.Paths().size(); ++index) {
                const auto& key = MakePathStatKey(options.Cluster(), options.Extended(), options.Paths()[index], generation);
                auto item = QContext_.GetReader()->Get({YtGateway_PathStat, key}).GetValueSync();
                if (!item) {
                    throw yexception() << "Missing replay data";
                }

                PathStatKeys_.emplace(key);
                auto valueNode = NYT::NodeFromYsonString(TStringBuf(item->Value));
                DeserializePathStat(valueNode, res, index);
            }

            res.SetSuccess();
            return NThreading::MakeFuture<TPathStatResult>(res);
        }

        auto optionsDup = options;
        return Inner_->PathStat(std::move(options))
            .Subscribe([optionsDup, qContext = QContext_, generation](const NThreading::TFuture<TPathStatResult>& future) {
                if (!qContext.CanWrite() || future.HasException()) {
                    return;
                }

                const auto& res = future.GetValueSync();
                if (!res.Success()) {
                    return;
                }

                for (ui32 index = 0; index < optionsDup.Paths().size(); ++index) {
                    const auto& key = MakePathStatKey(optionsDup.Cluster(), optionsDup.Extended(), optionsDup.Paths()[index], generation);
                    auto value = SerializePathStat(res, index);
                    qContext.GetWriter()->Put({YtGateway_PathStat, key}, value).GetValueSync();
                }
        });
    }

    TPathStatResult TryPathStat(TPathStatOptions&& options) final {
        ui64 generation = SessionGenerations_[options.SessionId()];
        if (QContext_.CanRead()) {
            TPathStatResult res;
            res.DataSize.resize(options.Paths().size(), 0);
            res.Extended.resize(options.Paths().size());

            for (ui32 index = 0; index < options.Paths().size(); ++index) {
                const auto& key = MakePathStatKey(options.Cluster(), options.Extended(), options.Paths()[index], generation);
                bool allow = false;
                if (PathStatKeys_.contains(key)) {
                    allow = true;
                } else {
                    auto missingItem = QContext_.GetReader()->Get({YtGateway_PathStatMissing, key}).GetValueSync();
                    if (!missingItem) {
                        allow = true;
                        PathStatKeys_.emplace(key);
                    }
                }

                if (!allow) {
                    return res;
                }

                auto item = QContext_.GetReader()->Get({YtGateway_PathStat, key}).GetValueSync();
                if (!item) {
                    return res;
                }

                auto valueNode = NYT::NodeFromYsonString(TStringBuf(item->Value));
                DeserializePathStat(valueNode, res, index);
            }

            res.SetSuccess();
            return res;
        }

        auto optionsDup = options;
        auto res = Inner_->TryPathStat(std::move(options));
        if (!QContext_.CanWrite()) {
            return res;
        }

        if (!res.Success()) {
            for (ui32 index = 0; index < optionsDup.Paths().size(); ++index) {
                const auto& key = MakePathStatKey(optionsDup.Cluster(), optionsDup.Extended(), optionsDup.Paths()[index], generation);
                QContext_.GetWriter()->Put({YtGateway_PathStatMissing, key}, "1").GetValueSync();
            }

            return res;
        }

        for (ui32 index = 0; index < optionsDup.Paths().size(); ++index) {
            const auto& key = MakePathStatKey(optionsDup.Cluster(), optionsDup.Extended(), optionsDup.Paths()[index], generation);
            auto value = SerializePathStat(res, index);
            QContext_.GetWriter()->Put({YtGateway_PathStat, key}, value).GetValueSync();
        }

        return res;
    }

    bool TryParseYtUrl(const TString& url, TString* cluster, TString* path) const final {
        return Inner_->TryParseYtUrl(url, cluster, path);
    }

    TString GetDefaultClusterName() const final {
        return Inner_->GetDefaultClusterName();
    }

    TString GetClusterServer(const TString& cluster) const final {
        return Inner_->GetClusterServer(cluster);
    }

    NYT::TRichYPath GetRealTable(const TString& sessionId, const TString& cluster, const TString& table, ui32 epoch, const TString& tmpFolder, bool temp, bool anonymous) const final {
        return Inner_->GetRealTable(sessionId, cluster, table, epoch, tmpFolder, temp, anonymous);
    }

    NYT::TRichYPath GetWriteTable(const TString& sessionId, const TString& cluster, const TString& table, const TString& tmpFolder) const final {
        return Inner_->GetWriteTable(sessionId, cluster, table, tmpFolder);
    }

    NThreading::TFuture<TDownloadTablesResult> DownloadTables(TDownloadTablesOptions&& options) final {
        if (QContext_.CanRead()) {
            TDownloadTablesResult res;
            res.SetSuccess();
            return NThreading::MakeFuture(std::move(res));
        }
        return Inner_->DownloadTables(std::move(options));
    }

    NThreading::TFuture<TUploadTableResult> UploadTable(TUploadTableOptions&& options) final {
        if (QContext_.CanRead()) {
            throw yexception() << "Can't replay UploadTable";
        }
        return Inner_->UploadTable(std::move(options));
    }

    TFullResultTableResult PrepareFullResultTable(TFullResultTableOptions&& options) final {
        if (QContext_.CanRead()) {
            throw yexception() << "Can't replay PrepareFullResultTable";
        }

        return Inner_->PrepareFullResultTable(std::move(options));
    }

    void SetStatUploader(IStatUploader::TPtr statUploader) final {
        return Inner_->SetStatUploader(statUploader);
    }

    void RegisterMkqlCompiler(NCommon::TMkqlCallableCompilerBase& compiler) final {
        return Inner_->RegisterMkqlCompiler(compiler);
    }

    TGetTablePartitionsResult GetTablePartitions(TGetTablePartitionsOptions&& options) final {
        if (QContext_.CanRead()) {
            throw yexception() << "Can't replay GetTablePartitions";
        }

        return Inner_->GetTablePartitions(std::move(options));
    }

    void AddCluster(const TYtClusterConfig& cluster) final {
        return Inner_->AddCluster(cluster);
    }

    TClusterConnectionResult GetClusterConnection(const TClusterConnectionOptions&& options) override {
        return Inner_->GetClusterConnection(std::move(options));
    }

    TMaybe<TString> GetTableFilePath(const TGetTableFilePathOptions&& options) override {
        return Inner_->GetTableFilePath(std::move(options));
    }

private:
    const IYtGateway::TPtr Inner_;
    const TQContext QContext_;
    const TIntrusivePtr<IRandomProvider> RandomProvider_;
    const TFileStoragePtr FileStorage_;
    THashSet<TString> PathStatKeys_;
    THashMap<TString, ui64> SessionGenerations_;
};

}

IYtGateway::TPtr WrapYtGatewayWithQContext(IYtGateway::TPtr gateway, const TQContext& qContext,
    const TIntrusivePtr<IRandomProvider>& randomProvider, const TFileStoragePtr& fileStorage) {
    return MakeIntrusive<TGateway>(gateway, qContext, randomProvider, fileStorage);
}

}
