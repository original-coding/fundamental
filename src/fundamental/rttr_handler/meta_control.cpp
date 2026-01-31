#include "meta_control.h"
#include <algorithm>
#include <vector>
namespace Fundamental {
std::string RttrMetaControlOption::ExcludeMetaDataKey() {
    static constexpr const char* kExcludeMetaDataKey = "__exclude_data__";
    return kExcludeMetaDataKey;
}

std::string RttrMetaControlOption::IncludeMetaDataKey() {
    static constexpr const char* kIncludeMetaDataKey = "__include_data__";
    return kIncludeMetaDataKey;
}

std::string RttrMetaControlOption::CommentMetaDataKey() {
    static constexpr const char* kCommentMetaDataKey = "__comment_data__";
    return kCommentMetaDataKey;
}

bool RttrMetaControlOption::HasInterSection(const RttrControlMetaDataType& data1,
                                            const RttrControlMetaDataType& data2) {
    std::vector<RttrControlMetaDataType::value_type> out;
    std::set_intersection(data1.begin(), data1.end(), data2.begin(), data2.end(), std::back_inserter(out));
    return !out.empty();
}

bool RttrMetaControlOption::ValidateSerialize(const rttr::property& prop) const {
    do {
        auto propExclueData = prop.get_metadata(ExcludeMetaDataKey());
        if (propExclueData.is_type<RttrControlMetaDataType>()) {
            auto& propExclueDataRef = propExclueData.get_wrapped_value<RttrControlMetaDataType>();
            if (!propExclueDataRef.empty() && !excludeDatas.empty()) {
                // check whether prop is excluded
                if (HasInterSection(propExclueDataRef, excludeDatas)) break;
            }
        }
        auto propIncludeData = prop.get_metadata(IncludeMetaDataKey());
        if (propIncludeData.is_type<RttrControlMetaDataType>()) {
            auto& propIncludeDataRef = propIncludeData.get_wrapped_value<RttrControlMetaDataType>();
            if (!propIncludeDataRef.empty() && !includeDatas.empty()) {
                // check whether prop is excluded
                if (!HasInterSection(propIncludeDataRef, includeDatas)) break;
            }
        }
        return true;
    } while (0);

    return false;
}

} // namespace Fundamental