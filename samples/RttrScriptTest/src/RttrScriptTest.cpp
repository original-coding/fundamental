/************************************************************************************
 *                                                                                   *
 *   Copyright (c) 2014 - 2018 Axel Menzel <info@rttr.org>                           *
 *                                                                                   *
 *   This file is part of RTTR (Run Time Type Reflection)                            *
 *   License: MIT License                                                            *
 *                                                                                   *
 *   Permission is hereby granted, free of charge, to any person obtaining           *
 *   a copy of this software and associated documentation files (the "Software"),    *
 *   to deal in the Software without restriction, including without limitation       *
 *   the rights to use, copy, modify, merge, publish, distribute, sublicense,        *
 *   and/or sell copies of the Software, and to permit persons to whom the           *
 *   Software is furnished to do so, subject to the following conditions:            *
 *                                                                                   *
 *   The above copyright notice and this permission notice shall be included in      *
 *   all copies or substantial portions of the Software.                             *
 *                                                                                   *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR      *
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,        *
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE     *
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER          *
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,   *
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE   *
 *   SOFTWARE.                                                                       *
 *                                                                                   *
 *************************************************************************************/

#include "fundamental/rttr_handler/script_support/jsscript_visitor.h"
#include <iostream>

#include <rttr/registration>

#include "fundamental/basic/filesystem_utils.hpp"
#include <rttr/type>

#include "fundamental/basic/log.h"
#include "fundamental/rttr_handler/serializer.h"

using namespace rttr;

enum class test_enum : std::uint32_t
{
    enum_value1,
    enum_value2
};

[[maybe_unused]] static int64_t get_int64_min(std::int64_t in) {
    auto ret = std::numeric_limits<std::int64_t>::min();
    std::cout << "int64 min:" << ret << " " << in << std::endl;
    return ret;
};

[[maybe_unused]] static int64_t get_int64_max(std::int64_t in) {
    auto ret = std::numeric_limits<std::int64_t>::max();
    std::cout << "int64 max:" << ret << " " << in << std::endl;
    return ret;
};

[[maybe_unused]] static std::uint64_t get_uint64_min(std::uint64_t in) {
    auto ret = std::numeric_limits<std::uint64_t>::min();
    std::cout << "uint64_t min:" << ret << " " << in << std::endl;
    return ret;
};

[[maybe_unused]] static std::uint64_t get_uint64_max(std::uint64_t in) {
    auto ret = std::numeric_limits<std::uint64_t>::max();
    std::cout << "uint64_t max:" << ret << " " << in << std::endl;
    return ret;
};

[[maybe_unused]] static void my_assert(bool check) {
    if (!check) {
        throw "check failed";
    }
};

[[maybe_unused]] static void println(qjs::rest<std::string> args) {
    for (auto const& arg : args)
        std::cout << arg << " ";
    std::cout << "\n";
}

[[maybe_unused]] static std::int64_t global_func() {
    return std::numeric_limits<std::int64_t>::max();
}

[[maybe_unused]] static test_enum test_enum_value(test_enum v) {
    switch (v) {
    case test_enum::enum_value1: return test_enum::enum_value2;

    default: return test_enum::enum_value1;
    }
}

struct custom_object : Fundamental::script_base_type {
    custom_object() = default;
    custom_object(std::int32_t v_) : v(v_) {
    }
    std::int32_t v = 0;
};

struct test_class {
    test_class() {
    }
    test_class(int b) {
        x = b;
    }
    inline static int x = 0;
    static int get_x() {
        return x;
    }
};

class class_base : Fundamental::script_base_type {
public:
    virtual ~class_base() {
        std::cout << "~class_base" << std::endl;
    }
    class_base() = default;
    class_base(std::int32_t v) : base_object { v } {};
    class_base(std::int32_t v1, std::int32_t v2) : base_object { { v2 + v1 } } {
        std::cout << v1 << " + " << v2 << std::endl;
    };
    class_base(std::string v) : base_object { std::stoi(v) } {};

    static std::string static_func() {
        return "static_func";
    }
    static double static_func2() {
        return 1.11f;
    }
    std::string mem_func() {
        return "mem_func_base";
    }

    virtual std::string v_mem_func() {
        return "v_mem_func_base";
    }
    std::int32_t get_object_v() const {
        return base_object.v;
    }
    void set_object_v(std::int32_t v_) {
        base_object.v = v_;
    }
    static std::int32_t get_static_object_v() {
        return s_base_object.v;
    }
    static void set_static_object_v(std::int32_t v_) {
        s_base_object.v = v_;
    }
    custom_object base_object { 333 };
    const std::int32_t v = 1;
    inline static custom_object s_base_object { 444 };
    inline static const std::int32_t v_r = 2;
    RTTR_ENABLE()
};

class class_a : virtual public class_base {
public:
    class_a() = default;
    std::string mem_func() {
        return "mem_func_a";
    }
    RTTR_ENABLE(class_base)
};

class class_b : virtual public class_base {
public:
    class_b() = default;
    std::string mem_func() {
        return "mem_func_b";
    }
    std::string v_mem_func() override {
        return "v_mem_func_b";
    }
    RTTR_ENABLE(class_base)
};

class class_c : virtual public class_b, virtual public class_a {
public:
    class_c() = default;
    std::string mem_func() {
        return "mem_func_c";
    }
    std::string v_mem_func() override {
        return "v_mem_func_c";
    }
    RTTR_ENABLE(class_b, class_a)
};
static std::vector<std::int32_t> test_vec { 1, 2, 3, 4, 5, 6 };
static std::string global_property                = "global_property";
static const std::string readonly_global_property = "readonly_global_property";
static std::string get_readonly_global_property_f() {
    return "get_readonly_global_property_f";
}
static bool compare_vec(const std::vector<std::int32_t>& vec) {
    return vec == test_vec;
}
RTTR_REGISTRATION {
    rttr::registration::method("compare_vec", compare_vec)(
        metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(), "/** 函数测试 */"));
    rttr::registration::method("get_int64_min",
                               get_int64_min)(metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(),
                                                       Fundamental::StringFormat("/** test meta {} */", __LINE__)));
    rttr::registration::method("get_int64_max",
                               get_int64_max)(metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(),
                                                       Fundamental::StringFormat("/** test meta {} */", __LINE__)));
    rttr::registration::method("get_uint64_min",
                               get_uint64_min)(metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(),
                                                        Fundamental::StringFormat("/** test meta {} */", __LINE__)));
    rttr::registration::method("get_uint64_max",
                               get_uint64_max)(metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(),
                                                        Fundamental::StringFormat("/** test meta {} */", __LINE__)));
    rttr::registration::method("global_func",
                               global_func)(metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(),
                                                     Fundamental::StringFormat("/** test meta {} */", __LINE__)));
    rttr::registration::method("global_func_lambda", []() { return "global_lambda"; })(
        metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(),
                 Fundamental::StringFormat("/** test meta {} */", __LINE__)));
    rttr::registration::method("test_enum_value",
                               test_enum_value)(metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(),
                                                         Fundamental::StringFormat("/** test meta {} */", __LINE__)));
    rttr::registration::method("assert",
                               my_assert)(metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(),
                                                   Fundamental::StringFormat("/** test meta {} */", __LINE__)));
    rttr::registration::method("println",
                               println)(metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(),
                                                 Fundamental::StringFormat("/** test meta {} */", __LINE__)));
    rttr::registration::property("global_property", &global_property)(
        metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(),
                 Fundamental::StringFormat("/** test meta {} */", __LINE__)));
    rttr::registration::property(
        "global_property_s", []() -> const std::string& { return global_property; },
        [](const std::string& v) { global_property = v; })(
        metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(),
                 Fundamental::StringFormat("/** test meta {} */", __LINE__)));
    rttr::registration::property_readonly("test_vec", []() -> std::vector<std::int32_t> { return test_vec; })(
        metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(),
                 Fundamental::StringFormat("/** test meta {} */", __LINE__)));
    rttr::registration::property_readonly("readonly_global_property", &readonly_global_property)(
        metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(),
                 Fundamental::StringFormat("/** test meta {} */", __LINE__)));
    rttr::registration::property_readonly("readonly_global_property_s",
                                          []() -> std::string { return "readonly_global_property_s"; })(
        metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(),
                 Fundamental::StringFormat("/** test meta {} */", __LINE__)));
    rttr::registration::property_readonly("readonly_global_property_f", get_readonly_global_property_f)(
        metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(),
                 Fundamental::StringFormat("/** test meta {} */", __LINE__)));
    {
        using register_type = test_enum;
        rttr::registration::enumeration<register_type>("test_enum")(
            value("red", register_type::enum_value1), value("blue", register_type::enum_value2),

            metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(), "test enum values."));
    }
    {
        using register_type = custom_object;
        rttr::registration::class_<register_type>("test::custom_object")
            .constructor()(rttr::policy::ctor::as_object,
                           metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(),
                                    Fundamental::StringFormat("/** test meta {} */", __LINE__)))
            .constructor<int>()(metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(),
                                         Fundamental::StringFormat("/** test meta {} */", __LINE__)))
            .property("v", &register_type::v)(metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(),
                                                       Fundamental::StringFormat("/** test meta {} */", __LINE__)));
    }
    {
        using register_type = class_base;
        rttr::registration::class_<register_type>("test::class_base")
            .constructor()(rttr::policy::ctor::as_object,
                           metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(),
                                    Fundamental::StringFormat("/** test meta {} */", __LINE__)))
            .constructor<int>()(metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(),
                                         Fundamental::StringFormat("/** test meta {} */", __LINE__)))
            .constructor<int, int>()(metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(),
                                              Fundamental::StringFormat("/** test meta {} */", __LINE__)))
            .constructor<std::string>()(metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(),
                                                 Fundamental::StringFormat("/** test meta {} */", __LINE__)))
            .method("static_func",
                    &register_type::static_func)(metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(),
                                                          Fundamental::StringFormat("/** test meta {} */", __LINE__)))
            .method("static_func2",
                    &register_type::static_func2)(metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(),
                                                           Fundamental::StringFormat("/** test meta {} */", __LINE__)))
            .method("mem_func",
                    &register_type::mem_func)(metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(),
                                                       Fundamental::StringFormat("/** test meta {} */", __LINE__)))
            .method("v_mem_func",
                    &register_type::v_mem_func)(metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(),
                                                         Fundamental::StringFormat("/** test meta {} */", __LINE__)))
            .property("base_object",
                      &register_type::base_object)(metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(),
                                                            Fundamental::StringFormat("/** test meta {} */", __LINE__)))
            .property("s_base_object", &register_type::s_base_object)(
                metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(),
                         Fundamental::StringFormat("/** test meta {} */", __LINE__)))
            .property("base_object_f", &register_type::get_object_v, &register_type::set_object_v)(
                metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(),
                         Fundamental::StringFormat("/** test meta {} */", __LINE__)))
            .property("s_base_object_f", &register_type::get_static_object_v, &register_type::set_static_object_v)(
                metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(),
                         Fundamental::StringFormat("/** test meta {} */", __LINE__)))
            .property_readonly("v",
                               &register_type::v)(metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(),
                                                           Fundamental::StringFormat("/** test meta {} */", __LINE__)))
            .property_readonly("v_r", &register_type::v_r)(
                metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(),
                         Fundamental::StringFormat("/** test meta {} */", __LINE__)))
            .property_readonly("n_r", &register_type::get_object_v)(
                metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(),
                         Fundamental::StringFormat("/** test meta {} */", __LINE__)))
            .property_readonly("s_r", &register_type::get_static_object_v)(
                metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(),
                         Fundamental::StringFormat("/** test meta {} */", __LINE__)));
    }
#if 1

    {
        using register_type = class_a;
        rttr::registration::class_<register_type>("test::class_a")
            .constructor()(rttr::policy::ctor::as_object,
                           metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(),
                                    Fundamental::StringFormat("/** test meta {} */", __LINE__)))
            .method("mem_func",
                    &register_type::mem_func)(metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(),
                                                       Fundamental::StringFormat("/** test meta {} */", __LINE__)));
    }
    {
        using register_type = class_b;
        rttr::registration::class_<register_type>("test::class_b")
            .constructor()(rttr::policy::ctor::as_object,
                           metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(),
                                    Fundamental::StringFormat("/** test meta {} */", __LINE__)))
            .method("mem_func",
                    &register_type::mem_func)(metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(),
                                                       Fundamental::StringFormat("/** test meta {} */", __LINE__)))
            .method("v_mem_func",
                    &register_type::v_mem_func)(metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(),
                                                         Fundamental::StringFormat("/** test meta {} */", __LINE__)));
    }
    {
        using register_type = class_c;
        rttr::registration::class_<register_type>("test::class_c")
            .constructor()(rttr::policy::ctor::as_object,
                           metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(),
                                    Fundamental::StringFormat("/** test meta {} */", __LINE__)))
            .method("mem_func",
                    &register_type::mem_func)(metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(),
                                                       Fundamental::StringFormat("/** test meta {} */", __LINE__)))
            .method("v_mem_func",
                    &register_type::v_mem_func)(metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(),
                                                         Fundamental::StringFormat("/** test meta {} */", __LINE__)));
    }
#endif
}

void test_js_script(std::string file_name = "test.js");

int main(int argc, char** argv) {
    Fundamental::fs::SwitchToProgramDir(argv[0]);
    std::string file_name = "test.js";
    if (argc > 1) file_name = argv[1];
    test_js_script(file_name);
    return 0;
}

void test_js_script(std::string filename) {
    qjs::Runtime runtime;
    qjs::js_visitor_context rttr_context(runtime);
    rttr_context.generate_export_data = true;
    auto& context                     = rttr_context.context;
    js_std_init_handlers(runtime.rt);
    /* loader for ES6 modules */
    JS_SetModuleLoaderFunc2(runtime.rt, nullptr, js_module_loader, nullptr, nullptr);
    js_std_add_helpers(context.ctx, 0, nullptr);

    /* system modules */
    js_init_module_std(context.ctx, "std");
    js_init_module_os(context.ctx, "os");
    /* make 'std' and 'os' visible to non module code */

    context.eval(R"xxx(
        import * as std from 'std';
        import * as os from 'os';
        globalThis.std = std;
        globalThis.os = os;
    )xxx",
                 "<input>", JS_EVAL_TYPE_MODULE);

    jsscript_visitor visitor(rttr_context);
    // module test
    auto& m1 = rttr_context.access_module(jsscript_visitor::split_identifier);

    {
        m1.add("test_register_v2", "test_strb");
    }
    {
        auto& m2 = rttr_context.access_module("test_mixed_module_register");
        m2.add("test_mixed_module_register_m2", "test_mixed_module_strb");
        auto& export_data       = m2.module_prop_export_datas["test_mixed_module_register_m2"];
        export_data.name        = "test_mixed_module_register_m2";
        export_data.is_readonly = true;
        export_data.comment.comments.emplace_back("/**测试模块混合添加*/");
    }
    {
        auto& m2        = rttr_context.access_module(jsscript_visitor::split_identifier + "test");
        auto& new_class = m2.access_register_class<class_base>("class_base");
        new_class.dynamic_fun<Fundamental::script_base_type>("get_val_external", []() { return "11"; });
    }
    visitor.visit_all_valid_type();

    {
        auto file_data = rttr_context.dump();
        std::cout << file_data << std::endl;
        Fundamental::fs::WriteFile(filename + ".d.ts", file_data.data(), file_data.size());
    }
    rttr_context.final_submit();
    auto& mod2 = rttr_context.context.addModule("test2");
    mod2.class_<test_class>("test_class")
        .constructor<>()
        .constructor<int>("test_class2")
        .static_fun<&test_class::get_x>("get_x")
        .static_fun<&test_class::x>("x");
    try {
        context.eval(R"xxx(
            //import * as rttr from '::';
            import * as rttr_test from '::test';    
            //globalThis.rttr = rttr;
            globalThis.rttr_test = rttr_test;

        )xxx",
                     "<import>", JS_EVAL_TYPE_MODULE);

        auto buf = qjs::detail::readFile(filename);
        if (!buf) throw std::runtime_error { std::string { "can't read file: " } + filename };
        // autodetect file type
        auto flags = JS_DetectModule(buf->data(), buf->size()) ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL;
        std::cout << "module flags:" << flags << std::endl;
        context.eval(*buf, "<eval>", flags);

    } catch (qjs::exception) {
        auto exc = context.getException();
        std::cerr << (std::string)exc << std::endl;
        if ((bool)exc["stack"]) std::cerr << (std::string)exc["stack"] << std::endl;
    }
}
