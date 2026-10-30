#define DUCKDB_EXTENSION_MAIN

#include "apt_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>
#include <array>
#include <cstdio>
#include <fstream>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace duckdb {

static const char *DPKG_STATUS_PATH = "/var/lib/dpkg/status";
static const char *APT_EXTENDED_STATES_PATH = "/var/lib/apt/extended_states";

// Helper function to execute a shell command and capture output
static string ExecuteCommand(const string &command) {
	std::array<char, 256> buffer;
	std::string result;

	std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(command.c_str(), "r"), pclose);
	if (!pipe) {
		throw IOException("Failed to execute command: " + command);
	}

	while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
		result += buffer.data();
	}

	return result;
}

// A deb822 stanza (as found in /var/lib/dpkg/status), keyed by field name
using AptStanza = std::unordered_map<string, string>;

// Parse a deb822 file into stanzas. Returns no stanzas when the file is missing (e.g. not a Debian-based system).
// Continuation lines are appended to their field, except for Description where only the synopsis is kept.
static vector<AptStanza> ReadStanzas(const string &path) {
	vector<AptStanza> stanzas;
	std::ifstream file(path);
	if (!file) {
		return stanzas;
	}

	AptStanza current;
	string last_key;
	string line;
	while (std::getline(file, line)) {
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		if (line.empty()) {
			if (!current.empty()) {
				stanzas.push_back(std::move(current));
				current.clear();
			}
			last_key.clear();
			continue;
		}
		if (line[0] == ' ' || line[0] == '\t') {
			if (!last_key.empty() && last_key != "Description") {
				string continuation = line;
				StringUtil::Trim(continuation);
				current[last_key] += " " + continuation;
			}
			continue;
		}
		auto colon = line.find(':');
		if (colon == string::npos) {
			continue;
		}
		last_key = line.substr(0, colon);
		string value = line.substr(colon + 1);
		StringUtil::Trim(value);
		current[last_key] = value;
	}
	if (!current.empty()) {
		stanzas.push_back(std::move(current));
	}
	return stanzas;
}

static string GetField(const AptStanza &stanza, const string &key) {
	auto entry = stanza.find(key);
	return entry == stanza.end() ? string() : entry->second;
}

// Empty strings become NULL
static Value StringOrNull(const string &value) {
	return value.empty() ? Value(LogicalType::VARCHAR) : Value(value);
}

struct AptPackage {
	string name;
	string version;
	string architecture;
	string section;
	string priority;
	string maintainer;
	string homepage;
	string source;
	string description;
	string multi_arch;
	string depends;
	string pre_depends;
	string recommends;
	string suggests;
	string provides;
	int64_t installed_size_kb;
	bool auto_installed;
};

// Packages marked as automatically installed (as dependencies) by apt, keyed by "name|architecture"
static std::unordered_set<string> ReadAutoInstalled() {
	std::unordered_set<string> result;
	for (auto &stanza : ReadStanzas(APT_EXTENDED_STATES_PATH)) {
		if (GetField(stanza, "Auto-Installed") == "1") {
			result.insert(GetField(stanza, "Package") + "|" + GetField(stanza, "Architecture"));
		}
	}
	return result;
}

// All fully installed packages. Packages that are only removed with leftover config files are skipped.
static vector<AptPackage> ReadInstalledPackages() {
	vector<AptPackage> packages;
	auto auto_installed = ReadAutoInstalled();

	for (auto &stanza : ReadStanzas(DPKG_STATUS_PATH)) {
		// Status is "<want> <flag> <state>", e.g. "install ok installed"
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

struct AptLocalState : public LocalTableFunctionState {
	idx_t batch_index = 0;
};

static unique_ptr<LocalTableFunctionState> AptInitLocal(ExecutionContext &context, TableFunctionInitInput &input,
                                                        GlobalTableFunctionState *global_state) {
	return make_uniq<AptLocalState>();
}

//===--------------------------------------------------------------------===//
// apt_version()
//===--------------------------------------------------------------------===//
static void AptVersionFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	// First line looks like "apt 2.7.14 (amd64)"
	string output = ExecuteCommand("LC_ALL=C apt --version 2>/dev/null | head -n 1");
	StringUtil::Trim(output);
	auto parts = StringUtil::Split(output, ' ');
	string version = parts.size() >= 2 ? parts[1] : "";

	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	if (version.empty()) {
		ConstantVector::SetNull(result, true);
		return;
	}
	auto result_data = ConstantVector::GetData<string_t>(result);
	result_data[0] = StringVector::AddString(result, version);
}

//===--------------------------------------------------------------------===//
// apt_packages()
//===--------------------------------------------------------------------===//
struct AptPackagesBindData : public TableFunctionData {
	vector<AptPackage> packages;
};

static unique_ptr<FunctionData> AptPackagesBind(ClientContext &context, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<AptPackagesBindData>();
	result->packages = ReadInstalledPackages();

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

	return std::move(result);
}

static void AptPackagesFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.bind_data->CastNoConst<AptPackagesBindData>();
	auto &local_state = data_p.local_state->Cast<AptLocalState>();
	idx_t count = 0;

	for (idx_t i = local_state.batch_index; i < data.packages.size() && count < STANDARD_VECTOR_SIZE; i++) {
		auto &pkg = data.packages[i];
		output.SetValue(0, count, Value(pkg.name));
		output.SetValue(1, count, Value(pkg.version));
		output.SetValue(2, count, Value(pkg.architecture));
		output.SetValue(3, count, StringOrNull(pkg.section));
		output.SetValue(4, count, StringOrNull(pkg.priority));
		output.SetValue(5, count, Value::BIGINT(pkg.installed_size_kb));
		output.SetValue(6, count, StringOrNull(pkg.maintainer));
		output.SetValue(7, count, StringOrNull(pkg.homepage));
		output.SetValue(8, count, Value(pkg.source));
		output.SetValue(9, count, StringOrNull(pkg.description));
		output.SetValue(10, count, StringOrNull(pkg.multi_arch));
		output.SetValue(11, count, Value::BOOLEAN(pkg.auto_installed));
		count++;
	}

	local_state.batch_index += count;
	output.SetCardinality(count);
}

//===--------------------------------------------------------------------===//
// apt_dependencies()
//===--------------------------------------------------------------------===//
struct AptDependency {
	string package;
	string architecture;
	string relation;
	string dependency;
	string version_constraint;
	bool alternative;
};

struct AptDependenciesBindData : public TableFunctionData {
	vector<AptDependency> dependencies;
};

// Parse a relationship field such as "libc6 (>= 2.34), debconf | debconf-2.0" into one row per alternative
static void ParseRelation(const AptPackage &pkg, const string &relation, const string &field,
                          vector<AptDependency> &out) {
	for (auto &group : StringUtil::Split(field, ',')) {
		auto alternatives = StringUtil::Split(group, '|');
		for (auto &alt : alternatives) {
			string entry = alt;
			StringUtil::Trim(entry);
			if (entry.empty()) {
				continue;
			}
			AptDependency dep;
			dep.package = pkg.name;
			dep.architecture = pkg.architecture;
			dep.relation = relation;
			dep.alternative = alternatives.size() > 1;

			auto paren = entry.find('(');
			if (paren != string::npos) {
				auto close = entry.find(')', paren);
				dep.version_constraint = entry.substr(paren + 1, close == string::npos ? string::npos : close - paren - 1);
				StringUtil::Trim(dep.version_constraint);
				entry = entry.substr(0, paren);
			}
			// Drop architecture restrictions such as "[amd64]" or "<!nocheck>", and qualifiers such as ":any"
			auto bracket = entry.find_first_of("[<:");
			if (bracket != string::npos) {
				entry = entry.substr(0, bracket);
			}
			StringUtil::Trim(entry);
			dep.dependency = entry;
			out.push_back(std::move(dep));
		}
	}
}

static unique_ptr<FunctionData> AptDependenciesBind(ClientContext &context, TableFunctionBindInput &input,
                                                    vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<AptDependenciesBindData>();
	for (auto &pkg : ReadInstalledPackages()) {
		ParseRelation(pkg, "Pre-Depends", pkg.pre_depends, result->dependencies);
		ParseRelation(pkg, "Depends", pkg.depends, result->dependencies);
		ParseRelation(pkg, "Recommends", pkg.recommends, result->dependencies);
		ParseRelation(pkg, "Suggests", pkg.suggests, result->dependencies);
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
	auto &data = data_p.bind_data->CastNoConst<AptDependenciesBindData>();
	auto &local_state = data_p.local_state->Cast<AptLocalState>();
	idx_t count = 0;

	for (idx_t i = local_state.batch_index; i < data.dependencies.size() && count < STANDARD_VECTOR_SIZE; i++) {
		auto &dep = data.dependencies[i];
		output.SetValue(0, count, Value(dep.package));
		output.SetValue(1, count, Value(dep.architecture));
		output.SetValue(2, count, Value(dep.relation));
		output.SetValue(3, count, Value(dep.dependency));
		output.SetValue(4, count, StringOrNull(dep.version_constraint));
		output.SetValue(5, count, Value::BOOLEAN(dep.alternative));
		count++;
	}

	local_state.batch_index += count;
	output.SetCardinality(count);
}

//===--------------------------------------------------------------------===//
// apt_provides()
//===--------------------------------------------------------------------===//
static unique_ptr<FunctionData> AptProvidesBind(ClientContext &context, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<AptDependenciesBindData>();
	for (auto &pkg : ReadInstalledPackages()) {
		ParseRelation(pkg, "Provides", pkg.provides, result->dependencies);
	}
	// Provides only allows exact versions, e.g. "dbus-system-bus (= 1.16.2)"
	for (auto &dep : result->dependencies) {
		if (StringUtil::StartsWith(dep.version_constraint, "=")) {
			dep.version_constraint = dep.version_constraint.substr(1);
			StringUtil::Trim(dep.version_constraint);
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
	auto &data = data_p.bind_data->CastNoConst<AptDependenciesBindData>();
	auto &local_state = data_p.local_state->Cast<AptLocalState>();
	idx_t count = 0;

	for (idx_t i = local_state.batch_index; i < data.dependencies.size() && count < STANDARD_VECTOR_SIZE; i++) {
		auto &dep = data.dependencies[i];
		output.SetValue(0, count, Value(dep.package));
		output.SetValue(1, count, Value(dep.architecture));
		output.SetValue(2, count, Value(dep.dependency));
		output.SetValue(3, count, StringOrNull(dep.version_constraint));
		count++;
	}

	local_state.batch_index += count;
	output.SetCardinality(count);
}

//===--------------------------------------------------------------------===//
// apt_upgradable()
//===--------------------------------------------------------------------===//
struct AptUpgradable {
	string name;
	string repository;
	string new_version;
	string architecture;
	string current_version;
};

struct AptUpgradableBindData : public TableFunctionData {
	vector<AptUpgradable> upgradable;
};

static unique_ptr<FunctionData> AptUpgradableBind(ClientContext &context, TableFunctionBindInput &input,
                                                  vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<AptUpgradableBindData>();

	// Lines look like: "curl/noble-updates,noble-security 8.5.0-2 amd64 [upgradable from: 8.5.0-1]"
	string output = ExecuteCommand("LC_ALL=C apt list --upgradable 2>/dev/null");
	for (auto &line : StringUtil::Split(output, '\n')) {
		static const string MARKER = "[upgradable from: ";
		auto slash = line.find('/');
		auto marker = line.find(MARKER);
		if (slash == string::npos || marker == string::npos) {
			continue;
		}
		auto fields = StringUtil::Split(line.substr(slash + 1, marker - slash - 1), ' ');
		if (fields.size() < 3) {
			continue;
		}
		AptUpgradable entry;
		entry.name = line.substr(0, slash);
		entry.repository = fields[0];
		entry.new_version = fields[1];
		entry.architecture = fields[2];
		auto start = marker + MARKER.size();
		auto end = line.find(']', start);
		entry.current_version = line.substr(start, end == string::npos ? string::npos : end - start);
		result->upgradable.push_back(std::move(entry));
	}

	names.emplace_back("name");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("repository");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("current_version");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("new_version");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("architecture");
	return_types.emplace_back(LogicalType::VARCHAR);

	return std::move(result);
}

static void AptUpgradableFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.bind_data->CastNoConst<AptUpgradableBindData>();
	auto &local_state = data_p.local_state->Cast<AptLocalState>();
	idx_t count = 0;

	for (idx_t i = local_state.batch_index; i < data.upgradable.size() && count < STANDARD_VECTOR_SIZE; i++) {
		auto &entry = data.upgradable[i];
		output.SetValue(0, count, Value(entry.name));
		output.SetValue(1, count, Value(entry.repository));
		output.SetValue(2, count, Value(entry.current_version));
		output.SetValue(3, count, Value(entry.new_version));
		output.SetValue(4, count, Value(entry.architecture));
		count++;
	}

	local_state.batch_index += count;
	output.SetCardinality(count);
}

static void LoadInternal(ExtensionLoader &loader) {
	ScalarFunction apt_version("apt_version", {}, LogicalType::VARCHAR, AptVersionFunction);
	loader.RegisterFunction(apt_version);

	TableFunction apt_packages("apt_packages", {}, AptPackagesFunction, AptPackagesBind);
	apt_packages.init_local = AptInitLocal;
	loader.RegisterFunction(apt_packages);

	TableFunction apt_dependencies("apt_dependencies", {}, AptDependenciesFunction, AptDependenciesBind);
	apt_dependencies.init_local = AptInitLocal;
	loader.RegisterFunction(apt_dependencies);

	TableFunction apt_provides("apt_provides", {}, AptProvidesFunction, AptProvidesBind);
	apt_provides.init_local = AptInitLocal;
	loader.RegisterFunction(apt_provides);

	TableFunction apt_upgradable("apt_upgradable", {}, AptUpgradableFunction, AptUpgradableBind);
	apt_upgradable.init_local = AptInitLocal;
	loader.RegisterFunction(apt_upgradable);
}

void AptExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string AptExtension::Name() {
	return "apt";
}

std::string AptExtension::Version() const {
#ifdef EXT_VERSION_APT
	return EXT_VERSION_APT;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(apt, loader) {
	duckdb::LoadInternal(loader);
}
}
