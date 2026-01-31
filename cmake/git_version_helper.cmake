# Git信息获取函数
# 参数:
#   SOURCE_DIR - Git工作目录（默认为${CMAKE_SOURCE_DIR}）
# 返回值:
#   GIT_COMMIT_DATE - 提交日期（格式：YYYYMMDD）
#   GIT_COMMIT_HASH - 提交哈希（短格式）
#   BRANCH_NAME - 分支名称
#   GIT_REPO_TAG - 最近标签
#   GIT_COMMIT_INFO - 组合信息：分支-日期-标签-哈希
function(get_git_info)
    # 解析函数参数
    cmake_parse_arguments(
        GIT_INFO
        "" # 无选项
        "SOURCE_DIR" # 单值参数
        "" # 多值参数
        ${ARGN}
    )

    # 设置默认工作目录
    if(NOT GIT_INFO_SOURCE_DIR)
        set(GIT_INFO_SOURCE_DIR ${CMAKE_SOURCE_DIR})
    endif()

    # 定义返回变量（默认值）
    set(GIT_COMMIT_DATE "[undefined]" PARENT_SCOPE)
    set(GIT_COMMIT_HASH "[undefined]" PARENT_SCOPE)
    set(BRANCH_NAME "[undefined]" PARENT_SCOPE)
    set(GIT_REPO_TAG "[no tag]" PARENT_SCOPE)
    set(GIT_COMMIT_INFO "[undefined]" PARENT_SCOPE)

    # 检查git是否存在
    find_program(GIT_EXECUTABLE git)
    if(NOT GIT_EXECUTABLE)
        message(WARNING "Git executable not found. Git info will be 'undefined'.")
        return()
    endif()
    # 检查是否在git仓库中
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --is-inside-work-tree
        WORKING_DIRECTORY ${GIT_INFO_SOURCE_DIR}
        OUTPUT_VARIABLE IS_GIT_REPO
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    if(NOT IS_GIT_REPO STREQUAL "true")
        message(WARNING "Not inside a git repository: ${GIT_INFO_SOURCE_DIR}")
        return()
    endif()

    # 获取提交日期
    execute_process(
        COMMAND ${GIT_EXECUTABLE} log -1 --date=format-local:%Y%m%d --pretty=format:%cd
        WORKING_DIRECTORY ${GIT_INFO_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_DATE_TMP
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(GIT_DATE_TMP)
        set(GIT_COMMIT_DATE ${GIT_DATE_TMP} PARENT_SCOPE)
    endif()

    # 获取提交哈希
    execute_process(
        COMMAND ${GIT_EXECUTABLE} log -1 --format=%h
        WORKING_DIRECTORY ${GIT_INFO_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_HASH_TMP
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    if(GIT_HASH_TMP)
        set(GIT_COMMIT_HASH ${GIT_HASH_TMP} PARENT_SCOPE)
    endif()

    # 获取分支名称
    execute_process(
        COMMAND ${GIT_EXECUTABLE} symbolic-ref --short -q HEAD
        WORKING_DIRECTORY ${GIT_INFO_SOURCE_DIR}
        OUTPUT_VARIABLE BRANCH_TMP
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    if(BRANCH_TMP)
        set(BRANCH_NAME ${BRANCH_TMP} PARENT_SCOPE)
    endif()

    # 获取最近标签
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --abbrev=0
        WORKING_DIRECTORY ${GIT_INFO_SOURCE_DIR}
        OUTPUT_VARIABLE TAG_TMP
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    if(TAG_TMP)
        set(GIT_REPO_TAG ${TAG_TMP} PARENT_SCOPE)
    endif()

    # 构建组合信息（仅当所有值都有效时）
    if(NOT GIT_DATE_TMP STREQUAL "[undefined]" AND
        NOT GIT_HASH_TMP STREQUAL "[undefined]" AND
        NOT BRANCH_TMP STREQUAL "[undefined]")
        set(GIT_COMMIT_INFO
            "${BRANCH_TMP}-${GIT_DATE_TMP}-${TAG_TMP}-${GIT_HASH_TMP}"
            PARENT_SCOPE
        )
    endif()
endfunction()
