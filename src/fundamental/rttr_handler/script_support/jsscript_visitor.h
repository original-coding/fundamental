#ifndef JSSCRIPT_VISITOR_H_
#define JSSCRIPT_VISITOR_H_

#include <rttr/enumeration.h>
#include <rttr/type.h>
#include <rttr/visitor.h>

#include <cstdint>
#include <functional>
#include <iostream>
#include <list>
#include <regex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

#include "fundamental/basic/string_utils.hpp"
#include "fundamental/rttr_handler/meta_control.h"

#include "jsscript_visitor_imp.hpp"

class jsscript_visitor;
namespace qjs
{
using js_module_t = decltype(std::declval<qjs::Context>().addModule("1"));
template <typename T>
using js_class_register_t = std::decay_t<decltype(std::declval<qjs::Context>().addModule("1").class_<T>("1"))>;

struct comment_data {
    void correct_data() {
        auto vec = std::move(comments);
        for (auto& s : vec) {
            auto tmp_vec = Fundamental::StringSplit(s, '\n');
            for (auto& s2 : tmp_vec) {
                while (true) {
                    Fundamental::StringTrimStartAndEnd(s2);
                    if (s2.empty()) break;
                    {
                        auto c = *s2.begin();
                        if (c == '/' || c == '*') {
                            s2.erase(0, 1);
                            continue;
                        }
                    }
                    {
                        auto c = *s2.rbegin();
                        if (c == '/' || c == '*') {
                            s2.resize(s2.size() - 1);
                            continue;
                        }
                    }
                    break;
                }
                if (!s2.empty()) comments.emplace_back(std::move(s2));
            }
        }
    }
    std::string dump(std::size_t depth = 0) {
        correct_data();
        std::string ret;
        std::string base = depth == 0 ? std::string("") : std::string(depth, '\t');
        if (!comments.empty()) {
            ret += base + "/**\n";
            for (auto& data : comments) {
                ret += base + " * " + data + "  \n";
            }
            ret += base + " */\n";
        }
        return ret;
    }
    std::vector<std::string> comments;
};
struct js_visitor_module_data {
    void final_submit(js_module_t module_ref, const char* class_name) {
        do_finish_submit(module_ref, class_name);
    }

    virtual void do_finish_submit(js_module_t module_ref, const char* class_name) = 0;
    std::string dump(const char* class_name, std::size_t depth) {
        if (constructor_export_datas.empty()) {
            auto& default_ctor = constructor_export_datas.emplace_back();
            default_ctor.name  = class_name;
        }
        std::string ret;
        std::string base        = depth == 0 ? std::string("") : std::string(depth, '\t');
        auto iter               = constructor_export_datas.begin();
        auto default_class_name = iter->name;
        while (iter != constructor_export_datas.end()) {
            // export class first declare
            ret += base;
            if (default_class_name == iter->name) {
                ret += "export class " + iter->name + " {\n";
            } else {
                ret += "export class " + iter->name + " extends " + default_class_name + " {\n";
            }
            // add constructors
            ret += iter->dump(depth + 1);
            ret += "\n";
            // only add prop and func for the thirst declared class
            if (iter == constructor_export_datas.begin()) {
                if (!class_func_export_datas.empty()) {
                    // add funcs
                    for (auto& f : class_func_export_datas) {
                        ret += f.second.dump(depth + 1);
                    }
                    ret += "\n";
                }
                // add props
                for (auto& p : class_prop_export_datas) {
                    ret += p.second.dump(depth + 1);
                }
            }
            // export end class declare
            ret += base + "}\n";
            ++iter;
        }
        return ret;
    }
    struct constructor_export_data {
        std::string name;
        std::size_t param_nums = 0;
        comment_data comment;
        std::string dump(std::size_t depth = 0) {
            std::string ret;
            std::string base = depth == 0 ? std::string("") : std::string(depth, '\t');
            if (!name.empty()) {
                ret += comment.dump(depth);
                ret += base + "constructor(";
                std::size_t offset = 1;
                while (offset <= param_nums) {
                    if (offset > 1) ret += ", ";
                    ret += "param" + std::to_string(offset) + ": any";
                    ++offset;
                }
                ret += ");\n";
            }
            return ret;
        }
    };
    struct class_prop_export_data {
        std::string name;
        bool is_readonly = false;
        bool is_static   = false;
        comment_data comment;
        std::string dump(std::size_t depth = 0) {
            std::string ret;
            std::string base = depth == 0 ? std::string("") : std::string(depth, '\t');
            if (!name.empty()) {
                ret += comment.dump(depth);
                ret += base;
                if (is_static) {
                    ret += "static ";
                }
                if (is_readonly) {
                    ret += "readonly ";
                }
                ret += name + ": any;\n";
            }
            return ret;
        }
    };
    struct class_func_export_data {
        std::string name;
        std::size_t param_nums = 0;
        bool has_return_value  = false;
        bool is_static         = false;
        comment_data comment;
        std::string dump(std::size_t depth = 0) {
            std::string ret;
            std::string base = depth == 0 ? std::string("") : std::string(depth, '\t');
            if (!name.empty()) {
                ret += comment.dump(depth);
                ret += base;
                if (is_static) {
                    ret += "static ";
                }
                ret += name + "(";
                std::size_t offset = 1;
                while (offset <= param_nums) {
                    if (offset > 1) ret += ", ";
                    ret += "param" + std::to_string(offset) + ": any";
                    ++offset;
                }
                ret += "): ";
                ret += has_return_value ? "any" : "void";
                ret += ";\n";
            }
            return ret;
        }
    };
    std::vector<constructor_export_data> constructor_export_datas;
    std::unordered_map<std::string, class_prop_export_data> class_prop_export_datas;
    std::unordered_map<std::string, class_func_export_data> class_func_export_datas;
};

template <typename T>
struct js_visitor_module_data_storage : js_visitor_module_data {
    void do_finish_submit(js_module_t module_ref, const char* class_name) override {
        js_class_register_t<T> ref_class = module_ref.class_<T>(class_name);
        for (auto& item : base_list) {
            item(ref_class);
        }
        base_list.clear();
        for (auto& item : ctor_list) {
            item(ref_class);
            Value& ctor = ref_class.template get_internal_ctor();
            for (auto& item : static_proto_list) {
                item.second(ctor);
            }
        }
        ctor_list.clear();
        Value& prototype = ref_class.template get_internal_prototype();

        for (auto& item : proto_list) {
            item.second(prototype);
        }
        proto_list.clear();
    }

    template <typename class_type, typename F>
    js_visitor_module_data_storage& dynamic_fun(const char* name, F&& f) {
        if constexpr (std::is_same_v<class_type, Fundamental::script_base_type>) {
            static_proto_list[name] = ([name = name, f = std::move(f)](Value& prototype) mutable {
                prototype[name] = js_magic_func_wrap::wrap(prototype.ctx, name, std::forward<F>(f));
            });
        } else {
            proto_list[name] = ([name = name, f = std::move(f)](Value& prototype) mutable {
                prototype[name] = js_magic_func_wrap::wrap_class<class_type>(prototype.ctx, name, std::forward<F>(f));
            });
        }

        return *this;
    }
    template <typename class_type, typename get_func_t>
    js_visitor_module_data_storage& dynamic_readonly_property(const char* name, const get_func_t& getter) {
        if constexpr (std::is_same_v<class_type, Fundamental::script_base_type>) {
            static_proto_list[name] = ([name = name, getter = getter](Value& prototype) mutable {
                auto ctx  = prototype.ctx;
                auto prop = JS_NewAtom(ctx, name);
                auto mask = JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE;
                int ret   = JS_DefinePropertyGetSet(ctx, prototype.v, prop, js_magic_func_wrap::wrap(ctx, name, getter),
                                                    JS_UNDEFINED, mask);

                JS_FreeAtom(ctx, prop);
                if (ret < 0) throw exception { ctx };
            });
        } else {
            proto_list[name] = ([name = name, getter = getter](Value& prototype) mutable {
                auto ctx  = prototype.ctx;
                auto prop = JS_NewAtom(ctx, name);
                auto mask = JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE;
                int ret   = JS_DefinePropertyGetSet(ctx, prototype.v, prop,
                                                    js_magic_func_wrap::wrap_class<class_type>(ctx, name, getter),
                                                    JS_UNDEFINED, mask);

                JS_FreeAtom(ctx, prop);
                if (ret < 0) throw exception { ctx };
            });
        }

        return *this;
    }

    template <typename class_type, typename get_func_t, typename set_func_t>
    js_visitor_module_data_storage& dynamic_property(const char* name,
                                                     const get_func_t& getter,
                                                     const set_func_t& setter) {
        if constexpr (std::is_same_v<class_type, Fundamental::script_base_type>) {
            static_proto_list[name] = ([name = name, getter, setter](Value& prototype) mutable {
                auto ctx  = prototype.ctx;
                auto prop = JS_NewAtom(ctx, name);
                auto mask = JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE;
                int ret   = JS_DefinePropertyGetSet(ctx, prototype.v, prop, js_magic_func_wrap::wrap(ctx, name, getter),
                                                    js_magic_func_wrap::wrap(ctx, name, setter), mask);

                JS_FreeAtom(ctx, prop);
                if (ret < 0) throw exception { ctx };
            });
        } else {
            proto_list[name] = ([name = name, getter, setter](Value& prototype) mutable {
                auto ctx  = prototype.ctx;
                auto prop = JS_NewAtom(ctx, name);
                auto mask = JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE;
                int ret   = JS_DefinePropertyGetSet(ctx, prototype.v, prop,
                                                    js_magic_func_wrap::wrap_class<class_type>(ctx, name, getter),
                                                    js_magic_func_wrap::wrap_class<class_type>(ctx, name, setter), mask);
                JS_FreeAtom(ctx, prop);
                if (ret < 0) throw exception { ctx };
            });
        }

        return *this;
    }

    template <typename... Args>
    js_visitor_module_data_storage& constructor(const char* name = nullptr) {
        ctor_list.emplace_back(
            [name = name](js_class_register_t<T>& class_ref) { class_ref.template constructor<Args...>(name); });
        return *this;
    }

    template <class B>
    js_visitor_module_data_storage& base() {
        base_list.emplace_back([](js_class_register_t<T>& class_ref) mutable { class_ref.template base<B>(); });
        return *this;
    }

    std::list<std::function<void(js_class_register_t<T>&)>> base_list;
    std::list<std::function<void(js_class_register_t<T>&)>> ctor_list;
    std::unordered_map<std::string_view, std::function<void(Value&)>> proto_list;
    std::unordered_map<std::string_view, std::function<void(Value&)>> static_proto_list;
    std::unordered_set<std::string> visited_ctor;
};
struct js_visitor_module {
    template <typename T>
    js_visitor_module_data_storage<T>& access_register_class(const std::string& name) {
        auto iter = cache_class.find(name);
        if (iter != cache_class.end()) {
            if (!iter->second) {
                throw std::runtime_error(name + "try register  after op_module finish submit action");
            }
            return *(reinterpret_cast<js_visitor_module_data_storage<T>*>(iter->second.get()));
        }
        auto new_torage_instance = std::make_shared<js_visitor_module_data_storage<T>>();
        cache_class.try_emplace(name, new_torage_instance);
        return *(reinterpret_cast<js_visitor_module_data_storage<T>*>(new_torage_instance.get()));
    }
    void finish_submit(qjs::Context& context, js_module_t ref_module) {
        if (!prop_list.empty()) {
            auto prop_value = context.newObject();
            for (auto& prop_item : prop_list) {
                prop_item(prop_value);
            }
            ref_module.add("prop", std::move(prop_value));
        }

        for (auto& item : submit_list) {
            item(ref_module);
        }
        submit_list.clear();
        for (auto& class_item : cache_class) {
            if (class_item.second) class_item.second->final_submit(ref_module, access_name_storage(class_item.first));
            class_item.second.reset();
        }
    }

    template <typename... Args>
    js_visitor_module& function(Args&&... args) {
        submit_list.emplace_back([captured_args = std::make_tuple(std::forward<Args>(args)...)](
                                     qjs::Context::Module& module_ref) mutable {
            std::apply([&module_ref](auto&&... args) { module_ref.function(std::forward<decltype(args)>(args)...); },
                       std::move(captured_args));
        });
        return *this;
    }

    template <typename... Args>
    js_visitor_module& add(Args&&... args) {
        submit_list.emplace_back(
            [captured_args = std::make_tuple(std::forward<Args>(args)...)](qjs::Context::Module& module_ref) mutable {
                std::apply([&module_ref](auto&&... args) { module_ref.add(std::forward<decltype(args)>(args)...); },
                           std::move(captured_args));
            });
        return *this;
    }

    template <typename T, typename Tp = std::decay_t<T>>
    js_visitor_module& module_property(const char* name,
                                       const std::function<T()>& getter,
                                       const std::function<void(const Tp&)>& setter = nullptr) {

        prop_list.emplace_back([name = name, getter, setter](qjs::Value& dst_v) mutable {
            auto ctx  = dst_v.ctx;
            auto prop = JS_NewAtom(ctx, name);
            auto mask = JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE;
            if (setter) {
                mask |= JS_PROP_WRITABLE;
            }
            int ret =
                JS_DefinePropertyGetSet(ctx, dst_v.v, prop, js_magic_func_wrap::wrap(ctx, name, getter),
                                        setter ? js_magic_func_wrap::wrap(ctx, name, setter) : JS_UNDEFINED, mask);
            JS_FreeAtom(ctx, prop);
            if (ret < 0) throw exception { ctx };
        });
        return *this;
    }

    const char* access_name_storage(const std::string& token) {
        auto& ret = dynamic_register_storage[token];
        if (!ret) {
            ret = new std::string(token);
        }
        return ret->c_str();
    }
    // import this content with "/// <reference path="xxx.d.ts" />"
    std::string dump(const char* module_name, std::size_t depth) {
        std::string ret;
        std::string base = depth == 0 ? std::string("") : std::string(depth, '\t');
        ret += base + "declare module \'" + module_name + "\' {\n";
        bool has_any_content = false;
        if (!enum_export_datas.empty()) {
            if (has_any_content) {
                ret += "\n";
            }
            has_any_content = true;
            // export enum values
            for (auto& e : enum_export_datas) {
                ret += e.second.dump(depth + 1);
            }
        }

        // export prop values
        if (!module_prop_export_datas.empty()) {
            if (has_any_content) {
                ret += "\n";
            }
            has_any_content = true;
            ret += base + "\texport namespace prop {\n";
            for (auto& v : module_prop_export_datas) {
                ret += v.second.dump(depth + 2);
            }
            ret += base + "\t}\n";
        }
        if (!module_func_export_datas.empty()) {
            if (has_any_content) {
                ret += "\n";
            }
            has_any_content = true;
            // export funcs
            for (auto& f : module_func_export_datas) {
                ret += f.second.dump(depth + 1);
            }
        }

        for (auto& item : cache_class) {
            if (!item.second) continue;
            if (has_any_content) {
                ret += "\n";
            }
            has_any_content = true;
            ret += item.second->dump(item.first.c_str(), depth + 1);
        }

        ret += base + "}\n";
        return ret;
    }
    struct enum_export_data {
        std::string name;
        std::vector<std::string> values;
        comment_data comment;
        std::string dump(std::size_t depth = 0) {
            std::string ret;
            std::string base = depth == 0 ? std::string("") : std::string(depth, '\t');
            if (!values.empty()) {
                ret += comment.dump(depth);
                ret += base + "export enum " + name + " {\n";
                for (std::size_t i = 0; i < values.size(); ++i) {
                    if ((i + 1) == values.size()) {
                        ret += base + "\t" + values[i] + "\n";
                    } else {
                        ret += base + "\t" + values[i] + ",\n";
                    }
                }

                ret += base + "}\n";
            }
            return ret;
        }
    };
    struct module_prop_export_data {
        std::string name;
        bool is_readonly = false;
        comment_data comment;
        std::string dump(std::size_t depth = 0) {
            std::string ret;
            std::string base = depth == 0 ? std::string("") : std::string(depth, '\t');
            if (!name.empty()) {
                ret += comment.dump(depth);
                ret += base;
                if (is_readonly) {
                    ret += "export const " + name + ": any;\n";
                } else {
                    ret += "export let " + name + ": any;\n";
                }
            }
            return ret;
        }
    };
    struct module_func_export_data {
        std::string name;
        std::size_t param_nums = 0;
        bool has_return_value  = false;
        comment_data comment;
        std::string dump(std::size_t depth = 0) {
            std::string ret;
            std::string base = depth == 0 ? std::string("") : std::string(depth, '\t');
            if (!name.empty()) {
                ret += comment.dump(depth);
                ret += base + "export function " + name + "(";
                std::size_t offset = 1;
                while (offset <= param_nums) {
                    if (offset > 1) ret += ", ";
                    ret += "param" + std::to_string(offset) + ": any";
                    ++offset;
                }
                ret += "): ";
                ret += has_return_value ? "any" : "void";
                ret += ";\n";
            }
            return ret;
        }
    };

    std::unordered_map<std::string, std::string*>& dynamic_register_storage;
    std::unordered_map<std::string, std::shared_ptr<js_visitor_module_data>> cache_class;
    std::list<std::function<void(js_module_t)>> submit_list;
    std::list<std::function<void(qjs::Value&)>> prop_list;
    std::unordered_map<std::string, enum_export_data> enum_export_datas;
    std::unordered_map<std::string, module_prop_export_data> module_prop_export_datas;
    std::unordered_map<std::string, module_func_export_data> module_func_export_datas;
};
// The quickjspp op_module registration has a static classid reference dependency. The code must be centrally
// registered, and no other op_module-related code can be inserted, as this may update the op_module id and cause
// resource access mismatches.
struct js_visitor_context {
    template <typename... Args>
    js_visitor_context(Args&&... args) : context(std::forward<Args>(args)...) {
    }
    js_visitor_module& access_module(const std::string& name) {

        auto iter = cache_modules.find(name);
        if (iter != cache_modules.end()) {
            if (!iter->second) {
                throw std::runtime_error(std::string(name) + " register after op_module finish submit action");
            }
            return *iter->second;
        }
        auto emplace_iter = cache_modules.try_emplace(
            name, std::shared_ptr<js_visitor_module>(new js_visitor_module { dynamic_register_storage }));
        return *emplace_iter.first->second;
    }

    const char* access_name_storage(const std::string& token) {
        auto& ret = dynamic_register_storage[token];
        if (!ret) {
            ret = new std::string(token);
        }
        return ret->c_str();
    }
    std::string dump(std::size_t depth = 0) {
        std::string ret;
        do {
            if (!generate_export_data) break;
            for (auto& iter : cache_modules) {
                if (!iter.second) continue;
                ret += iter.second->dump(iter.first.c_str(), depth);
                ret += "\n\n";
            }
        } while (0);
        return ret;
    }

    void final_submit() {
        for (auto& item : cache_modules) {
            if (item.second) {
                auto& module_ref = context.addModule(access_name_storage(item.first));
                item.second->finish_submit(context, module_ref);
            }
            item.second.reset();
        }
    }
    qjs::Context context;
    std::unordered_map<std::string, std::string*> dynamic_register_storage;
    std::unordered_map<std::string, std::shared_ptr<js_visitor_module>> cache_modules;
    bool generate_export_data = false;
};
} // namespace qjs
#if 1
class jsscript_visitor : public rttr::visitor {

public:
    inline static const std::string validate_identifier = "[a-zA-Z_][a-zA-Z0-9_]*";
    inline static const std::string validate_pattern_string { std::string("^") + validate_identifier +
                                                              "(::" + validate_identifier + ")*$" };
    inline static const std::string split_identifier = "::";

public:
    jsscript_visitor(qjs::js_visitor_context& context) : m_rttr_context(context), m_context(context.context) {
    }

    /////////////////////////////////////////////////////////////////////////////////////

    template <typename Derived>
    void iterate_base_classes() {
        // access class
        rttr::type t = rttr::type::template get<Derived>();
        if (!t.is_valid() || !t.is_class()) return;
        auto [module_name, type_name] = split_type_name(t.get_name().to_string(), false);
        auto& op_module               = m_rttr_context.access_module(std::string(module_name));
        op_module.access_register_class<Derived>(type_name);
    }

    template <typename Derived, typename Base_Class, typename... Base_Classes>
    void iterate_base_classes() {
        // add derived information
        rttr::type base_t = rttr::type::template get<Base_Class>();

        if (!base_t.is_valid() || !base_t.is_class()) return;
        rttr::type t = rttr::type::template get<Derived>();
        if (!t.is_valid() || !t.is_class()) return;
        auto [module_name, type_name]           = split_type_name(t.get_name().to_string(), false);
        auto& op_module                         = m_rttr_context.access_module(std::string(module_name));
        auto& register_class_instance           = op_module.access_register_class<Derived>(type_name);
        auto [base_module_name, base_type_name] = split_type_name(base_t.get_name().to_string(), false);
        auto& base_module                       = m_rttr_context.access_module(base_module_name);
        auto& base_register_class_instance      = base_module.access_register_class<Base_Class>(base_type_name);
        {
            auto& base_proto_list_ref = base_register_class_instance.proto_list;
            auto& proto_list_ref      = register_class_instance.proto_list;
            for (auto& item : base_proto_list_ref) {
                proto_list_ref[item.first] = item.second;
            }
        }
        {
            auto& base_proto_list_ref = base_register_class_instance.static_proto_list;
            auto& proto_list_ref      = register_class_instance.static_proto_list;
            for (auto& item : base_proto_list_ref) {
                proto_list_ref[item.first] = item.second;
            }
        }
        if (m_rttr_context.generate_export_data) {
            // we should copy all properties and functions export data
            { // copy properties
                auto& base_properties = base_register_class_instance.class_prop_export_datas;
                auto& dst_properties  = register_class_instance.class_prop_export_datas;
                for (auto& p : base_properties) {
                    dst_properties[p.first] = p.second;
                }
            }
            { // copy functions
                auto& base_functions = base_register_class_instance.class_func_export_datas;
                auto& dst_functions  = register_class_instance.class_func_export_datas;
                for (auto& f : base_functions) {
                    dst_functions[f.first] = f.second;
                }
            }
        }
        register_class_instance.template base<Base_Class>();
        iterate_base_classes<Derived, Base_Classes...>();
    }

    /////////////////////////////////////////////////////////////////////////////////////

    template <typename T, typename... Base_Classes>
    void visit_type_begin(const type_info<T>& info) {
        using declaring_type_t = typename type_info<T>::declaring_type;
        if constexpr (std::is_enum_v<declaring_type_t>) {
            visit_enum<declaring_type_t>();
        } else if constexpr (std::is_class_v<declaring_type_t>) {
            iterate_base_classes<declaring_type_t, Base_Classes...>();
        }
    }
    /////////////////////////////////////////////////////////////////////////////////////
    template <typename T, typename... Ctor_Args>
    void visit_constructor(const constructor_info<T>& info) {
        constexpr size_t ctor_arg_count = sizeof...(Ctor_Args);
        using declaring_type_t          = typename constructor_info<T>::declaring_type;
        rttr::type t                    = rttr::type::template get<declaring_type_t>();
        if (!t.is_valid() || !t.is_class()) return;
        auto [module_name, type_name] = split_type_name(t.get_name().to_string(), false);

        auto& op_module               = m_rttr_context.access_module(std::string(module_name));
        auto& register_class_instance = op_module.access_register_class<declaring_type_t>(std::string(type_name));
        auto& visited_dic             = register_class_instance.visited_ctor;
        auto ctor_signature           = info.ctor_item.get_signature().to_string();
        if (!visited_dic.insert(ctor_signature).second) return;
        auto ctor_name = type_name;
        if (visited_dic.size() > 1) {
            ctor_name += std::to_string(visited_dic.size());
        }
        if (m_rttr_context.generate_export_data) {
            auto& op_instance   = register_class_instance.constructor_export_datas;
            auto& new_item      = op_instance.emplace_back();
            new_item.name       = ctor_name;
            new_item.param_nums = ctor_arg_count;
            new_item.comment.comments.clear();
            new_item.comment.comments.emplace_back(std::string("signature:  ") +
                                                   info.ctor_item.get_signature().to_string());
            rttr::variant comment_value =
                info.ctor_item.get_metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey());
            if (comment_value.is_valid()) {
                auto comment_str = comment_value.get_wrapped_value<std::string>();
                new_item.comment.comments.emplace_back(comment_str);
            }
        }
        register_class_instance.template constructor<Ctor_Args...>(m_rttr_context.access_name_storage(ctor_name));
    }
    /////////////////////////////////////////////////////////////////////////////////////

    template <typename T>
    void visit_global_method(const method_info<T>& info) {
        const auto& method_info_name  = info.method_item.get_name().to_string();
        auto [module_name, type_name] = split_type_name(method_info_name, false);
        auto& op_module               = m_rttr_context.access_module(std::string(module_name));
        if (m_rttr_context.generate_export_data) {
            auto& op_instance         = op_module.module_func_export_datas;
            auto& new_item            = op_instance[std::string(type_name)];
            new_item.name             = std::string(type_name);
            new_item.has_return_value = info.method_item.get_return_type() != rttr::type::get<void>();
            new_item.param_nums       = info.method_item.get_parameter_infos().size();
            new_item.comment.comments.clear();
            new_item.comment.comments.emplace_back(std::string("signature: ") +
                                                   info.method_item.get_return_type().get_name().to_string() + " " +
                                                   info.method_item.get_signature().to_string());
            rttr::variant comment_value =
                info.method_item.get_metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey());
            if (comment_value.is_valid()) {
                auto comment_str = comment_value.get_wrapped_value<std::string>();
                new_item.comment.comments.emplace_back(comment_str);
            }
        }
        op_module.function(m_rttr_context.access_name_storage(type_name), info.function_ptr);
    }

    /////////////////////////////////////////////////////////////////////////////////////

    template <typename T>
    void visit_method(const method_info<T>& info) {
        using declaring_type_t = typename method_info<T>::declaring_type;
        rttr::type t           = rttr::type::template get<declaring_type_t>();
        if (!t.is_valid() || !t.is_class()) return;
        auto [module_name, type_name]  = split_type_name(t.get_name().to_string(), false);
        auto& op_module                = m_rttr_context.access_module(std::string(module_name));
        auto& register_class_instance  = op_module.access_register_class<declaring_type_t>(std::string(type_name));
        using function_type            = decltype(info.function_ptr);
        const auto& register_info_name = info.method_item.get_name().to_string();

        if (m_rttr_context.generate_export_data) {
            auto& op_instance         = register_class_instance.class_func_export_datas;
            auto& new_item            = op_instance[register_info_name];
            new_item.name             = register_info_name;
            new_item.has_return_value = info.method_item.get_return_type() != rttr::type::get<void>();
            new_item.is_static        = info.method_item.is_static();
            new_item.param_nums       = info.method_item.get_parameter_infos().size();
            new_item.comment.comments.clear();
            new_item.comment.comments.emplace_back(std::string("signature: ") +
                                                   info.method_item.get_return_type().get_name().to_string() + " " +
                                                   info.method_item.get_signature().to_string());
            rttr::variant comment_value =
                info.method_item.get_metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey());
            if (comment_value.is_valid()) {
                auto comment_str = comment_value.get_wrapped_value<std::string>();
                new_item.comment.comments.emplace_back(comment_str);
            }
        }
        if constexpr (std::is_member_pointer_v<function_type>) {
            register_class_instance.template dynamic_fun<declaring_type_t>(
                m_rttr_context.access_name_storage(register_info_name),
                qjs::make_js_member_callable(info.function_ptr));
        } else {
            register_class_instance.template dynamic_fun<Fundamental::script_base_type>(
                m_rttr_context.access_name_storage(register_info_name), info.function_ptr);
        }
    }

    /////////////////////////////////////////////////////////////////////////////////////
    template <typename T>
    void visit_global_property(const property_info<T>& info) {

        const auto& property_info_name = info.property_item.get_name().to_string();
        auto [module_name, type_name]  = split_type_name(property_info_name, false);
        auto& op_module                = m_rttr_context.access_module(std::string(module_name));
        if (m_rttr_context.generate_export_data) {
            auto& op_instance    = op_module.module_prop_export_datas;
            auto& new_item       = op_instance[property_info_name];
            new_item.name        = property_info_name;
            new_item.is_readonly = info.property_item.is_readonly();
            new_item.comment.comments.clear();
            new_item.comment.comments.emplace_back(std::string("type: ") +
                                                   info.property_item.get_type().get_name().to_string());
            rttr::variant comment_value =
                info.property_item.get_metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey());
            if (comment_value.is_valid()) {
                auto comment_str = comment_value.get_wrapped_value<std::string>();
                new_item.comment.comments.emplace_back(comment_str);
            }
        }
        using prop_type = decltype(*info.property_accessor);
        using raw_type  = std::decay_t<prop_type>;
        op_module.module_property<prop_type, raw_type>(
            m_rttr_context.access_name_storage(type_name),
            [acc = info.property_accessor]() -> prop_type { return *acc; },
            [acc = info.property_accessor](const raw_type& v) { *acc = v; });
    }

    template <typename T>
    void visit_property(const property_info<T>& info) {
        using declaring_type_t = typename property_info<T>::declaring_type;
        rttr::type t           = rttr::type::template get<declaring_type_t>();
        if (!t.is_valid() || !t.is_class()) return;
        auto name                      = t.get_name().to_string();
        const auto& property_info_name = info.property_item.get_name().to_string();
        auto [module_name, type_name]  = split_type_name(name, false);
        auto& op_module                = m_rttr_context.access_module(std::string(module_name));
        auto& register_class_instance  = op_module.access_register_class<declaring_type_t>(std::string(type_name));
        if (m_rttr_context.generate_export_data) {
            auto& op_instance    = register_class_instance.class_prop_export_datas;
            auto& new_item       = op_instance[property_info_name];
            new_item.name        = property_info_name;
            new_item.is_readonly = info.property_item.is_readonly();
            new_item.is_static   = info.property_item.is_static();
            new_item.comment.comments.clear();
            new_item.comment.comments.emplace_back(std::string("type: ") +
                                                   info.property_item.get_type().get_name().to_string());
            rttr::variant comment_value =
                info.property_item.get_metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey());
            if (comment_value.is_valid()) {
                auto comment_str = comment_value.get_wrapped_value<std::string>();
                new_item.comment.comments.emplace_back(comment_str);
            }
        }
        using prop_type = decltype(info.property_accessor);
        if constexpr (std::is_member_pointer_v<prop_type>) { // member func
            using value_type = typename Fundamental::member_pointer_traits<prop_type>::member_type;
            register_class_instance.template dynamic_property<declaring_type_t>(
                m_rttr_context.access_name_storage(property_info_name),
                [getter = info.property_accessor](std::shared_ptr<declaring_type_t> instance) mutable
                    -> decltype(auto) { return instance.get()->*getter; },
                [setter = info.property_accessor](std::shared_ptr<declaring_type_t> instance,
                                                  const value_type& v) mutable { instance.get()->*setter = v; });
        } else { // static func
            using value_type = std::decay_t<decltype(*info.property_accessor)>;
            register_class_instance.template dynamic_property<Fundamental::script_base_type>(
                m_rttr_context.access_name_storage(property_info_name),
                [getter = info.property_accessor]() mutable -> decltype(auto) { return *getter; },
                [setter = info.property_accessor](const value_type& v) mutable { *setter = v; });
        }
    }

    template <typename T>
    void visit_global_getter_setter_property(const property_getter_setter_info<T>& info) {
        const auto& property_info_name = info.property_item.get_name().to_string();
        auto [module_name, type_name]  = split_type_name(property_info_name, false);
        auto& op_module                = m_rttr_context.access_module(std::string(module_name));
        if (m_rttr_context.generate_export_data) {
            auto& op_instance    = op_module.module_prop_export_datas;
            auto& new_item       = op_instance[property_info_name];
            new_item.name        = property_info_name;
            new_item.is_readonly = false;
            new_item.comment.comments.clear();
            new_item.comment.comments.emplace_back(std::string("type: ") +
                                                   info.property_item.get_type().get_name().to_string());
            rttr::variant comment_value =
                info.property_item.get_metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey());
            if (comment_value.is_valid()) {
                auto comment_str = comment_value.get_wrapped_value<std::string>();
                new_item.comment.comments.emplace_back(comment_str);
            }
        }
        using property_type = decltype(info.property_getter());
        using raw_type      = std::decay_t<property_type>;
        op_module.module_property<property_type, raw_type>(
            m_rttr_context.access_name_storage(type_name),
            [acc = info.property_getter]() -> property_type { return acc(); },
            [acc = info.property_setter](const raw_type& v) mutable { acc(v); });
    }

    template <typename T>
    void visit_getter_setter_property(const property_getter_setter_info<T>& info) {
        using declaring_type_t = typename property_getter_setter_info<T>::declaring_type;
        rttr::type t           = rttr::type::template get<declaring_type_t>();
        if (!t.is_valid() || !t.is_class()) return;
        auto name                      = t.get_name().to_string();
        const auto& property_info_name = info.property_item.get_name().to_string();
        auto [module_name, type_name]  = split_type_name(name, false);
        auto& op_module                = m_rttr_context.access_module(std::string(module_name));
        auto& register_class_instance  = op_module.access_register_class<declaring_type_t>(std::string(type_name));
        if (m_rttr_context.generate_export_data) {
            auto& op_instance    = register_class_instance.class_prop_export_datas;
            auto& new_item       = op_instance[property_info_name];
            new_item.name        = property_info_name;
            new_item.is_readonly = false;
            new_item.is_static   = info.property_item.is_static();
            new_item.comment.comments.clear();
            new_item.comment.comments.emplace_back(std::string("type: ") +
                                                   info.property_item.get_type().get_name().to_string());
            rttr::variant comment_value =
                info.property_item.get_metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey());
            if (comment_value.is_valid()) {
                auto comment_str = comment_value.get_wrapped_value<std::string>();
                new_item.comment.comments.emplace_back(comment_str);
            }
        }
        using prop_type = decltype(info.property_getter);
        if constexpr (std::is_member_pointer_v<prop_type>) { // member func
            register_class_instance.template dynamic_property<declaring_type_t>(
                m_rttr_context.access_name_storage(property_info_name),
                qjs::make_js_member_callable(info.property_getter), qjs::make_js_member_callable(info.property_setter));
        } else { // static func
            register_class_instance.template dynamic_property<Fundamental::script_base_type>(
                m_rttr_context.access_name_storage(property_info_name), info.property_getter, info.property_setter);
        }
    }
    template <typename T>
    void visit_global_readonly_property(const property_info<T>& info) {
        const auto& property_info_name = info.property_item.get_name().to_string();
        auto [module_name, type_name]  = split_type_name(property_info_name, false);
        auto& op_module                = m_rttr_context.access_module(std::string(module_name));
        if (m_rttr_context.generate_export_data) {
            auto& op_instance    = op_module.module_prop_export_datas;
            auto& new_item       = op_instance[property_info_name];
            new_item.name        = property_info_name;
            new_item.is_readonly = true;
            new_item.comment.comments.clear();
            new_item.comment.comments.emplace_back(std::string("type: ") +
                                                   info.property_item.get_type().get_name().to_string());
            rttr::variant comment_value =
                info.property_item.get_metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey());
            if (comment_value.is_valid()) {
                auto comment_str = comment_value.get_wrapped_value<std::string>();
                new_item.comment.comments.emplace_back(comment_str);
            }
        }
        using ACC_TYPE = decltype(info.property_accessor);
        if constexpr (std::is_pointer_v<ACC_TYPE>) {
            // function object
            if constexpr (std::is_function_v<std::remove_pointer_t<ACC_TYPE>>) {
                using value_type = decltype(info.property_accessor());

                op_module.module_property<value_type>(m_rttr_context.access_name_storage(type_name),
                                                      [acc = info.property_accessor]() -> value_type { return acc(); });
            } else {
                using value_type = decltype(*info.property_accessor);
                op_module.module_property<value_type>(m_rttr_context.access_name_storage(type_name),
                                                      [acc = info.property_accessor]() -> value_type { return *acc; });
            }

        } else { // lambda
            using value_type = decltype(info.property_accessor());
            op_module.module_property<value_type>(
                m_rttr_context.access_name_storage(type_name),
                [acc = info.property_accessor]() mutable -> value_type { return acc(); });
        }
    }
    template <typename T>
    void visit_readonly_property(const property_info<T>& info) {
        using declaring_type_t = typename property_info<T>::declaring_type;
        rttr::type t           = rttr::type::get<declaring_type_t>();
        if (!t.is_valid() || !t.is_class()) return;
        auto name                      = t.get_name().to_string();
        const auto& property_info_name = info.property_item.get_name().to_string();
        auto [module_name, type_name]  = split_type_name(name, false);
        auto& op_module                = m_rttr_context.access_module(std::string(module_name));
        auto& register_class_instance  = op_module.access_register_class<declaring_type_t>(std::string(type_name));
        using prop_type                = decltype(info.property_accessor);
        if (m_rttr_context.generate_export_data) {
            auto& op_instance    = register_class_instance.class_prop_export_datas;
            auto& new_item       = op_instance[property_info_name];
            new_item.name        = property_info_name;
            new_item.is_readonly = true;
            new_item.is_static   = info.property_item.is_static();
            new_item.comment.comments.clear();
            new_item.comment.comments.emplace_back(std::string("type: ") +
                                                   info.property_item.get_type().get_name().to_string());
            rttr::variant comment_value =
                info.property_item.get_metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey());
            if (comment_value.is_valid()) {
                auto comment_str = comment_value.get_wrapped_value<std::string>();
                new_item.comment.comments.emplace_back(comment_str);
            }
        }

        if constexpr (std::is_member_pointer_v<prop_type>) {
            if constexpr (std::is_member_object_pointer_v<prop_type>) { // member
                using value_type = typename Fundamental::member_pointer_traits<prop_type>::member_type;
                register_class_instance.template dynamic_readonly_property<declaring_type_t>(
                    m_rttr_context.access_name_storage(property_info_name),
                    [getter = info.property_accessor](std::shared_ptr<declaring_type_t> instance) mutable
                        -> const value_type& { return instance.get()->*getter; });
            } else { // function
                register_class_instance.template dynamic_readonly_property<declaring_type_t>(
                    m_rttr_context.access_name_storage(property_info_name),
                    qjs::make_js_member_callable(info.property_accessor));
            }
        } else {
            if constexpr (std::is_function_v<std::remove_pointer_t<prop_type>>) { // static function
                register_class_instance.template dynamic_readonly_property<Fundamental::script_base_type>(
                    m_rttr_context.access_name_storage(property_info_name),
                    [acc = info.property_accessor]() -> decltype(info.property_accessor()) { return acc(); });
            } else { // static member
                using value_type = std::decay_t<std::remove_pointer_t<prop_type>>;
                register_class_instance.template dynamic_readonly_property<Fundamental::script_base_type>(
                    m_rttr_context.access_name_storage(property_info_name),
                    [acc = info.property_accessor]() -> const value_type& { return *acc; });
            }
        }
    }

    /////////////////////////////////////////////////////////////////////////////////////
    // register enum as a op_module value
    template <typename T, typename = std::enable_if_t<std::is_enum_v<T>>>
    void visit_enum() {

        rttr::type t = rttr::type::template get<T>();
        if (!t.is_valid() || !t.is_enumeration()) return;
        auto [module_name, type_name] = split_type_name(t.get_name().to_string(), false);
        auto& op_module               = m_rttr_context.access_module(std::string(module_name));
        auto new_enum_value           = m_context.newObject();
        {
            auto enum_value = t.get_enumeration();
            auto all_values = enum_value.get_names();
            if (m_rttr_context.generate_export_data) {
                auto& new_enum = op_module.enum_export_datas[type_name];
                new_enum.name  = type_name;
                new_enum.values.clear();
                new_enum.comment.comments.clear();
                new_enum.comment.comments.emplace_back(std::string("name: ") + t.get_name().to_string());
                new_enum.comment.comments.emplace_back(std::string("underlying type: ") +
                                                       enum_value.get_underlying_type().get_name().to_string());
                rttr::variant comment_value =
                    enum_value.get_metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey());
                if (comment_value.is_valid()) {
                    new_enum.comment.comments.emplace_back(comment_value.get_wrapped_value<std::string>());
                }

                for (const auto& value_name : all_values) {
                    new_enum.values.emplace_back(value_name.to_string());
                }
            }
            for (const auto& value_name : all_values) {
                auto value = enum_value.name_to_value(value_name).get_value<T>();
                new_enum_value[m_rttr_context.access_name_storage(value_name.to_string())] = value;
            }
        }
        op_module.add(m_rttr_context.access_name_storage(type_name), std::move(new_enum_value));
    }
    static bool is_valied_name_identifier(const std::string& str) {

        return std::regex_match(str, std::regex { validate_pattern_string });
    }
    void visit_all_valid_type() {
        auto all_types = rttr::type::get_types();
        for (const auto& current_type : all_types) {
            std::string name = current_type.get_name().to_string();
            // don't register basic type
            if (!is_valied_name_identifier(name) || current_type.is_arithmetic()) {
                continue;
            }
            visit(current_type);
        }
        auto all_global_methods = rttr::type::get_global_methods();
        for (const auto& current_method : all_global_methods) {
            visit(current_method);
        }
        auto all_global_properties = rttr::type::get_global_properties();
        for (const auto& current_property : all_global_properties) {
            visit(current_property);
        }
    }

private:
    static std::tuple<std::string, std::string> split_type_name(std::string name, bool check_pattern = true) {
        if (check_pattern) {
            if (!is_valied_name_identifier(name)) {
                throw std::invalid_argument(std::string("register name should match the pattern ") +
                                            validate_pattern_string);
            }
        }
        size_t pos = name.rfind(split_identifier);

        if (pos == std::string::npos) {
            return { split_identifier, std::move(name) };
        }
        std::string before = name.substr(0, pos);
        std::string after  = name.substr(pos + 2);
        return { split_identifier + before, after };
    }

private:
    qjs::js_visitor_context& m_rttr_context;
    qjs::Context& m_context;

    RTTR_ENABLE(visitor) // Important!! Otherwise the object instance cannot be casted from "visitor" to
                         // "js_script_binding_visitor"
};

RTTR_REGISTER_VISITOR(jsscript_visitor); // Important!!
                                         // In order to make the visitor available during registration
                                         // JSSCRIPT_VISITOR_H_
#endif
#endif