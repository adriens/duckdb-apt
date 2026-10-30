#include "apt_common.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/vector_operations/binary_executor.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"

namespace duckdb {

// Weight of a non-digit character in dpkg's ordering: "~" sorts before everything, even the end of the string,
// then letters, then all other characters
static int32_t CharacterOrder(char c) {
	if (StringUtil::CharacterIsDigit(c)) {
		return 0;
	}
	if (StringUtil::CharacterIsAlpha(c)) {
		return c;
	}
	if (c == '~') {
		return -1;
	}
	return c ? static_cast<uint8_t>(c) + 256 : 0;
}

// Compares alternating non-digit and digit parts, like dpkg's verrevcmp()
static int32_t CompareVersionParts(const string &a, const string &b) {
	idx_t i = 0;
	idx_t j = 0;
	auto at = [](const string &s, idx_t pos) {
		return pos < s.size() ? s[pos] : '\0';
	};
	while (i < a.size() || j < b.size()) {
		while ((i < a.size() && !StringUtil::CharacterIsDigit(a[i])) ||
		       (j < b.size() && !StringUtil::CharacterIsDigit(b[j]))) {
			auto a_order = CharacterOrder(at(a, i));
			auto b_order = CharacterOrder(at(b, j));
			if (a_order != b_order) {
				return a_order - b_order;
			}
			i++;
			j++;
		}
		while (at(a, i) == '0') {
			i++;
		}
		while (at(b, j) == '0') {
			j++;
		}
		int32_t first_difference = 0;
		while (StringUtil::CharacterIsDigit(at(a, i)) && StringUtil::CharacterIsDigit(at(b, j))) {
			if (!first_difference) {
				first_difference = at(a, i) - at(b, j);
			}
			i++;
			j++;
		}
		if (StringUtil::CharacterIsDigit(at(a, i))) {
			return 1;
		}
		if (StringUtil::CharacterIsDigit(at(b, j))) {
			return -1;
		}
		if (first_difference) {
			return first_difference;
		}
	}
	return 0;
}

struct DebianVersion {
	int64_t epoch = 0;
	string upstream;
	string revision;
};

// "[epoch:]upstream[-revision]": the revision starts after the last hyphen
static DebianVersion ParseDebianVersion(const string &version) {
	DebianVersion result;
	auto rest = version;
	StringUtil::Trim(rest);
	auto colon = rest.find(':');
	if (colon != string::npos) {
		try {
			result.epoch = std::stoll(rest.substr(0, colon));
		} catch (...) {
			result.epoch = 0;
		}
		rest = rest.substr(colon + 1);
	}
	auto hyphen = rest.rfind('-');
	if (hyphen != string::npos) {
		result.revision = rest.substr(hyphen + 1);
		rest = rest.substr(0, hyphen);
	}
	result.upstream = rest;
	return result;
}

int32_t CompareDebianVersions(const string &a, const string &b) {
	auto left = ParseDebianVersion(a);
	auto right = ParseDebianVersion(b);
	int32_t result = 0;
	if (left.epoch != right.epoch) {
		result = left.epoch < right.epoch ? -1 : 1;
	} else {
		result = CompareVersionParts(left.upstream, right.upstream);
		if (result == 0) {
			result = CompareVersionParts(left.revision, right.revision);
		}
	}
	return result < 0 ? -1 : (result > 0 ? 1 : 0);
}

static void AptVersionCompareFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	BinaryExecutor::Execute<string_t, string_t, int32_t>(
	    args.data[0], args.data[1], result,
	    [](string_t a, string_t b) { return CompareDebianVersions(a.GetString(), b.GetString()); });
}

void RegisterVersionFunctions(ExtensionLoader &loader) {
	ScalarFunction compare("apt_version_compare", {LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::INTEGER,
	                       AptVersionCompareFunction);
	CreateScalarFunctionInfo info(std::move(compare));
	FunctionDescription documentation;
	documentation.parameter_names = {"a", "b"};
	documentation.parameter_types = {LogicalType::VARCHAR, LogicalType::VARCHAR};
	documentation.description = "Compares two Debian package versions like dpkg --compare-versions (epoch, upstream "
	                            "version, revision, \"~\" sorting first): -1 if a < b, 0 if equal, 1 if a > b";
	documentation.examples.emplace_back("SELECT apt_version_compare('1:1.0', '2.0')");
	info.descriptions.push_back(std::move(documentation));
	loader.RegisterFunction(std::move(info));
}

} // namespace duckdb
