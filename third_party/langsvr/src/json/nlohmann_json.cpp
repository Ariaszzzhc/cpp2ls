#include "langsvr/json/builder.h"
#include "langsvr/json/value.h"
#include "langsvr/span.h"
#include "src/utils/block_allocator.h"

#include <sstream>

#include "nlohmann/json.hpp"

namespace langsvr::json {

namespace {

class BuilderImpl;

class ValueImpl : public Value {
  public:
    ValueImpl(nlohmann::json&& value, BuilderImpl& builder) : v(std::move(value)), b(builder) {}

    std::string Json() const override;
    json::Kind Kind() const override;
    Result<SuccessType> Null() const override;
    Result<json::Bool> Bool() const override;
    Result<json::I64> I64() const override;
    Result<json::U64> U64() const override;
    Result<json::F64> F64() const override;
    Result<json::String> String() const override;
    Result<const Value*> Get(size_t index) const override;
    Result<const Value*> Get(std::string_view name) const override;
    size_t Count() const override;
    Result<std::vector<std::string>> MemberNames() const override;
    bool Has(std::string_view name) const override;

    Failure ErrIncorrectType(std::string_view wanted) const;

    nlohmann::json v;
    BuilderImpl& b;
};

class BuilderImpl : public Builder {
  public:
    Result<const Value*> Parse(std::string_view json) override;
    const Value* Null() override;
    const Value* Bool(json::Bool value) override;
    const Value* I64(json::I64 value) override;
    const Value* U64(json::U64 value) override;
    const Value* F64(json::F64 value) override;
    const Value* String(json::String value) override;
    const Value* Array(Span<const Value*> elements) override;
    const Value* Object(Span<Member> members) override;

    BlockAllocator<ValueImpl> allocator;
};

////////////////////////////////////////////////////////////////////////////////
// ValueImpl
////////////////////////////////////////////////////////////////////////////////
std::string ValueImpl::Json() const {
    return v.dump();
}

json::Kind ValueImpl::Kind() const {
    switch (v.type()) {
        case nlohmann::json::value_t::null:
            return json::Kind::kNull;
        case nlohmann::json::value_t::boolean:
            return json::Kind::kBool;
        case nlohmann::json::value_t::number_integer:
            return json::Kind::kI64;
        case nlohmann::json::value_t::number_unsigned:
            return json::Kind::kU64;
        case nlohmann::json::value_t::number_float:
            return json::Kind::kF64;
        case nlohmann::json::value_t::string:
            return json::Kind::kString;
        case nlohmann::json::value_t::array:
            return json::Kind::kArray;
        case nlohmann::json::value_t::object:
            return json::Kind::kObject;
        case nlohmann::json::value_t::binary:
        case nlohmann::json::value_t::discarded:
            break;
    }

    return json::Kind::kNull;
}

// json::Reader compliance

Result<SuccessType> ValueImpl::Null() const {
    if (v.is_null()) {
        return Success;
    }
    return ErrIncorrectType("Null");
}

Result<json::Bool> ValueImpl::Bool() const {
    if (v.is_boolean()) {
        return v.get<bool>();
    }
    return ErrIncorrectType("Bool");
}

Result<json::I64> ValueImpl::I64() const {
    if (v.is_number_integer()) {
        return v.get<json::I64>();
    }
    return ErrIncorrectType("I64");
}

Result<json::U64> ValueImpl::U64() const {
    if (v.is_number_unsigned()) {
        return v.get<json::U64>();
    }
    return ErrIncorrectType("U64");
}

Result<json::F64> ValueImpl::F64() const {
    if (v.is_number_float()) {
        return v.get<json::F64>();
    }
    return ErrIncorrectType("F64");
}

Result<json::String> ValueImpl::String() const {
    if (v.is_string()) {
        return v.get<std::string>();
    }
    return ErrIncorrectType("String");
}

Result<const Value*> ValueImpl::Get(size_t index) const {
    if (v.is_array()) {
        if (index < v.size()) {
            return b.allocator.Create(nlohmann::json(v[index]), b);
        }
        std::stringstream err;
        err << "index >= array length of " << v.size();
        return Failure{err.str()};
    }
    return ErrIncorrectType("Array");
}

Result<const Value*> ValueImpl::Get(std::string_view name) const {
    if (v.is_object()) {
        std::string key(name);
        if (v.contains(key)) {
            return b.allocator.Create(nlohmann::json(v[key]), b);
        }
        std::stringstream err;
        err << "object has no field with name '" << name << "'";
        return Failure{err.str()};
    }
    return ErrIncorrectType("Object");
}

size_t ValueImpl::Count() const {
    return v.size();
}

Result<std::vector<std::string>> ValueImpl::MemberNames() const {
    if (v.is_object()) {
        std::vector<std::string> names;
        names.reserve(v.size());
        for (auto& [key, val] : v.items()) {
            names.push_back(key);
        }
        return names;
    }
    return ErrIncorrectType("Object");
}

bool ValueImpl::Has(std::string_view name) const {
    return v.is_object() && v.contains(std::string(name));
}

Failure ValueImpl::ErrIncorrectType(std::string_view wanted) const {
    std::stringstream err;
    err << "value is " << v.type_name() << ", not " << wanted;
    return Failure{err.str()};
}

////////////////////////////////////////////////////////////////////////////////
// BuilderImpl
////////////////////////////////////////////////////////////////////////////////

Result<const Value*> BuilderImpl::Parse(std::string_view json) {
    try {
        auto root = nlohmann::json::parse(json);
        return allocator.Create(std::move(root), *this);
    } catch (const nlohmann::json::parse_error& e) {
        return Failure{e.what()};
    }
}

const Value* BuilderImpl::Null() {
    return allocator.Create(nlohmann::json(nullptr), *this);
}

const Value* BuilderImpl::Bool(json::Bool value) {
    return allocator.Create(nlohmann::json(value), *this);
}

const Value* BuilderImpl::I64(json::I64 value) {
    return allocator.Create(nlohmann::json(value), *this);
}

const Value* BuilderImpl::U64(json::U64 value) {
    return allocator.Create(nlohmann::json(value), *this);
}

const Value* BuilderImpl::F64(json::F64 value) {
    return allocator.Create(nlohmann::json(value), *this);
}

const Value* BuilderImpl::String(json::String value) {
    return allocator.Create(nlohmann::json(value), *this);
}

const Value* BuilderImpl::Array(Span<const Value*> elements) {
    nlohmann::json array = nlohmann::json::array();
    for (auto* el : elements) {
        array.push_back(static_cast<const ValueImpl*>(el)->v);
    }
    return allocator.Create(std::move(array), *this);
}

const Value* BuilderImpl::Object(Span<Member> members) {
    nlohmann::json object = nlohmann::json::object();
    for (auto& member : members) {
        object[member.name] = static_cast<const ValueImpl*>(member.value)->v;
    }
    return allocator.Create(std::move(object), *this);
}

}  // namespace

Value::~Value() = default;
Builder::~Builder() = default;

std::unique_ptr<Builder> Builder::Create() {
    return std::make_unique<BuilderImpl>();
}

}  // namespace langsvr::json
