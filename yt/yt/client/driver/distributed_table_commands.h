#pragma once

#include "table_commands.h"

#include <yt/yt/client/formats/format.h>

#include <yt/yt/client/ypath/rich.h>

namespace NYT::NDriver {

////////////////////////////////////////////////////////////////////////////////

// -> DistributedWriteSession
class TStartDistributedWriteSessionCommand
    : public TTypedCommand<NApi::TDistributedWriteSessionStartOptions>
{
public:
    REGISTER_YSON_STRUCT_LITE(TStartDistributedWriteSessionCommand);

    static void Register(TRegistrar registrar);

private:
    NYPath::TRichYPath Path;

    void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

// -> Nothing
class TFinishDistributedWriteSessionCommand
    : public TTypedCommand<NApi::TDistributedWriteSessionFinishOptions>
{
public:
    REGISTER_YSON_STRUCT_LITE(TFinishDistributedWriteSessionCommand);

    static void Register(TRegistrar registrar);

private:
    NYTree::INodePtr Session;
    std::vector<NYTree::INodePtr> Results;

    void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

// -> Cookie
class TWriteTableFragmentCommand
    : public TTypedCommand<NApi::TTableFragmentWriterOptions>
{
public:
    REGISTER_YSON_STRUCT_LITE(TWriteTableFragmentCommand);

    static void Register(TRegistrar registrar);

private:
    using TBase = TWriteTableCommand;

    NYTree::INodePtr Cookie;
    i64 MaxRowBufferSize;

    NApi::ITableFragmentWriterPtr CreateTableWriter(const ICommandContextPtr& context);

    void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NDriver
