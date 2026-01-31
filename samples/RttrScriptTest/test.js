import * as rttr from '::';
import * as rttr_test2 from 'test2';
print(rttr_test2.test_class2.x)
rttr_test2.test_class2.x = 2;
print(rttr_test2.test_class2.get_x())
function assert(b, str = "FAIL") {
    if (b) {
        return;
    } else {
        throw Error("assertion failed: " + str);
    }
}

function assert_eq(a, b, str = "") {
    assert(a === b, `${JSON.stringify(a)} should be equal to ${JSON.stringify(b)}. ${str}`);
}
print("global start----------");
print(rttr);
//global func
let c = rttr.prop.test_vec;
print("should equal true:", rttr.compare_vec(c));
rttr.get_int64_max(rttr.get_int64_max(0));
rttr.get_int64_min(rttr.get_int64_min(0));
rttr.get_uint64_max(rttr.get_uint64_max(0));
rttr.get_uint64_min(rttr.get_uint64_min(0));
//enum type
rttr.assert(rttr.test_enum_value(rttr.test_enum.red) == rttr.test_enum.blue);
rttr.println(rttr.global_func());

rttr.println(rttr.global_func_lambda());

//global property
print(rttr.prop.global_property)
print(rttr.prop.global_property + " has changed directly")
rttr.prop.global_property = rttr.prop.global_property + " has changed directly";

print(rttr.prop.global_property)

//global get/set
print("get with getter:", rttr.prop.global_property_s)
rttr.prop.global_property_s = rttr.prop.global_property_s + " changed with setter";
print(rttr.prop.global_property_s)

//global readonly
print(rttr.prop.readonly_global_property);
print(rttr.prop.readonly_global_property_f);
print(rttr.prop.readonly_global_property_s);


print("global end------------");


print("class start----------");
print(rttr_test);
print(rttr_test.class_base.prototype)
let base1 = new rttr_test.class_base(1);
print("class property start *******");
print("read readonly property")


print("static property v_r:", rttr_test.class_base.v_r);
print("static getter property s_r:", rttr_test.class_base.s_r);
print("property v:", base1.v);
print("getter property n_r:", base1.n_r);

print("class  property start");

print("static object member should be 444:", rttr_test.class_base.s_base_object.v);
rttr_test.class_base.s_base_object.v = 321;
print("static property changed to 321: actual ", rttr_test.class_base.s_base_object.v);

print("static getter/setter property:", rttr_test.class_base.s_base_object_f);
rttr_test.class_base.s_base_object_f = 322;
print("static getter/setter property changed to 322: actual", rttr_test.class_base.s_base_object_f);

print("property object member should be 333:", base1.base_object.v);
base1.base_object.v = 350;
print("property changed to 350: actual ", base1.base_object.v);


print("property getter/setter:", rttr_test.class_base.s_base_object_f);
rttr_test.class_base.s_base_object_f = 344;
print("property getter/setter changed to 344: actual", rttr_test.class_base.s_base_object_f);


print("class property end #########");


print("class  constructor start");
print(rttr_test.custom_object.prototype);
let o1 = new rttr_test.custom_object();
print(o1.v);
let o2 = new rttr_test.custom_object2(2);
print(o2.v);
{
    let base2 = new rttr_test.class_base3(2, 2);
    print("v should be 4:", base2.base_object.v);
}
{
    let base3 = new rttr_test.class_base4("4");
    print("v should be 4:", base3.base_object.v);
}
let ca = new rttr_test.class_a();
let cb = new rttr_test.class_b();
let cc = new rttr_test.class_c();
print("class  constructor end");
print("class  func start");
print(rttr_test.class_base.prototype);
print(rttr_test.class_base.static_func())
{
    let tag = "base call: ";
    print(tag + base1.mem_func());
    print(tag + base1.v_mem_func());
    print(tag + rttr_test.class_base.static_func2())
    print(tag + rttr_test.class_base.get_val_external())
}
{
    let tag = "class a call: ";
    print(tag + ca.mem_func());
    print(tag + ca.v_mem_func());
    print(tag + rttr_test.class_a.static_func2())
}
{
    let tag = "class b call: ";
    print(tag + cb.mem_func());
    print(tag + cb.v_mem_func());
    print(tag + rttr_test.class_b.static_func2())
}
{
    let tag = "class c call: ";
    print(tag + cc.mem_func());
    print(tag + cc.v_mem_func());
    print(tag + rttr_test.class_c.static_func2())
    print(tag + rttr_test.class_c.get_val_external())
}
print("class  func end");
print("class end----------");