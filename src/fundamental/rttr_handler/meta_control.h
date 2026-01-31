//
// @author xuyuanwang <469953258@qq.com> 2024/05
//
#pragma once
#ifdef _MSC_VER
    #pragma warning(push)
    #pragma warning(disable : 26819 26437 26439 26495 26800 26498) // disable warning 4996
#endif
#include "nlohmann/json.hpp"
#include <rttr/type>
#ifdef _MSC_VER
    #pragma warning(pop)
#endif
#include <set>
#include <string>
#include <string_view>

namespace Fundamental {
using RttrControlMetaDataType = std::set<std::string>;
struct RttrMetaControlOption {
    RttrControlMetaDataType excludeDatas;
    RttrControlMetaDataType includeDatas;
    static std::string ExcludeMetaDataKey();
    static std::string IncludeMetaDataKey();
    static std::string CommentMetaDataKey();
    static bool HasInterSection(const RttrControlMetaDataType& data1, const RttrControlMetaDataType& data2);
    bool ValidateSerialize(const rttr::property& prop) const;
};

using RttrSerializeOption   = RttrMetaControlOption;
using RttrDeserializeOption = RttrMetaControlOption;

} // namespace Fundamental