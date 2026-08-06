#pragma once
// psd_writer_internal.hpp - internal serialization machinery for the PSD
// writer. This is NOT a public API: only psd_writer.cpp (and future writer
// tests) should include it. The public interface lives in psd_writer.hpp.

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "psd_writer.hpp"

namespace psdw {

// ---------------------------------------------------------------------------
// Big-endian byte buffer
// ---------------------------------------------------------------------------
class Buffer {
public:
    void u8(uint8_t v);
    void u16(uint16_t v);
    void u32(uint32_t v);
    void i16(int16_t v);
    void i32(int32_t v);
    void f64(double v);
    void raw(const void* data, size_t n);
    void raw(const std::vector<uint8_t>& v);
    void raw(const std::string& s);
    void pad(size_t align);          // zero-pad to a multiple of `align`
    size_t size() const;
    const std::vector<uint8_t>& data() const;
private:
    std::vector<uint8_t> b_;
};

// ---------------------------------------------------------------------------
// Photoshop descriptor (used by TySh, lfx2, ...)
// ---------------------------------------------------------------------------
struct Descriptor;

struct DValue {
    enum class T { Obj, List, Double, Unit, Text, Enum, Long, Bool, Raw };
    T t = T::Long;

    std::shared_ptr<Descriptor> obj;   // T::Obj
    std::vector<DValue> list;          // T::List
    std::string class_id;              // T::Obj
    std::string unit;                  // T::Unit (4 chars, e.g. "#Pxl")
    std::string enum_type;             // T::Enum
    std::string enum_val;
    std::string text;                  // T::Text (UTF-8)
    std::vector<uint8_t> raw;          // T::Raw
    double d = 0.0;
    int64_t l = 0;
    bool b = false;
};

struct Descriptor {
    std::string name;
    std::string class_id = "null";
    std::vector<std::pair<std::string, DValue>> items;

    void add(const std::string& key, DValue v) {
        items.emplace_back(key, std::move(v));
    }
};

DValue dObj(const std::string& class_id);
DValue dList(std::vector<DValue> items);
DValue dDouble(double v);
DValue dUnit(const std::string& unit, double v);
DValue dText(const std::string& utf8);
DValue dEnum(const std::string& type_id, const std::string& enum_id);
DValue dLong(int64_t v);
DValue dBool(bool v);
DValue dRaw(std::vector<uint8_t> bytes);

// ---------------------------------------------------------------------------
// Text-engine data markup (EngineData)
// ---------------------------------------------------------------------------
struct EVal;
using EDict = std::vector<std::pair<std::string, std::shared_ptr<EVal>>>;

struct EVal {
    enum class T { Dict, List, Str, Flt, Int, Bool } t = T::Int;
    EDict dict;                                  // T::Dict
    std::vector<std::shared_ptr<EVal>> list;     // T::List
    std::string str;                             // T::Str (UTF-8)
    double flt = 0.0;                            // T::Flt
    int64_t integer = 0;                         // T::Int
    bool boolean = false;                        // T::Bool
};

std::shared_ptr<EVal> eDict();
std::shared_ptr<EVal> eList(std::vector<std::shared_ptr<EVal>> items);
std::shared_ptr<EVal> eStr(const std::string& utf8);
std::shared_ptr<EVal> eFlt(double v);
std::shared_ptr<EVal> eInt(int64_t v);
std::shared_ptr<EVal> eBool(bool v);

void edict_set(EDict& d, const std::string& key, std::shared_ptr<EVal> v);

// ---------------------------------------------------------------------------
// Internal tagged-block builders (exposed for writer tests)
// ---------------------------------------------------------------------------
std::vector<uint8_t> build_tysh(const TextLayerData& text);
std::vector<uint8_t> build_lrfx(const Effects& effects);  // empty if disabled

}  // namespace psdw
