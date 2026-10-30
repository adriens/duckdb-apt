#include "apt_common.hpp"

#include "duckdb/common/optional.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"

namespace duckdb {

// Packages marked as automatically installed (as dependencies) by apt, keyed by "name|architecture"
static std::unordered_set<string> ReadAutoInstalled(FileSystem &fs, const string &root) {
	std::unordered_set<string> result;
	for (auto &stanza : ReadStanzas(fs, root + APT_EXTENDED_STATES_PATH)) {
		if (GetField(stanza, "Auto-Installed") == "1") {
			result.insert(GetField(stanza, "Package") + "|" + GetField(stanza, "Architecture"));
		}
	}
	return result;
}

vector<AptPackage> ReadInstalledPackages(FileSystem &fs, const string &root) {
	vector<AptPackage> packages;
	auto auto_installed = ReadAutoInstalled(fs, root);

	for (auto &stanza : ReadStanzas(fs, root + DPKG_STATUS_PATH)) {
		// Status is "<want> <flag> <state>", e.g. "install ok installed" or "hold ok installed"
		auto status = StringUtil::Split(GetField(stanza, "Status"), ' ');
		if (status.size() != 3 || status[2] != "installed") {
			continue;
		}

		AptPackage pkg;
		pkg.name = GetField(stanza, "Package");
		pkg.version = GetField(stanza, "Version");
		pkg.architecture = GetField(stanza, "Architecture");
		pkg.section = GetField(stanza, "Section");
		pkg.priority = GetField(stanza, "Priority");
		pkg.maintainer = GetField(stanza, "Maintainer");
		pkg.homepage = GetField(stanza, "Homepage");
		pkg.description = GetField(stanza, "Description");
		pkg.multi_arch = GetField(stanza, "Multi-Arch");
		pkg.depends = GetField(stanza, "Depends");
		pkg.pre_depends = GetField(stanza, "Pre-Depends");
		pkg.recommends = GetField(stanza, "Recommends");
		pkg.suggests = GetField(stanza, "Suggests");
		pkg.provides = GetField(stanza, "Provides");
		pkg.held = status[0] == "hold";

		// Source is "name" or "name (version)", and is absent when it matches the package name
		pkg.source = GetField(stanza, "Source");
		auto space = pkg.source.find(' ');
		if (space != string::npos) {
			pkg.source = pkg.source.substr(0, space);
		}
		if (pkg.source.empty()) {
			pkg.source = pkg.name;
		}

		try {
			pkg.installed_size_kb = std::stoll(GetField(stanza, "Installed-Size"));
		} catch (...) {
			pkg.installed_size_kb = 0;
		}
		pkg.auto_installed = auto_installed.count(pkg.name + "|" + pkg.architecture) > 0;
		packages.push_back(std::move(pkg));
	}
	return packages;
}

//===--------------------------------------------------------------------===//
// apt_packages()
//===--------------------------------------------------------------------===//
struct AptPackagesBindData : public TableFunctionData {
	vector<AptPackage> packages;
	vector<timestamp_t> installed_times;
	vector<string> purls;
};

// KEY=value lines of os-release, with optional quotes
static std::unordered_map<string, string> ReadOsRelease(FileSystem &fs, const string &root) {
	std::unordered_map<string, string> result;
	for (auto path : {OS_RELEASE_PATH, OS_RELEASE_FALLBACK_PATH}) {
		AptLineReader reader(fs, root + path);
		if (!reader.Exists()) {
			continue;
		}
		string line;
		while (reader.ReadLine(line)) {
			auto equals = line.find('=');
			if (equals == string::npos || StringUtil::StartsWith(line, "#")) {
				continue;
			}
			auto value = line.substr(equals + 1);
			if (value.size() >= 2 && (value[0] == '"' || value[0] == '\'') && value.back() == value[0]) {
				value = value.substr(1, value.size() - 2);
			}
			result[line.substr(0, equals)] = value;
		}
		break;
	}
	return result;
}

// Percent-encodes everything but the unreserved characters, as the package URL specification requires
static string PurlEncode(const string &value) {
	static const char *HEX = "0123456789ABCDEF";
	string result;
	for (auto c : value) {
		if (StringUtil::CharacterIsAlphaNumeric(c) || c == '.' || c == '-' || c == '_' || c == '~') {
			result += c;
		} else {
			auto byte = static_cast<uint8_t>(c);
			result += '%';
			result += HEX[byte >> 4];
			result += HEX[byte & 0xF];
		}
	}
	return result;
}

// Package URL (https://github.com/package-url/purl-spec), e.g.
// "pkg:deb/debian/curl@7.88.1-10?arch=amd64&distro=debian-12"
static string BuildPurl(const AptPackage &pkg, const string &os_id, const string &distro) {
	if (os_id.empty()) {
		return "";
	}
	auto purl = "pkg:deb/" + PurlEncode(os_id) + "/" + PurlEncode(pkg.name) + "@" + PurlEncode(pkg.version) +
	            "?arch=" + PurlEncode(pkg.architecture);
	if (!distro.empty()) {
		purl += "&distro=" + PurlEncode(distro);
	}
	return purl;
}

static unique_ptr<FunctionData> AptPackagesBind(ClientContext &context, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<AptPackagesBindData>();
	auto &fs = FileSystem::GetFileSystem(context);
	auto root = GetRoot(input);
	result->packages = ReadInstalledPackages(fs, root);

	auto os_release = ReadOsRelease(fs, root);
	auto os_id = StringUtil::Lower(os_release["ID"]);
	auto distro =
	    os_release["VERSION_ID"].empty() ? os_release["VERSION_CODENAME"] : os_id + "-" + os_release["VERSION_ID"];
	for (auto &pkg : result->packages) {
		// dpkg rewrites the file list of a package each time it is installed or upgraded
		auto installed_time = timestamp_t::infinity();
		for (auto &list_name : {pkg.name + ":" + pkg.architecture + ".list", pkg.name + ".list"}) {
			AptLineReader list(fs, root + DPKG_INFO_DIR + "/" + list_name);
			if (list.Exists()) {
				installed_time = list.LastModifiedTime();
				break;
			}
		}
		result->installed_times.push_back(installed_time);
		result->purls.push_back(BuildPurl(pkg, os_id, distro));
	}

	names.emplace_back("name");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("version");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("architecture");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("section");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("priority");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("installed_size_kb");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("maintainer");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("homepage");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("source");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("description");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("multi_arch");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("auto_installed");
	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("held");
	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("installed_time");
	return_types.emplace_back(LogicalType::TIMESTAMP);
	names.emplace_back("purl");
	return_types.emplace_back(LogicalType::VARCHAR);

	return std::move(result);
}

static void AptPackagesFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.bind_data->CastNoConst<AptPackagesBindData>();
	auto &local_state = data_p.local_state->Cast<AptLocalState>();
	idx_t count = 0;

	for (idx_t i = local_state.batch_index; i < data.packages.size() && count < STANDARD_VECTOR_SIZE; i++) {
		auto &pkg = data.packages[i];
		output.data[0].Append(Value(pkg.name));
		output.data[1].Append(Value(pkg.version));
		output.data[2].Append(Value(pkg.architecture));
		output.data[3].Append(StringOrNull(pkg.section));
		output.data[4].Append(StringOrNull(pkg.priority));
		output.data[5].Append(Value::BIGINT(pkg.installed_size_kb));
		output.data[6].Append(StringOrNull(pkg.maintainer));
		output.data[7].Append(StringOrNull(pkg.homepage));
		output.data[8].Append(Value(pkg.source));
		output.data[9].Append(StringOrNull(pkg.description));
		output.data[10].Append(StringOrNull(pkg.multi_arch));
		output.data[11].Append(Value::BOOLEAN(pkg.auto_installed));
		output.data[12].Append(Value::BOOLEAN(pkg.held));
		output.data[13].Append(TimestampOrNull(data.installed_times[i]));
		output.data[14].Append(StringOrNull(data.purls[i]));
		count++;
	}

	local_state.batch_index += count;
	output.CheckCardinality(count);
}

//===--------------------------------------------------------------------===//
// apt_dependencies() and apt_provides()
//===--------------------------------------------------------------------===//
struct AptRelation {
	string package;
	string architecture;
	string relation;
	string target;
	string version_constraint;
	bool alternative;
};

struct AptRelationsBindData : public TableFunctionData {
	vector<AptRelation> relations;
};

// Parse a relationship field such as "libc6 (>= 2.34), debconf | debconf-2.0" into one row per alternative
static void ParseRelation(const AptPackage &pkg, const string &relation, const string &field,
                          vector<AptRelation> &out) {
	for (auto &group : StringUtil::Split(field, ',')) {
		auto alternatives = StringUtil::Split(group, '|');
		for (auto &alt : alternatives) {
			string entry = alt;
			StringUtil::Trim(entry);
			if (entry.empty()) {
				continue;
			}
			AptRelation rel;
			rel.package = pkg.name;
			rel.architecture = pkg.architecture;
			rel.relation = relation;
			rel.alternative = alternatives.size() > 1;

			auto paren = entry.find('(');
			if (paren != string::npos) {
				auto close = entry.find(')', paren);
				rel.version_constraint =
				    entry.substr(paren + 1, close == string::npos ? string::npos : close - paren - 1);
				StringUtil::Trim(rel.version_constraint);
				entry = entry.substr(0, paren);
			}
			// Drop architecture restrictions such as "[amd64]" or "<!nocheck>", and qualifiers such as ":any"
			auto bracket = entry.find_first_of("[<:");
			if (bracket != string::npos) {
				entry = entry.substr(0, bracket);
			}
			StringUtil::Trim(entry);
			rel.target = entry;
			out.push_back(std::move(rel));
		}
	}
}

static unique_ptr<FunctionData> AptDependenciesBind(ClientContext &context, TableFunctionBindInput &input,
                                                    vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<AptRelationsBindData>();
	for (auto &pkg : ReadInstalledPackages(FileSystem::GetFileSystem(context), GetRoot(input))) {
		ParseRelation(pkg, "Pre-Depends", pkg.pre_depends, result->relations);
		ParseRelation(pkg, "Depends", pkg.depends, result->relations);
		ParseRelation(pkg, "Recommends", pkg.recommends, result->relations);
		ParseRelation(pkg, "Suggests", pkg.suggests, result->relations);
	}

	names.emplace_back("package");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("architecture");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("relation");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("dependency");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("version_constraint");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("alternative");
	return_types.emplace_back(LogicalType::BOOLEAN);

	return std::move(result);
}

static void AptDependenciesFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.bind_data->CastNoConst<AptRelationsBindData>();
	auto &local_state = data_p.local_state->Cast<AptLocalState>();
	idx_t count = 0;

	for (idx_t i = local_state.batch_index; i < data.relations.size() && count < STANDARD_VECTOR_SIZE; i++) {
		auto &rel = data.relations[i];
		output.data[0].Append(Value(rel.package));
		output.data[1].Append(Value(rel.architecture));
		output.data[2].Append(Value(rel.relation));
		output.data[3].Append(Value(rel.target));
		output.data[4].Append(StringOrNull(rel.version_constraint));
		output.data[5].Append(Value::BOOLEAN(rel.alternative));
		count++;
	}

	local_state.batch_index += count;
	output.CheckCardinality(count);
}

static unique_ptr<FunctionData> AptProvidesBind(ClientContext &context, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<AptRelationsBindData>();
	for (auto &pkg : ReadInstalledPackages(FileSystem::GetFileSystem(context), GetRoot(input))) {
		ParseRelation(pkg, "Provides", pkg.provides, result->relations);
	}
	// Provides only allows exact versions, e.g. "dbus-system-bus (= 1.16.2)"
	for (auto &rel : result->relations) {
		if (StringUtil::StartsWith(rel.version_constraint, "=")) {
			rel.version_constraint = rel.version_constraint.substr(1);
			StringUtil::Trim(rel.version_constraint);
		}
	}

	names.emplace_back("package");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("architecture");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("provides");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("version");
	return_types.emplace_back(LogicalType::VARCHAR);

	return std::move(result);
}

static void AptProvidesFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.bind_data->CastNoConst<AptRelationsBindData>();
	auto &local_state = data_p.local_state->Cast<AptLocalState>();
	idx_t count = 0;

	for (idx_t i = local_state.batch_index; i < data.relations.size() && count < STANDARD_VECTOR_SIZE; i++) {
		auto &rel = data.relations[i];
		output.data[0].Append(Value(rel.package));
		output.data[1].Append(Value(rel.architecture));
		output.data[2].Append(Value(rel.target));
		output.data[3].Append(StringOrNull(rel.version_constraint));
		count++;
	}

	local_state.batch_index += count;
	output.CheckCardinality(count);
}

//===--------------------------------------------------------------------===//
// apt_version()
//===--------------------------------------------------------------------===//
// The installed version of the apt package itself
static optional<string> GetAptVersion(ClientContext &context, string root) {
	while (StringUtil::EndsWith(root, "/")) {
		root.pop_back();
	}
	for (auto &pkg : ReadInstalledPackages(FileSystem::GetFileSystem(context), root)) {
		if (pkg.name == "apt") {
			return pkg.version;
		}
	}
	return std::nullopt;
}

static void AptVersionFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto version = GetAptVersion(state.GetContext(), "");
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	if (!version) {
		ConstantVector::SetNull(result, true);
		return;
	}
	ConstantVector::GetData<string_t>(result)[0] = StringVector::AddString(result, *version);
}

static void AptVersionRootFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, [&](string_t root) -> optional<string_t> {
		auto version = GetAptVersion(context, root.GetString());
		if (!version) {
			return std::nullopt;
		}
		return StringVector::AddString(result, *version);
	});
}

void RegisterPackageFunctions(ExtensionLoader &loader) {
	RegisterAptTableFunction(
	    loader, TableFunction("apt_packages", {}, AptPackagesFunction, AptPackagesBind),
	    "Installed packages, from the dpkg database: version, size, maintainer, whether they were installed on "
	    "purpose (auto_installed), held, last install time and package URL (purl)",
	    "SELECT name, version FROM apt_packages() WHERE NOT auto_installed");
	RegisterAptTableFunction(
	    loader, TableFunction("apt_dependencies", {}, AptDependenciesFunction, AptDependenciesBind),
	    "Dependencies of the installed packages (Pre-Depends, Depends, Recommends, Suggests), one row per alternative",
	    "SELECT package FROM apt_dependencies() WHERE dependency = 'libssl3t64'");
	RegisterAptTableFunction(loader, TableFunction("apt_provides", {}, AptProvidesFunction, AptProvidesBind),
	                         "Virtual packages provided by the installed packages",
	                         "SELECT package FROM apt_provides() WHERE provides = 'mail-transport-agent'");

	ScalarFunctionSet apt_version("apt_version");
	apt_version.AddFunction(ScalarFunction({}, LogicalType::VARCHAR, AptVersionFunction));
	apt_version.AddFunction(ScalarFunction({LogicalType::VARCHAR}, LogicalType::VARCHAR, AptVersionRootFunction));
	CreateScalarFunctionInfo info(std::move(apt_version));
	FunctionDescription without_root;
	without_root.description = "Installed version of apt, NULL when apt is not installed";
	without_root.examples.emplace_back("SELECT apt_version()");
	FunctionDescription with_root;
	with_root.parameter_names.emplace_back("root");
	with_root.parameter_types.emplace_back(LogicalType::VARCHAR);
	with_root.description = "Installed version of apt on the system mounted at root";
	with_root.examples.emplace_back("SELECT apt_version('/mnt/image')");
	info.descriptions.push_back(std::move(without_root));
	info.descriptions.push_back(std::move(with_root));
	loader.RegisterFunction(std::move(info));
}

} // namespace duckdb
