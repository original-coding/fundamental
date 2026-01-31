if(__reflect_helper__)
    return()
endif()
set(__reflect_helper__ TRUE)

function(fetch_reflection_src GroupName DstSrcs)
set(__REFLECTIONT_PROPERTY_NAME__ "__Reflect_group_${GroupName}__")
get_property(srcs GLOBAL PROPERTY ${__REFLECTIONT_PROPERTY_NAME__})
list(APPEND ${DstSrcs} ${srcs})
set(${DstSrcs} ${${DstSrcs}} PARENT_SCOPE)
endfunction()

## RegisterSrcs filelist
##
function(register_reflection_src GroupName RegisterSrcs)
fetch_reflection_src(${GroupName} __CURRENT_VALUE__)
list(APPEND __CURRENT_VALUE__ ${${RegisterSrcs}})
set(__REFLECTIONT_PROPERTY_NAME__ "__Reflect_group_${GroupName}__")
set_property(GLOBAL PROPERTY ${__REFLECTIONT_PROPERTY_NAME__} ${__CURRENT_VALUE__})
endfunction()




