#define DUCKDB_EXTENSION_MAIN

#include "apt_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
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

static constexpr const char *DPKG_STATUS_PATH = "/var/lib/dpkg/status";
static constexpr const char *APT_EXTENDED_STATES_PATH = "/var/lib/apt/extended_states";
static constexpr const char *APT_SOURCES_LIST_PATH = "/etc/apt/sources.list";
static constexpr const char *APT_SOURCES_DIR = "/etc/apt/sources.list.d";
static constexpr const char *APT_LISTS_DIR = "/var/lib/apt/lists";

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
		if (line[0] == '#') {
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

// Empty lists become NULL
static Value StringListOrNull(const vector<string> &values) {
	if (values.empty()) {
		return Value(LogicalType::LIST(LogicalType::VARCHAR));
	}
	vector<Value> list;
	for (auto &value : values) {
		list.emplace_back(value);
	}
	return Value::LIST(LogicalType::VARCHAR, std::move(list));
}

static vector<string> SplitWhitespace(const string &input) {
	vector<string> result;
	string current;
	for (auto c : input) {
		if (StringUtil::CharacterIsSpace(c)) {
			if (!current.empty()) {
				result.push_back(std::move(current));
				current.clear();
			}
		} else {
			current += c;
		}
	}
	if (!current.empty()) {
		result.push_back(std::move(current));
	}
	return result;
}

// Optional "root" parameter: the directory paths are resolved under, e.g. a mounted image or test fixtures
static string GetRoot(TableFunctionBindInput &input) {
	auto entry = input.named_parameters.find("root");
	if (entry == input.named_parameters.end() || entry->second.IsNull()) {
		return "";
	}
	auto root = StringValue::Get(entry->second);
	while (StringUtil::EndsWith(root, "/")) {
		root.pop_back();
	}
	return root;
}

// Sorted names of the regular files of a directory (empty when it does not exist)
static vector<string> ListDirectory(FileSystem &fs, const string &directory) {
	vector<string> files;
	fs.ListFiles(directory, [&](const string &name, bool is_dir) {
		if (!is_dir) {
			files.push_back(name);
		}
	});
	std::sort(files.begin(), files.end());
	return files;
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
static std::unordered_set<string> ReadAutoInstalled(const string &root) {
	std::unordered_set<string> result;
	for (auto &stanza : ReadStanzas(root + APT_EXTENDED_STATES_PATH)) {
		if (GetField(stanza, "Auto-Installed") == "1") {
			result.insert(GetField(stanza, "Package") + "|" + GetField(stanza, "Architecture"));
		}
	}
	return result;
}

// All fully installed packages. Packages that are only removed with leftover config files are skipped.
static vector<AptPackage> ReadInstalledPackages(const string &root) {
	vector<AptPackage> packages;
	auto auto_installed = ReadAutoInstalled(root);

	for (auto &stanza : ReadStanzas(root + DPKG_STATUS_PATH)) {
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
	result->packages = ReadInstalledPackages(GetRoot(input));

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
		count++;
	}

	local_state.batch_index += count;
	output.CheckCardinality(count);
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
				dep.version_constraint =
				    entry.substr(paren + 1, close == string::npos ? string::npos : close - paren - 1);
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
	for (auto &pkg : ReadInstalledPackages(GetRoot(input))) {
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
		output.data[0].Append(Value(dep.package));
		output.data[1].Append(Value(dep.architecture));
		output.data[2].Append(Value(dep.relation));
		output.data[3].Append(Value(dep.dependency));
		output.data[4].Append(StringOrNull(dep.version_constraint));
		output.data[5].Append(Value::BOOLEAN(dep.alternative));
		count++;
	}

	local_state.batch_index += count;
	output.CheckCardinality(count);
}

//===--------------------------------------------------------------------===//
// apt_provides()
//===--------------------------------------------------------------------===//
static unique_ptr<FunctionData> AptProvidesBind(ClientContext &context, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<AptDependenciesBindData>();
	for (auto &pkg : ReadInstalledPackages(GetRoot(input))) {
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
		output.data[0].Append(Value(dep.package));
		output.data[1].Append(Value(dep.architecture));
		output.data[2].Append(Value(dep.dependency));
		output.data[3].Append(StringOrNull(dep.version_constraint));
		count++;
	}

	local_state.batch_index += count;
	output.CheckCardinality(count);
}

//===--------------------------------------------------------------------===//
// apt_sources()
//===--------------------------------------------------------------------===//
struct AptSource {
	string file;
	string format;
	bool enabled;
	string type;
	string uri;
	string suite;
	vector<string> components;
	vector<string> architectures;
	string signed_by;
	bool trusted;
};

struct AptSourcesBindData : public TableFunctionData {
	vector<AptSource> sources;
};

// One-line format: "deb [arch=amd64 signed-by=/usr/share/keyrings/x.gpg] http://host/path suite main contrib"
static void ReadOneLineSources(const string &path, vector<AptSource> &out) {
	std::ifstream file(path);
	string line;
	while (std::getline(file, line)) {
		auto comment = line.find('#');
		if (comment != string::npos) {
			line = line.substr(0, comment);
		}
		auto tokens = SplitWhitespace(line);
		if (tokens.size() < 3 || (tokens[0] != "deb" && tokens[0] != "deb-src")) {
			continue;
		}
		AptSource source;
		source.file = path;
		source.format = "one-line";
		source.enabled = true;
		source.trusted = false;
		source.type = tokens[0];

		idx_t pos = 1;
		if (StringUtil::StartsWith(tokens[pos], "[")) {
			string options;
			for (; pos < tokens.size(); pos++) {
				options += " " + tokens[pos];
				if (StringUtil::EndsWith(tokens[pos], "]")) {
					pos++;
					break;
				}
			}
			StringUtil::Trim(options);
			for (auto &option : SplitWhitespace(options.substr(1, options.size() - 2))) {
				auto equals = option.find('=');
				if (equals == string::npos) {
					continue;
				}
				auto key = option.substr(0, equals);
				auto value = option.substr(equals + 1);
				if (key == "arch") {
					source.architectures = StringUtil::Split(value, ',');
				} else if (key == "signed-by") {
					source.signed_by = value;
				} else if (key == "trusted") {
					source.trusted = value == "yes";
				}
			}
		}
		if (pos + 1 >= tokens.size()) {
			continue;
		}
		source.uri = tokens[pos];
		source.suite = tokens[pos + 1];
		source.components.assign(tokens.begin() + NumericCast<int64_t>(pos + 2), tokens.end());
		out.push_back(std::move(source));
	}
}

// deb822 format, one stanza per group of Types x URIs x Suites
static void ReadDeb822Sources(const string &path, vector<AptSource> &out) {
	for (auto &stanza : ReadStanzas(path)) {
		auto signed_by = GetField(stanza, "Signed-By");
		if (signed_by.find("BEGIN PGP PUBLIC KEY BLOCK") != string::npos) {
			signed_by = "(inline key)";
		}
		auto enabled = StringUtil::Lower(GetField(stanza, "Enabled"));
		auto trusted = StringUtil::Lower(GetField(stanza, "Trusted"));
		for (auto &type : SplitWhitespace(GetField(stanza, "Types"))) {
			for (auto &uri : SplitWhitespace(GetField(stanza, "URIs"))) {
				for (auto &suite : SplitWhitespace(GetField(stanza, "Suites"))) {
					AptSource source;
					source.file = path;
					source.format = "deb822";
					source.enabled = enabled != "no";
					source.type = type;
					source.uri = uri;
					source.suite = suite;
					source.components = SplitWhitespace(GetField(stanza, "Components"));
					source.architectures = SplitWhitespace(GetField(stanza, "Architectures"));
					source.signed_by = signed_by;
					source.trusted = trusted == "yes";
					out.push_back(std::move(source));
				}
			}
		}
	}
}

static unique_ptr<FunctionData> AptSourcesBind(ClientContext &context, TableFunctionBindInput &input,
                                               vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<AptSourcesBindData>();
	auto &fs = FileSystem::GetFileSystem(context);

	// apt ignores files of sources.list.d with other extensions (e.g. ".disabled")
	auto root = GetRoot(input);
	ReadOneLineSources(root + APT_SOURCES_LIST_PATH, result->sources);
	for (auto &name : ListDirectory(fs, root + APT_SOURCES_DIR)) {
		auto path = root + APT_SOURCES_DIR + "/" + name;
		if (StringUtil::EndsWith(name, ".list")) {
			ReadOneLineSources(path, result->sources);
		} else if (StringUtil::EndsWith(name, ".sources")) {
			ReadDeb822Sources(path, result->sources);
		}
	}

	names.emplace_back("file");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("format");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("enabled");
	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("type");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("uri");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("suite");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("components");
	return_types.emplace_back(LogicalType::LIST(LogicalType::VARCHAR));
	names.emplace_back("architectures");
	return_types.emplace_back(LogicalType::LIST(LogicalType::VARCHAR));
	names.emplace_back("signed_by");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("trusted");
	return_types.emplace_back(LogicalType::BOOLEAN);

	return std::move(result);
}

static void AptSourcesFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.bind_data->CastNoConst<AptSourcesBindData>();
	auto &local_state = data_p.local_state->Cast<AptLocalState>();
	idx_t count = 0;

	for (idx_t i = local_state.batch_index; i < data.sources.size() && count < STANDARD_VECTOR_SIZE; i++) {
		auto &source = data.sources[i];
		output.data[0].Append(Value(source.file));
		output.data[1].Append(Value(source.format));
		output.data[2].Append(Value::BOOLEAN(source.enabled));
		output.data[3].Append(Value(source.type));
		output.data[4].Append(Value(source.uri));
		output.data[5].Append(Value(source.suite));
		output.data[6].Append(StringListOrNull(source.components));
		output.data[7].Append(StringListOrNull(source.architectures));
		output.data[8].Append(StringOrNull(source.signed_by));
		output.data[9].Append(Value::BOOLEAN(source.trusted));
		count++;
	}

	local_state.batch_index += count;
	output.CheckCardinality(count);
}

//===--------------------------------------------------------------------===//
// apt_package_origins()
//===--------------------------------------------------------------------===//
struct AptPackageOrigin {
	string name;
	string architecture;
	string installed_version;
	string available_version;
	string origin;
	string label;
	string suite;
	string codename;
	string component;
	string site;
};

struct AptPackageOriginsBindData : public TableFunctionData {
	vector<AptPackageOrigin> origins;
};

// Repository metadata shared by all packages of an index file
struct AptRepository {
	string origin;
	string label;
	string suite;
	string codename;
	string component;
	string site;
};

// Index files are named "<site>_dists_<suite>_<component>_binary-<arch>_Packages", with "/" encoded as "_".
// The suite is found by probing for its "<site>_dists_<suite>_InRelease" (or "_Release") file.
static AptRepository ResolveRepository(FileSystem &fs, const string &lists_dir, const string &index_name) {
	AptRepository repo;
	auto base = index_name.substr(0, index_name.size() - string("_Packages").size());
	auto dists = base.find("_dists_");
	if (dists == string::npos) {
		repo.site = StringUtil::Replace(base, "_", "/");
		return repo;
	}
	auto prefix = base.substr(0, dists);
	repo.site = StringUtil::Replace(prefix, "_", "/");

	auto rest = base.substr(dists + string("_dists_").size());
	auto binary = rest.rfind("_binary-");
	if (binary != string::npos) {
		rest = rest.substr(0, binary);
	}
	for (auto split = rest.find('_'); split != string::npos; split = rest.find('_', split + 1)) {
		auto release_base = lists_dir + "/" + prefix + "_dists_" + rest.substr(0, split);
		string release_path;
		if (fs.FileExists(release_base + "_InRelease")) {
			release_path = release_base + "_InRelease";
		} else if (fs.FileExists(release_base + "_Release")) {
			release_path = release_base + "_Release";
		} else {
			continue;
		}
		repo.component = StringUtil::Replace(rest.substr(split + 1), "_", "/");
		for (auto &stanza : ReadStanzas(release_path)) {
			if (stanza.count("Suite") || stanza.count("Origin")) {
				repo.origin = GetField(stanza, "Origin");
				repo.label = GetField(stanza, "Label");
				repo.suite = GetField(stanza, "Suite");
				repo.codename = GetField(stanza, "Codename");
				break;
			}
		}
		break;
	}
	return repo;
}

static unique_ptr<FunctionData> AptPackageOriginsBind(ClientContext &context, TableFunctionBindInput &input,
                                                      vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<AptPackageOriginsBindData>();
	auto &fs = FileSystem::GetFileSystem(context);
	auto root = GetRoot(input);
	auto lists_dir = root + APT_LISTS_DIR;

	std::unordered_map<string, string> installed;
	for (auto &pkg : ReadInstalledPackages(root)) {
		installed[pkg.name + "|" + pkg.architecture] = pkg.version;
	}

	// "all" packages appear in the index of every architecture: keep each origin once
	std::unordered_set<string> seen;
	for (auto &index_name : ListDirectory(fs, lists_dir)) {
		if (!StringUtil::EndsWith(index_name, "_Packages")) {
			continue;
		}
		auto repo = ResolveRepository(fs, lists_dir, index_name);

		// Index files are large, so only the three fields needed are extracted
		std::ifstream file(lists_dir + "/" + index_name);
		string line, name, version, architecture;
		auto flush = [&]() {
			auto entry = installed.find(name + "|" + architecture);
			if (!name.empty() && entry != installed.end()) {
				auto key = name + "|" + architecture + "|" + version + "|" + repo.site + "|" + repo.suite + "|" +
				           repo.component;
				if (seen.insert(key).second) {
					AptPackageOrigin origin;
					origin.name = name;
					origin.architecture = architecture;
					origin.installed_version = entry->second;
					origin.available_version = version;
					origin.origin = repo.origin;
					origin.label = repo.label;
					origin.suite = repo.suite;
					origin.codename = repo.codename;
					origin.component = repo.component;
					origin.site = repo.site;
					result->origins.push_back(std::move(origin));
				}
			}
			name.clear();
			version.clear();
			architecture.clear();
		};
		while (std::getline(file, line)) {
			if (line.empty()) {
				flush();
			} else if (StringUtil::StartsWith(line, "Package: ")) {
				name = line.substr(9);
			} else if (StringUtil::StartsWith(line, "Version: ")) {
				version = line.substr(9);
			} else if (StringUtil::StartsWith(line, "Architecture: ")) {
				architecture = line.substr(14);
			}
		}
		flush();
	}

	names.emplace_back("name");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("architecture");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("installed_version");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("available_version");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("is_installed_version");
	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("origin");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("label");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("suite");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("codename");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("component");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("site");
	return_types.emplace_back(LogicalType::VARCHAR);

	return std::move(result);
}

static void AptPackageOriginsFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.bind_data->CastNoConst<AptPackageOriginsBindData>();
	auto &local_state = data_p.local_state->Cast<AptLocalState>();
	idx_t count = 0;

	for (idx_t i = local_state.batch_index; i < data.origins.size() && count < STANDARD_VECTOR_SIZE; i++) {
		auto &origin = data.origins[i];
		output.data[0].Append(Value(origin.name));
		output.data[1].Append(Value(origin.architecture));
		output.data[2].Append(Value(origin.installed_version));
		output.data[3].Append(Value(origin.available_version));
		output.data[4].Append(Value::BOOLEAN(origin.installed_version == origin.available_version));
		output.data[5].Append(StringOrNull(origin.origin));
		output.data[6].Append(StringOrNull(origin.label));
		output.data[7].Append(StringOrNull(origin.suite));
		output.data[8].Append(StringOrNull(origin.codename));
		output.data[9].Append(StringOrNull(origin.component));
		output.data[10].Append(Value(origin.site));
		count++;
	}

	local_state.batch_index += count;
	output.CheckCardinality(count);
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
		output.data[0].Append(Value(entry.name));
		output.data[1].Append(Value(entry.repository));
		output.data[2].Append(Value(entry.current_version));
		output.data[3].Append(Value(entry.new_version));
		output.data[4].Append(Value(entry.architecture));
		count++;
	}

	local_state.batch_index += count;
	output.CheckCardinality(count);
}

// Table functions reading apt/dpkg files accept an optional "root" directory
static void RegisterFileTableFunction(ExtensionLoader &loader, TableFunction function) {
	function.init_local = AptInitLocal;
	function.GetSignature().WithTypedKwargs("options",
	                                        [](TypedKwargs &options) { options.Add("root", LogicalType::VARCHAR); });
	loader.RegisterFunction(function);
}

static void LoadInternal(ExtensionLoader &loader) {
	ScalarFunction apt_version("apt_version", {}, LogicalType::VARCHAR, AptVersionFunction);
	loader.RegisterFunction(apt_version);

	RegisterFileTableFunction(loader, TableFunction("apt_packages", {}, AptPackagesFunction, AptPackagesBind));
	RegisterFileTableFunction(loader,
	                          TableFunction("apt_dependencies", {}, AptDependenciesFunction, AptDependenciesBind));
	RegisterFileTableFunction(loader, TableFunction("apt_provides", {}, AptProvidesFunction, AptProvidesBind));
	RegisterFileTableFunction(loader, TableFunction("apt_sources", {}, AptSourcesFunction, AptSourcesBind));
	RegisterFileTableFunction(
	    loader, TableFunction("apt_package_origins", {}, AptPackageOriginsFunction, AptPackageOriginsBind));

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
