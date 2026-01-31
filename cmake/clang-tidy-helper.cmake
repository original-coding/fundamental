if(__CLANG_TIDY_HELPER__)
    return()
endif()
set(__CLANG_TIDY_HELPER__ TRUE)


option(ENABLE_CLANG_TIDY_CHECK "enable clang tidy check" OFF)

find_program(CLANG_TIDY_EXE NAMES "clang-tidy-4.0" "clang-tidy40" "clang-tidy" DOC "Path to clang-tidy executable")
if(NOT CLANG_TIDY_EXE)
    message(STATUS "clang-tidy not found.")
else()
    message(STATUS "clang-tidy found: ${CLANG_TIDY_EXE}")
    # string(CONCAT TIDY_OPTS "-checks=*"
    #     ",-cert-err58-cpp"
    #     ",-cppcoreguidelines-pro-*"
    #     ",-google-build-using-namespace"
    #     ",-google-readability-casting"
    #     ",-google-readability-todo"
    #     ",-google-runtime-int"
    #     ",-google-runtime-references"
    #     ",-modernize-loop-convert"
    #     ",-performance-inefficient-string-concatenation"
    #     ",-readability-inconsistent-declaration-parameter-name"
    #     ",-readability-named-parameter"
    #     ",clang-diagnostic-*,bugprone-*,misc-*"
    #     ",-warnings-as-errors=clang-diagnostic-implicit-function-declaration"
    # )
    string(CONCAT TIDY_OPTS "-checks=-*,"
        "clang-diagnostic-*,bugprone-*,misc-*"
    )
    set(DO_CLANG_TIDY "${CLANG_TIDY_EXE}" ${TIDY_OPTS})
endif()
# supported by clang compiler
function(target_enable_clang_tidy target_name)
    if(NOT ENABLE_CLANG_TIDY_CHECK)
        return()
    endif()
    if(NOT CLANG_TIDY_EXE)
        return()
    endif()
    set_target_properties(${target_name} PROPERTIES CXX_CLANG_TIDY  "${DO_CLANG_TIDY}")
    set_target_properties(${target_name} PROPERTIES INTERFACE_CXX_CLANG_TIDY  "${DO_CLANG_TIDY}")
endfunction()
