

#include "fundamental/basic/buffer.hpp"
#include "fundamental/basic/log.h"
#include "fundamental/basic/random_generator.hpp"
#include "fundamental/basic/utils.hpp"
#include "fundamental/rttr_handler/binary_packer.h"

#include "fundamental/rttr_handler/deserializer.h"
#include "fundamental/rttr_handler/serializer.h"

#include <iostream>
#include <list>
#include <map>
#include <nlohmann/json.hpp>
#include <rttr/registration>
#include <rttr/type>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>
using namespace rttr;

static nlohmann::json json_from_string(const std::string& str, bool& ok) {
    try {
        ok = true;
        return nlohmann::json::parse(str);
    } catch (const std::exception& e) {
        ok = false;
    }
    return {};
}

static std::string json_to_string(const nlohmann::json& object, bool& ok) {
    ok = true;
    return object.dump(4);
}

enum class color
{
    red,
    green,
    blue
};

enum class unregister_color
{
    n_red,
    n_green,
    n_blue
};

struct point2d {
    point2d() {
    }
    point2d(int x_, int y_) : x(x_), y(y_) {
    }
    bool operator==(const point2d& other) const noexcept {
        return other.x == x && other.y == y;
    }
    bool operator!=(const point2d& other) const noexcept {
        return other.x != x || other.y != y;
    }
    int x = 0;
    int y = 0;
};
struct ComplexS1 {
    virtual ~ComplexS1() {
    }
    float z;
    std::string s;
    RTTR_ENABLE()
};

struct ComplexS : ComplexS1 {
    color c = color::red;
    point2d point;
    std::vector<point2d> ponits;
    RTTR_ENABLE(ComplexS1)
};

struct ComplexSS : ComplexS {
    std::set<int> dic;
    nlohmann::json object;

    RTTR_ENABLE(ComplexS)
};

struct CustomObject {
    CustomObject() = default;
    std::vector<color> cs;
    std::vector<nlohmann::json> objects;
    std::list<int> is;

    bool operator==(const CustomObject& other) const noexcept {
        return cs == other.cs && objects == other.objects && is == other.is;
    }
    bool operator!=(const CustomObject& other) const noexcept {
        return cs != other.cs || objects != other.objects || is != other.is;
    }
};

struct CustomObject2 {
    CustomObject2() = default;
    std::list<int> is;
    bool operator==(const CustomObject& other) const noexcept {
        return is == other.is;
    }
    bool operator!=(const CustomObject& other) const noexcept {
        return is != other.is;
    }
};

struct TestContainerEle {
    TestContainerEle() = default;
    int x;
    int y;
    bool operator==(const TestContainerEle& other) const noexcept {
        return other.x == x && other.y == y;
    }
    bool operator!=(const TestContainerEle& other) const noexcept {
        return !operator==(other);
    }
    bool operator>(const TestContainerEle& other) const noexcept {
        return x > other.x;
    }
    bool operator<(const TestContainerEle& other) const noexcept {
        return x < other.x;
    }
};

struct TestContainer {
    TestContainer() = default;
    TestContainerEle obj;

    std::vector<std::string> empty_v;
    std::vector<TestContainerEle> no_empty_v;
    std::vector<std::vector<TestContainerEle>> objects_v;
    std::set<std::string> empty_set;
    std::set<TestContainerEle> objects_set;
    std::map<std::string, std::string> empty_map;
    std::map<TestContainerEle, std::set<TestContainerEle>> no_empty_map;
    std::map<TestContainerEle, std::map<TestContainerEle, std::set<TestContainerEle>>> no_empty_map2;
    void update() {
        auto g = Fundamental::DefaultNumberGenerator<int>();
        obj.x  = g();
        obj.y  = g();
        no_empty_v.push_back(obj);
        objects_v.push_back(no_empty_v);
        objects_set.insert(obj);
        no_empty_map[obj]  = objects_set;
        no_empty_map2[obj] = no_empty_map;
    }
    bool operator==(const TestContainer& other) const noexcept {
        return obj == other.obj && empty_v == other.empty_v && no_empty_v == other.no_empty_v &&
               empty_set == other.empty_set && objects_set == other.objects_set && empty_map == other.empty_map &&
               no_empty_map == other.no_empty_map && no_empty_map2 == other.no_empty_map2;
    }
};

struct TestVarObject {
    TestVarObject()  = default;
    bool v1          = false;
    std::int8_t v2   = 1;
    std::uint8_t v3  = 2;
    std::int16_t v4  = 3;
    std::uint16_t v5 = 4;
    std::int32_t v6  = 5;
    std::uint32_t v7 = 6;
    std::int64_t v8  = 7;
    std::uint64_t v9 = 8;
    std::string v10  = "1";
    float v11        = 0.1f;
    double v12       = 0.1;
    TestVarObject& update() {
        v1 = !v1;
        v2 += 1;
        v3 += 1;
        v4 += 1;
        v5 += 1;
        v6 += 1;
        v7 += 1;
        v8 += 1;
        v9 += 1;
        v10.push_back('1');
        v11 += 0.1f;
        v12 += 0.1;
        return *this;
    }
    bool operator==(const TestVarObject& other) const noexcept {
        return v1 == other.v1 && v2 == other.v2 && v3 == other.v3 && v4 == other.v4 && v5 == other.v5 &&
               v6 == other.v6 && v7 == other.v7 && v8 == other.v8 && v9 == other.v9 && v10 == other.v10 &&
               v11 == other.v11 && v12 == other.v12;
    }
    bool operator!=(const TestVarObject& other) const noexcept {
        return !(operator==(other));
    }
    bool operator>(const TestVarObject& other) const noexcept {
        return v2 > other.v2;
    }
    bool operator>=(const TestVarObject& other) const noexcept {
        return v2 >= other.v2;
    }
    bool operator<(const TestVarObject& other) const noexcept {
        return v2 < other.v2;
    }
    bool operator<=(const TestVarObject& other) const noexcept {
        return v2 <= other.v2;
    }
};

struct TestVarObject2 {
    TestVarObject2() = default;
    TestVarObject ob1;
    std::vector<TestVarObject> obj2;
    std::set<TestVarObject> obj3;
    std::map<TestVarObject, int> obj4;
    TestVarObject ob5;
    bool operator==(const TestVarObject2& other) const noexcept {
        return ob1 == other.ob1 && obj2 == other.obj2 && obj3 == other.obj3 && obj4 == other.obj4 && ob5 == other.ob5;
    }
};

struct TestExtraObject {
    TestExtraObject() = default;
    std::vector<std::string> ss;
    std::vector<color> cc;
    std::set<std::string> sss;
    std::map<std::string, std::string> mm;
    bool operator==(const TestExtraObject& other) const noexcept {
        return ss == other.ss && cc == other.cc && sss == other.sss && mm == other.mm;
    }
};

RTTR_REGISTRATION {
    rttr::registration::class_<point2d>("point2d")
        .constructor()(rttr::policy::ctor::as_object)
        .property("x", &point2d::x)
        .property("y", &point2d::y);
    type::register_converter_func(json_from_string);
    type::register_converter_func(json_to_string);
    rttr::registration::enumeration<color>("color")(value("red", color::red), value("blue", color::blue),
                                                    value("green", color::green));
    {
        using register_type = ComplexS1;
        rttr::registration::class_<register_type>("ComplexS1")
            .constructor()(rttr::policy::ctor::as_object)
            .property("s", &register_type::s)
            .property("z", &register_type::z);
    }
    {
        using register_type = ComplexS;
        rttr::registration::class_<register_type>("ComplexS")
            .constructor()(rttr::policy::ctor::as_object)
            .property("c", &register_type::c)
            .property("point", &register_type::point)
            .property("ponits", &register_type::ponits);
    }
    {
        using register_type = ComplexSS;
        rttr::registration::class_<register_type>("ComplexSS")
            .constructor()(rttr::policy::ctor::as_object)
            .property("dic", &register_type::dic)
            .property("object", &register_type::object);
    }
    {
        using register_type = CustomObject;
        rttr::registration::class_<register_type>("CustomObject")
            .constructor()(rttr::policy::ctor::as_object)
            .property("cs", &register_type::cs)
            .property("is", &register_type::is)
            .property("objects", &register_type::objects);
    }
    {
        using register_type = CustomObject2;
        rttr::registration::class_<register_type>("CustomObject2")
            .constructor()(rttr::policy::ctor::as_object)
            .property("is", &register_type::is);
    }
    {
        using register_type = TestVarObject;
        rttr::registration::class_<register_type>("TestVarObject")
            .constructor()(rttr::policy::ctor::as_object)
            .property("1", &register_type::v1)
            .property("2", &register_type::v2)
            .property("3", &register_type::v3)
            .property("4", &register_type::v4)
            .property("5", &register_type::v5)
            .property("6", &register_type::v6)
            .property("7", &register_type::v7)
            .property("8", &register_type::v8)
            .property("9", &register_type::v9)
            .property("a", &register_type::v10)
            .property("b", &register_type::v11)
            .property("c", &register_type::v12);
    }
    {
        using register_type = TestVarObject2;
        rttr::registration::class_<register_type>("TestVarObject2")
            .constructor()(rttr::policy::ctor::as_object)
            .property("1", &register_type::ob1)
            .property("2", &register_type::obj2)
            .property("3", &register_type::obj3)
            .property("4", &register_type::obj4)
            .property("5", &register_type::ob5);
    }

    {
        using register_type = TestContainerEle;
        rttr::registration::class_<register_type>("TestContainerEle")
            .constructor()(rttr::policy::ctor::as_object)
            .property("x", &register_type::x)
            .property("y", &register_type::y);
    }

    {
        using register_type = TestContainer;
        rttr::registration::class_<register_type>("TestContainer")
            .constructor()(rttr::policy::ctor::as_object)
            .property("1", &register_type::obj)
            .property("2", &register_type::empty_v)
            .property("3", &register_type::no_empty_v)

            .property("4", &register_type::objects_v)

            .property("5", &register_type::empty_set)
            .property("6", &register_type::objects_set)
            .property("7", &register_type::empty_map)
            .property("8", &register_type::no_empty_map)
            .property("9", &register_type::no_empty_map2);
    }
    {
        using register_type = TestExtraObject;
        rttr::registration::class_<register_type>("TestExtraObject")
            .constructor()(rttr::policy::ctor::as_object)
            .property("ss", &register_type::ss)
            .property("cc", &register_type::cc)
            .property("mm", &register_type::mm)
            .property("sss", &register_type::sss);
    }
}

void test_normal_packer();
int main(int argc, char* argv[]) {
    test_normal_packer();
    using namespace Fundamental::io;
    auto type = rttr::type::get<std::set<TestContainerEle>>();

    constructor ctor = type.get_constructor();
    auto ctors       = type.get_constructors();
    for (auto& item : ctors) {
        if (item.get_instantiated_type() == type) ctor = item;
    }
    auto v = type.create();

    {
        int cnt = 0;

        TestContainer obj;
        while (cnt < 20) {
            obj.update();
            auto data = binary_pack(obj);
            TestContainer tmp;
            binary_unpack(data.data(), data.size(), tmp, true, 0);
            FASSERT(tmp == obj);
            ++cnt;
        }
    }
    TestVarObject basic_object;
    std::int32_t cnt = 5;
    TestVarObject2 obj;
    while (cnt > 0) {
        --cnt;
        obj.ob1 = basic_object.update();
        obj.obj2.push_back(basic_object.update());
        obj.obj3.insert(basic_object.update());
        obj.obj4.emplace(basic_object.update(), cnt);
        obj.ob5   = basic_object.update();
        auto data = binary_pack(obj);
        FINFOS << "TestVarObject2 gen buf:" << Fundamental::Utils::BufferToHex(data.data(), data.size());
        TestVarObject2 obj2;

        FINFOS << binary_unpack(data.data(), data.size(), obj2, true, 0);
        FASSERT(obj == obj2);
        data = binary_pack(obj2.ob1);
        binary_pack(data, obj2.obj2);
        binary_pack(data, obj2.obj3);
        binary_pack(data, obj2.obj4);
        binary_pack(data, obj2.ob5);
        FINFOS << "TestVarObject2 gen buf 2:" << Fundamental::Utils::BufferToHex(data.data(), data.size());
        TestVarObject2 obj3;
        FINFOS << binary_unpack(data.data(), data.size(), obj3.ob1, true, 0);
        FINFOS << binary_unpack(data.data(), data.size(), obj3.obj2, true, 1);
        FINFOS << binary_unpack(data.data(), data.size(), obj3.obj3, true, 2);
        FINFOS << binary_unpack(data.data(), data.size(), obj3.obj4, true, 3);
        FINFOS << binary_unpack(data.data(), data.size(), obj3.ob5, true, 4);
        FASSERT(obj == obj3);
        data = binary_pack(basic_object.v1);
        binary_pack(data, basic_object.v2);
        binary_pack(data, basic_object.v3);
        binary_pack(data, basic_object.v4);
        binary_pack(data, basic_object.v5);
        binary_pack(data, basic_object.v6);
        binary_pack(data, basic_object.v7);
        binary_pack(data, basic_object.v8);
        binary_pack(data, basic_object.v9);
        binary_pack(data, basic_object.v10);
        binary_pack(data, basic_object.v11);
        binary_pack(data, basic_object.v12);
        FINFOS << "TestVarObject gen buf :" << Fundamental::Utils::BufferToHex(data.data(), data.size());
        TestVarObject new_object;
        std::size_t offset = 0;
        FINFOS << binary_unpack(data.data(), data.size(), new_object.v1, true, offset++);
        FINFOS << binary_unpack(data.data(), data.size(), new_object.v2, true, offset++);
        FINFOS << binary_unpack(data.data(), data.size(), new_object.v3, true, offset++);
        FINFOS << binary_unpack(data.data(), data.size(), new_object.v4, true, offset++);
        FINFOS << binary_unpack(data.data(), data.size(), new_object.v5, true, offset++);
        FINFOS << binary_unpack(data.data(), data.size(), new_object.v6, true, offset++);
        FINFOS << binary_unpack(data.data(), data.size(), new_object.v7, true, offset++);
        FINFOS << binary_unpack(data.data(), data.size(), new_object.v8, true, offset++);
        FINFOS << binary_unpack(data.data(), data.size(), new_object.v9, true, offset++);
        FINFOS << binary_unpack(data.data(), data.size(), new_object.v10, true, offset++);
        FINFOS << binary_unpack(data.data(), data.size(), new_object.v11, true, offset++);
        FINFOS << binary_unpack(data.data(), data.size(), new_object.v12, true, offset++);
        FASSERT(new_object == basic_object);
    }

    {
        int a            = 2;
        int b            = 3;
        int c            = 4;
        using tuple_type = std::tuple<int, int, int>;
        tuple_type origin { a, b, c };
        auto data  = binary_batch_pack(a, b, c);
        auto data2 = binary_pack_tuple(origin);
        FINFOS << "gen buf:" << Fundamental::Utils::BufferToHex(data.data(), data.size());
        FINFOS << "gen buf:" << Fundamental::Utils::BufferToHex(data2.data(), data2.size());
        tuple_type gen;

        FASSERT(binary_unpack_tuple(data.data(), data.size(), gen, true, 0) == true);
        FASSERT(origin == gen);
        tuple_type gen2;
        FASSERT(binary_unpack_tuple(data2.data(), data2.size(), gen2, true, 0) == true);
        FASSERT(origin == gen2);
        [[maybe_unused]] int a_c = 0;
        [[maybe_unused]] int b_c = 0;
        [[maybe_unused]] int c_c = 0;
        FASSERT(binary_bacth_unpack(data.data(), data.size(), true, 0, a_c, b_c, c_c) == true);
        FASSERT(a == a_c && b == b_c && c == c_c);
        a_c = 0;
        b_c = 0;
        c_c = 0;
        FASSERT(binary_bacth_unpack(data2.data(), data2.size(), true, 0, a_c, b_c, c_c) == true);
        FASSERT(a == a_c && b == b_c && c == c_c);
    }

    {
        int x     = 2;
        int y     = 3;
        auto data = binary_pack(x);
        binary_pack(data, y);
        FINFOS << "gen buf:" << Fundamental::Utils::BufferToHex(data.data(), data.size());

        int x_gen = 0;
        int y_gen = 0;

        FINFOS << binary_unpack(data.data(), data.size(), x_gen, true, 0);
        FINFOS << binary_unpack(data.data(), data.size(), y_gen, true, 1);
        FASSERT(x == x_gen);
        FASSERT(y == y_gen);
    }

    {
        CustomObject origin;
        origin.cs.push_back(color::blue);
        auto& object = origin.objects.emplace_back();
        object["1"]  = 1;
        object["2"]  = 3;
        origin.is.push_back(1);
        origin.is.push_back(2);
        auto data = binary_pack(origin);
        FINFOS << "gen buf:" << Fundamental::Utils::BufferToHex(data.data(), data.size());
        CustomObject gen;
        FINFOS << binary_unpack(data.data(), data.size(), gen);
        FASSERT(origin == gen);
        CustomObject2 gen2;
        FINFOS << binary_unpack(data.data(), data.size(), gen2);
        FASSERT(origin.is == gen2.is);
    }

    {
        nlohmann::json origin;
        origin["1"] = 1;
        origin["2"] = 2;
        auto data   = binary_pack(origin);
        FINFOS << "gen buf:" << Fundamental::Utils::BufferToHex(data.data(), data.size());
        nlohmann::json gen;
        FINFOS << binary_unpack(data.data(), data.size(), gen);
        FASSERT(origin == gen);
    }

    {
        using basic_type  = bool;
        basic_type origin = false;
        auto data         = binary_pack(origin);
        FINFOS << "buf:" << Fundamental::Utils::BufferToHex(data.data(), data.size());
        basic_type gen {};
        FINFOS << binary_unpack(data.data(), data.size(), gen);
        FINFOS << gen;
    }
    {
        using basic_type  = char;
        basic_type origin = 1;
        auto data         = binary_pack(origin);
        FINFOS << "buf:" << Fundamental::Utils::BufferToHex(data.data(), data.size());
        basic_type gen {};
        FINFOS << binary_unpack(data.data(), data.size(), gen);
        FINFOS << static_cast<std::uint64_t>(gen);
    }
    {
        using basic_type  = std::int8_t;
        basic_type origin = 1;
        auto data         = binary_pack(origin);
        FINFOS << "buf:" << Fundamental::Utils::BufferToHex(data.data(), data.size());
        basic_type gen {};
        FINFOS << binary_unpack(data.data(), data.size(), gen);
        FINFOS << static_cast<std::uint64_t>(gen);
    }
    {
        using basic_type  = std::int16_t;
        basic_type origin = 5;
        auto data         = binary_pack(origin);
        FINFOS << "buf:" << Fundamental::Utils::BufferToHex(data.data(), data.size());
        basic_type gen {};
        FINFOS << binary_unpack(data.data(), data.size(), gen);
        FINFOS << static_cast<std::uint64_t>(gen);
    }
    {
        using basic_type  = std::int32_t;
        basic_type origin = 5;
        auto data         = binary_pack(origin);
        FINFOS << "buf:" << Fundamental::Utils::BufferToHex(data.data(), data.size());
        basic_type gen {};
        FINFOS << binary_unpack(data.data(), data.size(), gen);
        FINFOS << gen;
    }
    {
        using basic_type  = std::int64_t;
        basic_type origin = 5;
        auto data         = binary_pack(origin);
        FINFOS << "buf:" << Fundamental::Utils::BufferToHex(data.data(), data.size());
        basic_type gen {};
        FINFOS << binary_unpack(data.data(), data.size(), gen);
        FINFOS << gen;
    }
    {
        using basic_type  = std::uint8_t;
        basic_type origin = 5;
        auto data         = binary_pack(origin);
        FINFOS << "buf:" << Fundamental::Utils::BufferToHex(data.data(), data.size());
        basic_type gen {};
        FINFOS << binary_unpack(data.data(), data.size(), gen);
        FINFOS << static_cast<std::uint64_t>(gen);
    }
    {
        using basic_type  = std::uint16_t;
        basic_type origin = 5;
        auto data         = binary_pack(origin);
        FINFOS << "buf:" << Fundamental::Utils::BufferToHex(data.data(), data.size());
        basic_type gen {};
        FINFOS << binary_unpack(data.data(), data.size(), gen);
        FINFOS << static_cast<std::uint64_t>(gen);
    }
    {
        using basic_type  = std::uint32_t;
        basic_type origin = 5;
        auto data         = binary_pack(origin);
        FINFOS << "buf:" << Fundamental::Utils::BufferToHex(data.data(), data.size());
        basic_type gen {};
        FINFOS << binary_unpack(data.data(), data.size(), gen);
        FINFOS << gen;
    }
    {
        using basic_type  = std::uint64_t;
        basic_type origin = 5;
        auto data         = binary_pack(origin);
        FINFOS << "buf:" << Fundamental::Utils::BufferToHex(data.data(), data.size());
        basic_type gen {};
        FINFOS << binary_unpack(data.data(), data.size(), gen);
        FINFOS << gen;
    }
    {
        using basic_type  = float;
        basic_type origin = 5;
        auto data         = binary_pack(origin);
        FINFOS << "buf:" << Fundamental::Utils::BufferToHex(data.data(), data.size());
        basic_type gen {};
        FINFOS << binary_unpack(data.data(), data.size(), gen);
        FINFOS << gen;
    }
    {
        using basic_type  = double;
        basic_type origin = 5;
        auto data         = binary_pack(origin);
        FINFOS << "buf:" << Fundamental::Utils::BufferToHex(data.data(), data.size());
        basic_type gen {};
        FINFOS << binary_unpack(data.data(), data.size(), gen);
        FINFOS << gen;
    }

    {
        using basic_type  = std::string;
        basic_type origin = "123";
        auto data         = binary_pack(origin);
        FINFOS << "buf:" << Fundamental::Utils::BufferToHex(data.data(), data.size());
        basic_type gen;
        FINFOS << binary_unpack(data.data(), data.size(), gen);
        FINFOS << gen;
    }

    {
        using basic_type  = color;
        basic_type origin = color::green;
        auto data         = binary_pack(origin);
        FINFOS << "buf:" << Fundamental::Utils::BufferToHex(data.data(), data.size());
        basic_type gen;
        FINFOS << binary_unpack(data.data(), data.size(), gen);
        FINFOS << static_cast<std::uint64_t>(gen);
        [[maybe_unused]] unregister_color n_c;
        FASSERT(!binary_unpack(data.data(), data.size(), n_c));
    }

    {
        using basic_type = std::vector<int>;
        basic_type origin { 0, 1, 2, 3, 4 };
        auto data = binary_pack(origin);
        FINFOS << "buf:" << Fundamental::Utils::BufferToHex(data.data(), data.size());
        basic_type gen;
        FINFOS << binary_unpack(data.data(), data.size(), gen);
        FASSERT(gen == origin);
    }
    {
        using basic_type = std::list<int>;
        basic_type origin { 0, 1, 2, 3, 4 };
        auto data = binary_pack(origin);
        FINFOS << "buf:" << Fundamental::Utils::BufferToHex(data.data(), data.size());
        basic_type gen;
        FINFOS << binary_unpack(data.data(), data.size(), gen);
        FASSERT(gen == origin);
    }

    {
        using basic_type = std::set<int>;
        basic_type origin { 0, 1, 2, 3, 4 };
        auto data = binary_pack(origin);
        FINFOS << "buf:" << Fundamental::Utils::BufferToHex(data.data(), data.size());
        basic_type gen;
        FINFOS << binary_unpack(data.data(), data.size(), gen);
        FASSERT(gen == origin);
    }

    {
        using basic_type = std::map<int, int>;
        basic_type origin { { 0, 0 }, { 1, 1 }, { 2, 2 }, { 3, 3 }, { 4, 4 } };
        auto data = binary_pack(origin);
        FINFOS << "buf:" << Fundamental::Utils::BufferToHex(data.data(), data.size());
        basic_type gen;
        FINFOS << binary_unpack(data.data(), data.size(), gen);
        FASSERT(gen == origin);
    }

    {
        using basic_type = std::unordered_set<int>;
        basic_type origin { 0, 1, 2, 3, 4 };
        auto data = binary_pack(origin);
        FINFOS << "buf:" << Fundamental::Utils::BufferToHex(data.data(), data.size());
        basic_type gen;
        FINFOS << binary_unpack(data.data(), data.size(), gen);
        FASSERT(gen == origin);
    }
    {
        using basic_type = std::unordered_map<std::string, std::string>;
        basic_type origin { { "0", "0" }, { "1", "1" }, { "2", "2" }, { "3", "3" }, { "4", "4" } };
        auto data = binary_pack(origin);
        FINFOS << "buf:" << Fundamental::Utils::BufferToHex(data.data(), data.size());
        basic_type gen;
        FINFOS << binary_unpack(data.data(), data.size(), gen);
        FASSERT(gen == origin);
    }

    {
        point2d origin { 2, 3 };
        auto data = binary_pack(origin);
        FINFOS << "buf:" << Fundamental::Utils::BufferToHex(data.data(), data.size());
        point2d gen;
        FINFOS << binary_unpack(data.data(), data.size(), gen);
        FINFOS << gen.x;
        FINFOS << gen.y;
    }
    {
        ComplexS1 origin;
        origin.z  = 0.1f;
        origin.s  = "231232132";
        auto data = binary_pack(origin);
        FINFOS << "gen buf:" << Fundamental::Utils::BufferToHex(data.data(), data.size());
        ComplexS1 gen;
        FINFOS << binary_unpack(data.data(), data.size(), gen);
        FASSERT(origin.s == gen.s);
        FASSERT(origin.z == gen.z);
    }

    {
        ComplexSS origin;
        origin.ponits      = decltype(origin.ponits) { { 1, 2 }, { 3, 4 }, { 5, 6 } };
        origin.dic         = decltype(origin.dic) { 1, 2, 3, 4, 5 };
        origin.c           = color::blue;
        origin.point       = { 4, 5 };
        origin.object["1"] = 1;
        origin.object["2"] = 2;
        auto data          = binary_pack(origin);
        FINFOS << "gen buf:" << Fundamental::Utils::BufferToHex(data.data(), data.size());
        ComplexSS gen;
        FINFOS << binary_unpack(data.data(), data.size(), gen);
        FASSERT(origin.point == gen.point);
        FASSERT(origin.dic == gen.dic);
        FASSERT(origin.ponits == gen.ponits);
        FASSERT(origin.object == gen.object);
        FASSERT(static_cast<int>(origin.c) == static_cast<int>(gen.c));
    }

    return 0;
}

void test_normal_packer() {
    {
        TestExtraObject obj;
        obj.ss.emplace_back("123");
        obj.ss.emplace_back("1234");
        obj.cc.emplace_back(color::blue);
        obj.sss.insert("456");
        obj.sss.insert("4567");
        obj.mm.emplace("123", "123");
        obj.mm.emplace("1234", "1233");
        auto data = Fundamental::io::to_json(obj);
        TestExtraObject tmp;
        [[maybe_unused]] auto ret = Fundamental::io::from_json(data, tmp);
        FASSERT(ret);
        FINFO("raw:{}", data);
        FINFO("gen:{}", Fundamental::io::to_json(tmp));
        FASSERT(tmp == obj);
    }
    {
        std::vector<TestContainerEle> v;
        {
            auto& item = v.emplace_back();
            item.x     = 1;
            item.y     = 2;
        }
        {
            auto& item = v.emplace_back();
            item.x     = 3;
            item.y     = 4;
        }
        auto data = Fundamental::io::to_json(v);
        decltype(v) tmp;
        Fundamental::io::from_json(data, tmp);
        FINFO("raw:{}", data);
        FINFO("gen:{}", Fundamental::io::to_json(tmp));
        FASSERT(tmp == v);
    }

    {
        int cnt = 0;

        TestContainer obj;
        while (cnt < 1) {
            obj.update();
            auto data = Fundamental::io::to_json(obj);
            TestContainer tmp;
            auto type        = rttr::type::get(tmp.obj);
            rttr::variant v1 = tmp.obj.x;
            rttr::variant v2 = 123L;
            bool convert_ret         = v2.convert(v1.get_type());
            FASSERT(convert_ret, "convert from {} to {} failed", v2.get_type().get_name().data(),
                    v1.get_type().get_name().data());
            type.set_property_value("x", tmp.obj, v2);
            FASSERT(tmp.obj.x == 123, "{}", tmp.obj.x);
            [[maybe_unused]] auto ret = Fundamental::io::from_json(data, tmp);
            FASSERT(ret);
            FINFO("raw:{}", data);
            FINFO("gen:{}", Fundamental::io::to_json(tmp));
            FASSERT(tmp == obj);
            ++cnt;
        }
    }
}