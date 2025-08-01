#include "duckdb/common/extra_type_info.hpp"
#include "duckdb/common/extra_type_info/enum_type_info.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/common/string_map_set.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Extension Type Info
//===--------------------------------------------------------------------===//

bool ExtensionTypeInfo::Equals(optional_ptr<ExtensionTypeInfo> lhs, optional_ptr<ExtensionTypeInfo> rhs) {
	// Either both are null, or both are the same, so they are equal
	if (lhs.get() == rhs.get()) {
		return true;
	}
	// If one is null, then we cant compare them
	if (lhs == nullptr || rhs == nullptr) {
		return true;
	}

	// Both are not null, so we can compare them
	D_ASSERT(lhs != nullptr && rhs != nullptr);

	// Compare modifiers
	const auto &lhs_mods = lhs->modifiers;
	const auto &rhs_mods = rhs->modifiers;
	const auto common_mods = MinValue(lhs_mods.size(), rhs_mods.size());
	for (idx_t i = 0; i < common_mods; i++) {
		// If the types are not strictly equal, they are not equal
		auto &lhs_val = lhs_mods[i].value;
		auto &rhs_val = rhs_mods[i].value;

		if (lhs_val.type() != rhs_val.type()) {
			return false;
		}

		// If both are null, its fine
		if (lhs_val.IsNull() && rhs_val.IsNull()) {
			continue;
		}

		// If one is null, the other must be null too
		if (lhs_val.IsNull() != rhs_val.IsNull()) {
			return false;
		}

		if (lhs_val != rhs_val) {
			return false;
		}
	}

	// Properties are optional, so only compare those present in both
	const auto &lhs_props = lhs->properties;
	const auto &rhs_props = rhs->properties;

	for (const auto &kv : lhs_props) {
		auto it = rhs_props.find(kv.first);
		if (it == rhs_props.end()) {
			// Continue
			continue;
		}
		if (kv.second != it->second) {
			// Mismatch!
			return false;
		}
	}

	// All ok!
	return true;
}

//===--------------------------------------------------------------------===//
// Extra Type Info
//===--------------------------------------------------------------------===//
ExtraTypeInfo::ExtraTypeInfo(ExtraTypeInfoType type) : type(type) {
}
ExtraTypeInfo::ExtraTypeInfo(ExtraTypeInfoType type, string alias) : type(type), alias(std::move(alias)) {
}
ExtraTypeInfo::~ExtraTypeInfo() {
}

ExtraTypeInfo::ExtraTypeInfo(const ExtraTypeInfo &other) : type(other.type), alias(other.alias) {
	if (other.extension_info) {
		extension_info = make_uniq<ExtensionTypeInfo>(*other.extension_info);
	}
}

ExtraTypeInfo &ExtraTypeInfo::operator=(const ExtraTypeInfo &other) {
	type = other.type;
	alias = other.alias;
	if (other.extension_info) {
		extension_info = make_uniq<ExtensionTypeInfo>(*other.extension_info);
	}
	return *this;
}

shared_ptr<ExtraTypeInfo> ExtraTypeInfo::Copy() const {
	return shared_ptr<ExtraTypeInfo>(new ExtraTypeInfo(*this));
}

shared_ptr<ExtraTypeInfo> ExtraTypeInfo::DeepCopy() const {
	return Copy();
}

bool ExtraTypeInfo::Equals(ExtraTypeInfo *other_p) const {
	if (type == ExtraTypeInfoType::INVALID_TYPE_INFO || type == ExtraTypeInfoType::STRING_TYPE_INFO ||
	    type == ExtraTypeInfoType::GENERIC_TYPE_INFO) {
		if (!other_p) {
			if (!alias.empty()) {
				return false;
			}
			if (extension_info) {
				return false;
			}
			//! We only need to compare aliases when both types have them in this case
			return true;
		}
		if (alias != other_p->alias) {
			return false;
		}
		if (!ExtensionTypeInfo::Equals(extension_info, other_p->extension_info)) {
			return false;
		}
		return true;
	}
	if (!other_p) {
		return false;
	}
	if (type != other_p->type) {
		return false;
	}
	if (alias != other_p->alias) {
		return false;
	}
	if (!ExtensionTypeInfo::Equals(extension_info, other_p->extension_info)) {
		return false;
	}
	return EqualsInternal(other_p);
}

bool ExtraTypeInfo::EqualsInternal(ExtraTypeInfo *other_p) const {
	// Do nothing
	return true;
}

//===--------------------------------------------------------------------===//
// Decimal Type Info
//===--------------------------------------------------------------------===//
DecimalTypeInfo::DecimalTypeInfo() : ExtraTypeInfo(ExtraTypeInfoType::DECIMAL_TYPE_INFO) {
}

DecimalTypeInfo::DecimalTypeInfo(uint8_t width_p, uint8_t scale_p)
    : ExtraTypeInfo(ExtraTypeInfoType::DECIMAL_TYPE_INFO), width(width_p), scale(scale_p) {
	D_ASSERT(width_p >= scale_p);
}

bool DecimalTypeInfo::EqualsInternal(ExtraTypeInfo *other_p) const {
	auto &other = other_p->Cast<DecimalTypeInfo>();
	return width == other.width && scale == other.scale;
}

shared_ptr<ExtraTypeInfo> DecimalTypeInfo::Copy() const {
	return make_shared_ptr<DecimalTypeInfo>(*this);
}

//===--------------------------------------------------------------------===//
// String Type Info
//===--------------------------------------------------------------------===//
StringTypeInfo::StringTypeInfo() : ExtraTypeInfo(ExtraTypeInfoType::STRING_TYPE_INFO) {
}

StringTypeInfo::StringTypeInfo(string collation_p)
    : ExtraTypeInfo(ExtraTypeInfoType::STRING_TYPE_INFO), collation(std::move(collation_p)) {
}

bool StringTypeInfo::EqualsInternal(ExtraTypeInfo *other_p) const {
	// collation info has no impact on equality
	return true;
}

shared_ptr<ExtraTypeInfo> StringTypeInfo::Copy() const {
	return make_shared_ptr<StringTypeInfo>(*this);
}

//===--------------------------------------------------------------------===//
// List Type Info
//===--------------------------------------------------------------------===//
ListTypeInfo::ListTypeInfo() : ExtraTypeInfo(ExtraTypeInfoType::LIST_TYPE_INFO) {
}

ListTypeInfo::ListTypeInfo(LogicalType child_type_p)
    : ExtraTypeInfo(ExtraTypeInfoType::LIST_TYPE_INFO), child_type(std::move(child_type_p)) {
}

bool ListTypeInfo::EqualsInternal(ExtraTypeInfo *other_p) const {
	auto &other = other_p->Cast<ListTypeInfo>();
	return child_type == other.child_type;
}

shared_ptr<ExtraTypeInfo> ListTypeInfo::Copy() const {
	return make_shared_ptr<ListTypeInfo>(*this);
}

shared_ptr<ExtraTypeInfo> ListTypeInfo::DeepCopy() const {
	return make_shared_ptr<ListTypeInfo>(child_type.DeepCopy());
}

//===--------------------------------------------------------------------===//
// Struct Type Info
//===--------------------------------------------------------------------===//
StructTypeInfo::StructTypeInfo() : ExtraTypeInfo(ExtraTypeInfoType::STRUCT_TYPE_INFO) {
}

StructTypeInfo::StructTypeInfo(child_list_t<LogicalType> child_types_p)
    : ExtraTypeInfo(ExtraTypeInfoType::STRUCT_TYPE_INFO), child_types(std::move(child_types_p)) {
}

bool StructTypeInfo::EqualsInternal(ExtraTypeInfo *other_p) const {
	auto &other = other_p->Cast<StructTypeInfo>();
	return child_types == other.child_types;
}

shared_ptr<ExtraTypeInfo> StructTypeInfo::Copy() const {
	return make_shared_ptr<StructTypeInfo>(*this);
}

shared_ptr<ExtraTypeInfo> StructTypeInfo::DeepCopy() const {
	child_list_t<LogicalType> copied_child_types;
	for (const auto &child_type : child_types) {
		copied_child_types.emplace_back(child_type.first, child_type.second.DeepCopy());
	}
	return make_shared_ptr<StructTypeInfo>(std::move(copied_child_types));
}

//===--------------------------------------------------------------------===//
// Aggregate State Type Info
//===--------------------------------------------------------------------===//
AggregateStateTypeInfo::AggregateStateTypeInfo() : ExtraTypeInfo(ExtraTypeInfoType::AGGREGATE_STATE_TYPE_INFO) {
}

AggregateStateTypeInfo::AggregateStateTypeInfo(aggregate_state_t state_type_p)
    : ExtraTypeInfo(ExtraTypeInfoType::AGGREGATE_STATE_TYPE_INFO), state_type(std::move(state_type_p)) {
}

bool AggregateStateTypeInfo::EqualsInternal(ExtraTypeInfo *other_p) const {
	auto &other = other_p->Cast<AggregateStateTypeInfo>();
	return state_type.function_name == other.state_type.function_name &&
	       state_type.return_type == other.state_type.return_type &&
	       state_type.bound_argument_types == other.state_type.bound_argument_types;
}

shared_ptr<ExtraTypeInfo> AggregateStateTypeInfo::Copy() const {
	return make_shared_ptr<AggregateStateTypeInfo>(*this);
}

//===--------------------------------------------------------------------===//
// User Type Info
//===--------------------------------------------------------------------===//
UserTypeInfo::UserTypeInfo() : ExtraTypeInfo(ExtraTypeInfoType::USER_TYPE_INFO) {
}

UserTypeInfo::UserTypeInfo(string name_p)
    : ExtraTypeInfo(ExtraTypeInfoType::USER_TYPE_INFO), user_type_name(std::move(name_p)) {
}

UserTypeInfo::UserTypeInfo(string name_p, vector<Value> modifiers_p)
    : ExtraTypeInfo(ExtraTypeInfoType::USER_TYPE_INFO), user_type_name(std::move(name_p)),
      user_type_modifiers(std::move(modifiers_p)) {
}

UserTypeInfo::UserTypeInfo(string catalog_p, string schema_p, string name_p, vector<Value> modifiers_p)
    : ExtraTypeInfo(ExtraTypeInfoType::USER_TYPE_INFO), catalog(std::move(catalog_p)), schema(std::move(schema_p)),
      user_type_name(std::move(name_p)), user_type_modifiers(std::move(modifiers_p)) {
}

bool UserTypeInfo::EqualsInternal(ExtraTypeInfo *other_p) const {
	auto &other = other_p->Cast<UserTypeInfo>();
	return other.user_type_name == user_type_name;
}

shared_ptr<ExtraTypeInfo> UserTypeInfo::Copy() const {
	return make_shared_ptr<UserTypeInfo>(*this);
}

//===--------------------------------------------------------------------===//
// Enum Type Info
//===--------------------------------------------------------------------===//
PhysicalType EnumTypeInfo::DictType(idx_t size) {
	if (size <= NumericLimits<uint8_t>::Maximum()) {
		return PhysicalType::UINT8;
	} else if (size <= NumericLimits<uint16_t>::Maximum()) {
		return PhysicalType::UINT16;
	} else if (size <= NumericLimits<uint32_t>::Maximum()) {
		return PhysicalType::UINT32;
	} else {
		throw InternalException("Enum size must be lower than " + std::to_string(NumericLimits<uint32_t>::Maximum()));
	}
}

EnumTypeInfo::EnumTypeInfo(Vector &values_insert_order_p, idx_t dict_size_p)
    : ExtraTypeInfo(ExtraTypeInfoType::ENUM_TYPE_INFO), values_insert_order(values_insert_order_p),
      dict_type(EnumDictType::VECTOR_DICT), dict_size(dict_size_p) {
}

const EnumDictType &EnumTypeInfo::GetEnumDictType() const {
	return dict_type;
}

const Vector &EnumTypeInfo::GetValuesInsertOrder() const {
	return values_insert_order;
}

const idx_t &EnumTypeInfo::GetDictSize() const {
	return dict_size;
}

LogicalType EnumTypeInfo::CreateType(Vector &ordered_data, idx_t size) {
	// Generate EnumTypeInfo
	shared_ptr<ExtraTypeInfo> info;
	auto enum_internal_type = EnumTypeInfo::DictType(size);
	switch (enum_internal_type) {
	case PhysicalType::UINT8:
		info = make_shared_ptr<EnumTypeInfoTemplated<uint8_t>>(ordered_data, size);
		break;
	case PhysicalType::UINT16:
		info = make_shared_ptr<EnumTypeInfoTemplated<uint16_t>>(ordered_data, size);
		break;
	case PhysicalType::UINT32:
		info = make_shared_ptr<EnumTypeInfoTemplated<uint32_t>>(ordered_data, size);
		break;
	default:
		throw InternalException("Invalid Physical Type for ENUMs");
	}
	// Generate Actual Enum Type
	return LogicalType(LogicalTypeId::ENUM, info);
}

template <class T>
int64_t TemplatedGetPos(const string_map_t<T> &map, const string_t &key) {
	auto it = map.find(key);
	if (it == map.end()) {
		return -1;
	}
	return it->second;
}

int64_t EnumType::GetPos(const LogicalType &type, const string_t &key) {
	auto info = type.AuxInfo();
	switch (type.InternalType()) {
	case PhysicalType::UINT8:
		return TemplatedGetPos(info->Cast<EnumTypeInfoTemplated<uint8_t>>().GetValues(), key);
	case PhysicalType::UINT16:
		return TemplatedGetPos(info->Cast<EnumTypeInfoTemplated<uint16_t>>().GetValues(), key);
	case PhysicalType::UINT32:
		return TemplatedGetPos(info->Cast<EnumTypeInfoTemplated<uint32_t>>().GetValues(), key);
	default:
		throw InternalException("ENUM can only have unsigned integers (except UINT64) as physical types");
	}
}

string_t EnumType::GetString(const LogicalType &type, idx_t pos) {
	D_ASSERT(pos < EnumType::GetSize(type));
	return FlatVector::GetData<string_t>(EnumType::GetValuesInsertOrder(type))[pos];
}

shared_ptr<ExtraTypeInfo> EnumTypeInfo::Deserialize(Deserializer &deserializer) {
	auto values_count = deserializer.ReadProperty<idx_t>(200, "values_count");
	auto enum_internal_type = EnumTypeInfo::DictType(values_count);
	switch (enum_internal_type) {
	case PhysicalType::UINT8:
		return EnumTypeInfoTemplated<uint8_t>::Deserialize(deserializer, NumericCast<uint32_t>(values_count));
	case PhysicalType::UINT16:
		return EnumTypeInfoTemplated<uint16_t>::Deserialize(deserializer, NumericCast<uint32_t>(values_count));
	case PhysicalType::UINT32:
		return EnumTypeInfoTemplated<uint32_t>::Deserialize(deserializer, NumericCast<uint32_t>(values_count));
	default:
		throw InternalException("Invalid Physical Type for ENUMs");
	}
}

// Equalities are only used in enums with different catalog entries
bool EnumTypeInfo::EqualsInternal(ExtraTypeInfo *other_p) const {
	auto &other = other_p->Cast<EnumTypeInfo>();
	if (dict_type != other.dict_type) {
		return false;
	}
	D_ASSERT(dict_type == EnumDictType::VECTOR_DICT);
	// We must check if both enums have the same size
	if (other.dict_size != dict_size) {
		return false;
	}
	auto other_vector_ptr = FlatVector::GetData<string_t>(other.values_insert_order);
	auto this_vector_ptr = FlatVector::GetData<string_t>(values_insert_order);

	// Now we must check if all strings are the same
	for (idx_t i = 0; i < dict_size; i++) {
		if (!Equals::Operation(other_vector_ptr[i], this_vector_ptr[i])) {
			return false;
		}
	}
	return true;
}

void EnumTypeInfo::Serialize(Serializer &serializer) const {
	ExtraTypeInfo::Serialize(serializer);

	// Enums are special in that we serialize their values as a list instead of dumping the whole vector
	auto strings = FlatVector::GetData<string_t>(values_insert_order);
	serializer.WriteProperty(200, "values_count", dict_size);
	serializer.WriteList(201, "values", dict_size,
	                     [&](Serializer::List &list, idx_t i) { list.WriteElement(strings[i]); });
}

shared_ptr<ExtraTypeInfo> EnumTypeInfo::Copy() const {
	Vector values_insert_order_copy(LogicalType::VARCHAR, false, false, 0);
	values_insert_order_copy.Reference(values_insert_order);
	return make_shared_ptr<EnumTypeInfo>(values_insert_order_copy, dict_size);
}

//===--------------------------------------------------------------------===//
// ArrayTypeInfo
//===--------------------------------------------------------------------===//

ArrayTypeInfo::ArrayTypeInfo(LogicalType child_type_p, uint32_t size_p)
    : ExtraTypeInfo(ExtraTypeInfoType::ARRAY_TYPE_INFO), child_type(std::move(child_type_p)), size(size_p) {
}

bool ArrayTypeInfo::EqualsInternal(ExtraTypeInfo *other_p) const {
	auto &other = other_p->Cast<ArrayTypeInfo>();
	return child_type == other.child_type && size == other.size;
}

shared_ptr<ExtraTypeInfo> ArrayTypeInfo::Copy() const {
	return make_shared_ptr<ArrayTypeInfo>(*this);
}

shared_ptr<ExtraTypeInfo> ArrayTypeInfo::DeepCopy() const {
	return make_shared_ptr<ArrayTypeInfo>(child_type.DeepCopy(), size);
}

//===--------------------------------------------------------------------===//
// Any Type Info
//===--------------------------------------------------------------------===//
AnyTypeInfo::AnyTypeInfo() : ExtraTypeInfo(ExtraTypeInfoType::ANY_TYPE_INFO) {
}

AnyTypeInfo::AnyTypeInfo(LogicalType target_type_p, idx_t cast_score_p)
    : ExtraTypeInfo(ExtraTypeInfoType::ANY_TYPE_INFO), target_type(std::move(target_type_p)), cast_score(cast_score_p) {
}

bool AnyTypeInfo::EqualsInternal(ExtraTypeInfo *other_p) const {
	auto &other = other_p->Cast<AnyTypeInfo>();
	return target_type == other.target_type && cast_score == other.cast_score;
}

shared_ptr<ExtraTypeInfo> AnyTypeInfo::Copy() const {
	return make_shared_ptr<AnyTypeInfo>(*this);
}

shared_ptr<ExtraTypeInfo> AnyTypeInfo::DeepCopy() const {
	return make_shared_ptr<AnyTypeInfo>(target_type.DeepCopy(), cast_score);
}

//===--------------------------------------------------------------------===//
// Integer Literal Type Info
//===--------------------------------------------------------------------===//
IntegerLiteralTypeInfo::IntegerLiteralTypeInfo() : ExtraTypeInfo(ExtraTypeInfoType::INTEGER_LITERAL_TYPE_INFO) {
}

IntegerLiteralTypeInfo::IntegerLiteralTypeInfo(Value constant_value_p)
    : ExtraTypeInfo(ExtraTypeInfoType::INTEGER_LITERAL_TYPE_INFO), constant_value(std::move(constant_value_p)) {
	if (constant_value.IsNull()) {
		throw InternalException("Integer literal cannot be NULL");
	}
}

bool IntegerLiteralTypeInfo::EqualsInternal(ExtraTypeInfo *other_p) const {
	auto &other = other_p->Cast<IntegerLiteralTypeInfo>();
	return constant_value == other.constant_value;
}

shared_ptr<ExtraTypeInfo> IntegerLiteralTypeInfo::Copy() const {
	return make_shared_ptr<IntegerLiteralTypeInfo>(*this);
}

} // namespace duckdb
