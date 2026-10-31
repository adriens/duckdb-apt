#include "apt_common.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"

namespace duckdb {

bool AptRepository::IsSecurity() const {
	return StringUtil::EndsWith(suite, "-security") || StringUtil::EndsWith(codename, "-security") ||
	       StringUtil::Contains(StringUtil::Lower(label), "security");
}

// apt names index files after their URI: "/" becomes "_", and "_" or ":" are percent-encoded ("%5f", "%3a")
static string DecodeListName(const string &encoded) {
	auto decoded = StringUtil::Replace(encoded, "_", "/");
	string result;
	for (idx_t i = 0; i < decoded.size(); i++) {
		if (decoded[i] == '%' && i + 2 < decoded.size() && StringUtil::CharacterIsHex(decoded[i + 1]) &&
		    StringUtil::CharacterIsHex(decoded[i + 2])) {
			result += static_cast<char>(std::stoi(decoded.substr(i + 1, 2), nullptr, 16));
			i += 2;
		} else {
			result += decoded[i];
		}
	}
	return result;
}

// Release dates follow RFC 2822, e.g. "Sat, 10 Feb 2024 10:00:00 UTC"
static timestamp_t ParseReleaseDate(const string &value) {
	static const char *MONTHS[] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};
	auto tokens = SplitWhitespace(value);
	for (idx_t i = 1; i + 2 < tokens.size(); i++) {
		for (int32_t month = 0; month < 12; month++) {
			if (StringUtil::Lower(tokens[i]) != MONTHS[month]) {
				continue;
			}
			auto day = ExtractNumbers(tokens[i - 1]);
			auto year = ExtractNumbers(tokens[i + 1]);
			auto time = ExtractNumbers(tokens[i + 2]);
			if (day.size() != 1 || year.size() != 1 || time.size() != 3 || !Date::IsValid(year[0], month + 1, day[0]) ||
			    !Time::IsValidTime(time[0], time[1], time[2], 0)) {
				return timestamp_t::infinity();
			}
			auto result = Timestamp::FromDatetime(Date::FromDate(year[0], month + 1, day[0]),
			                                      Time::FromTime(time[0], time[1], time[2]));
			// Optional numeric offset, e.g. "+0100": convert to UTC
			if (i + 3 < tokens.size() && tokens[i + 3].size() == 5 &&
			    (tokens[i + 3][0] == '+' || tokens[i + 3][0] == '-')) {
				auto offset = ExtractNumbers(tokens[i + 3]);
				if (offset.size() == 1) {
					auto minutes = (offset[0] / 100) * 60 + offset[0] % 100;
					auto sign = tokens[i + 3][0] == '+' ? -1 : 1;
					result.value += sign * int64_t(minutes) * Interval::MICROS_PER_MINUTE;
				}
			}
			return result;
		}
	}
	return timestamp_t::infinity();
}

// Index files are named "<site>_dists_<suite>_<component>_binary-<arch>_Packages[.ext]". The suite is found by
// probing for its "<site>_dists_<suite>_InRelease" (or "_Release") file, whose fields describe the repository.
static AptRepository ResolveRepository(FileSystem &fs, const string &lists_dir, const string &base, string &key) {
	AptRepository repo;
	auto dists = base.find("_dists_");
	if (dists == string::npos) {
		repo.site = DecodeListName(base);
		key = base;
		return repo;
	}
	auto prefix = base.substr(0, dists);
	repo.site = DecodeListName(prefix);

	auto rest = base.substr(dists + string("_dists_").size());
	auto binary = rest.rfind("_binary-");
	string architecture;
	if (binary != string::npos) {
		architecture = rest.substr(binary + string("_binary-").size());
		rest = rest.substr(0, binary);
	}
	if (!architecture.empty()) {
		repo.architectures.push_back(architecture);
	}
	key = prefix + "|" + rest;
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
		repo.component = DecodeListName(rest.substr(split + 1));
		for (auto &stanza : ReadStanzas(fs, release_path)) {
			if (stanza.count("Suite") || stanza.count("Origin")) {
				repo.origin = GetField(stanza, "Origin");
				repo.label = GetField(stanza, "Label");
				repo.suite = GetField(stanza, "Suite");
				repo.codename = GetField(stanza, "Codename");
				repo.date = ParseReleaseDate(GetField(stanza, "Date"));
				repo.valid_until = ParseReleaseDate(GetField(stanza, "Valid-Until"));
				repo.not_automatic = StringUtil::Lower(GetField(stanza, "NotAutomatic")) == "yes";
				repo.but_automatic_upgrades = StringUtil::Lower(GetField(stanza, "ButAutomaticUpgrades")) == "yes";
				break;
			}
		}
		break;
	}
	return repo;
}

// The compression of an index file, or false if the file is not a Packages index
static bool GetIndexCompression(const string &name, string &base, AptCompression &compression) {
	auto packages = name.rfind("_Packages");
	if (packages == string::npos) {
		return false;
	}
	base = name.substr(0, packages);
	auto extension = name.substr(packages + string("_Packages").size());
	if (extension.empty()) {
		compression = AptCompression::NONE;
	} else if (extension == ".gz") {
		compression = AptCompression::GZIP;
	} else if (extension == ".zst") {
		compression = AptCompression::ZSTD;
	} else if (extension == ".lz4") {
		compression = AptCompression::LZ4;
	} else if (extension[0] == '.' && extension.find('_') == string::npos) {
		// Skipping it would report its packages as provided by no repository: fail instead
		throw NotImplementedException(
		    "Repository index \"%s\" is compressed with \"%s\", which is not supported (only gzip, zstd and lz4 "
		    "are): set Acquire::CompressionTypes::Order to one of them in the apt configuration",
		    name, extension.substr(1));
	} else {
		return false;
	}
	return true;
}

AptIndexes ReadIndexes(FileSystem &fs, const string &root, const std::unordered_set<string> &wanted) {
	AptIndexes result;
	auto lists_dir = root + APT_LISTS_DIR;
	std::unordered_map<string, idx_t> repository_ids;
	vector<std::unordered_set<string>> repository_packages;
	// "all" packages appear in the index of every architecture: keep each entry once
	std::unordered_set<string> seen;

	for (auto &index_name : ListDirectory(fs, lists_dir)) {
		string base;
		AptCompression compression;
		if (!GetIndexCompression(index_name, base, compression)) {
			continue;
		}
		string key;
		auto repo = ResolveRepository(fs, lists_dir, base, key);
		auto existing = repository_ids.find(key);
		idx_t repository_id;
		if (existing == repository_ids.end()) {
			repository_id = result.repositories.size();
			repository_ids[key] = repository_id;
			result.repositories.push_back(std::move(repo));
			repository_packages.emplace_back();
		} else {
			repository_id = existing->second;
			auto &architectures = result.repositories[repository_id].architectures;
			for (auto &architecture : repo.architectures) {
				if (std::find(architectures.begin(), architectures.end(), architecture) == architectures.end()) {
					architectures.push_back(architecture);
				}
			}
		}

		// Index files are large, so only the three fields needed are extracted
		AptLineReader reader(fs, lists_dir + "/" + index_name, compression);
		string line, name, version, architecture;
		auto flush = [&]() {
			if (!name.empty()) {
				auto package_key = name + "|" + architecture;
				repository_packages[repository_id].insert(package_key);
				if (wanted.count(package_key) &&
				    seen.insert(package_key + "|" + version + "|" + to_string(repository_id)).second) {
					result.entries.push_back({name, architecture, version, repository_id});
				}
			}
			name.clear();
			version.clear();
			architecture.clear();
		};
		while (reader.ReadLine(line)) {
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
	for (idx_t i = 0; i < result.repositories.size(); i++) {
		result.repositories[i].packages = NumericCast<int64_t>(repository_packages[i].size());
	}
	return result;
}

static std::unordered_set<string> InstalledKeys(const vector<AptPackage> &packages) {
	std::unordered_set<string> keys;
	for (auto &pkg : packages) {
		keys.insert(pkg.name + "|" + pkg.architecture);
	}
	return keys;
}

//===--------------------------------------------------------------------===//
// apt_repositories()
//===--------------------------------------------------------------------===//
struct AptRepositoriesBindData : public TableFunctionData {
	vector<AptRepository> repositories;
};

static unique_ptr<FunctionData> AptRepositoriesBind(ClientContext &context, TableFunctionBindInput &input,
                                                    vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<AptRepositoriesBindData>();
	result->repositories = ReadIndexes(FileSystem::GetFileSystem(context), GetRoot(input), {}).repositories;

	names.emplace_back("site");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("suite");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("codename");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("component");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("origin");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("label");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("architectures");
	return_types.emplace_back(LogicalType::LIST(LogicalType::VARCHAR));
	names.emplace_back("packages");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("date");
	return_types.emplace_back(LogicalType::TIMESTAMP);
	names.emplace_back("valid_until");
	return_types.emplace_back(LogicalType::TIMESTAMP);
	names.emplace_back("not_automatic");
	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("but_automatic_upgrades");
	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("is_security");
	return_types.emplace_back(LogicalType::BOOLEAN);

	return std::move(result);
}

static void AptRepositoriesFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.bind_data->CastNoConst<AptRepositoriesBindData>();
	auto &local_state = data_p.local_state->Cast<AptLocalState>();
	idx_t count = 0;

	for (idx_t i = local_state.batch_index; i < data.repositories.size() && count < STANDARD_VECTOR_SIZE; i++) {
		auto &repo = data.repositories[i];
		output.data[0].Append(Value(repo.site));
		output.data[1].Append(StringOrNull(repo.suite));
		output.data[2].Append(StringOrNull(repo.codename));
		output.data[3].Append(StringOrNull(repo.component));
		output.data[4].Append(StringOrNull(repo.origin));
		output.data[5].Append(StringOrNull(repo.label));
		output.data[6].Append(StringListOrNull(repo.architectures));
		output.data[7].Append(Value::BIGINT(repo.packages));
		output.data[8].Append(TimestampOrNull(repo.date));
		output.data[9].Append(TimestampOrNull(repo.valid_until));
		output.data[10].Append(Value::BOOLEAN(repo.not_automatic));
		output.data[11].Append(Value::BOOLEAN(repo.but_automatic_upgrades));
		output.data[12].Append(Value::BOOLEAN(repo.IsSecurity()));
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
	idx_t repository;
};

struct AptPackageOriginsBindData : public TableFunctionData {
	vector<AptRepository> repositories;
	vector<AptPackageOrigin> origins;
};

static unique_ptr<FunctionData> AptPackageOriginsBind(ClientContext &context, TableFunctionBindInput &input,
                                                      vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<AptPackageOriginsBindData>();
	auto &fs = FileSystem::GetFileSystem(context);
	auto root = GetRoot(input);

	auto packages = ReadInstalledPackages(fs, root);
	std::unordered_map<string, string> installed;
	for (auto &pkg : packages) {
		installed[pkg.name + "|" + pkg.architecture] = pkg.version;
	}
	auto indexes = ReadIndexes(fs, root, InstalledKeys(packages));
	for (auto &entry : indexes.entries) {
		result->origins.push_back({entry.name, entry.architecture, installed[entry.name + "|" + entry.architecture],
		                           entry.version, entry.repository});
	}
	result->repositories = std::move(indexes.repositories);

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
		auto &repo = data.repositories[origin.repository];
		output.data[0].Append(Value(origin.name));
		output.data[1].Append(Value(origin.architecture));
		output.data[2].Append(Value(origin.installed_version));
		output.data[3].Append(Value(origin.available_version));
		output.data[4].Append(Value::BOOLEAN(origin.installed_version == origin.available_version));
		output.data[5].Append(StringOrNull(repo.origin));
		output.data[6].Append(StringOrNull(repo.label));
		output.data[7].Append(StringOrNull(repo.suite));
		output.data[8].Append(StringOrNull(repo.codename));
		output.data[9].Append(StringOrNull(repo.component));
		output.data[10].Append(Value(repo.site));
		count++;
	}

	local_state.batch_index += count;
	output.CheckCardinality(count);
}

//===--------------------------------------------------------------------===//
// apt_upgradable()
//===--------------------------------------------------------------------===//
struct AptUpgrade {
	string name;
	string architecture;
	string current_version;
	string new_version;
	string repositories;
	bool is_security;
};

struct AptUpgradableBindData : public TableFunctionData {
	vector<AptUpgrade> upgrades;
};

// apt's default policy: repositories marked NotAutomatic (experimental) are never upgraded from, except the
// ButAutomaticUpgrades ones (backports) for packages already installed from them. Pins (apt_preferences) are
// not applied.
static unique_ptr<FunctionData> AptUpgradableBind(ClientContext &context, TableFunctionBindInput &input,
                                                  vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<AptUpgradableBindData>();
	auto &fs = FileSystem::GetFileSystem(context);
	auto root = GetRoot(input);

	auto packages = ReadInstalledPackages(fs, root);
	auto indexes = ReadIndexes(fs, root, InstalledKeys(packages));
	std::unordered_map<string, vector<idx_t>> entries_by_package;
	for (idx_t i = 0; i < indexes.entries.size(); i++) {
		auto &entry = indexes.entries[i];
		entries_by_package[entry.name + "|" + entry.architecture].push_back(i);
	}

	for (auto &pkg : packages) {
		auto &entries = entries_by_package[pkg.name + "|" + pkg.architecture];
		bool installed_from_backports = false;
		for (auto i : entries) {
			auto &repo = indexes.repositories[indexes.entries[i].repository];
			if (indexes.entries[i].version == pkg.version && repo.not_automatic && repo.but_automatic_upgrades) {
				installed_from_backports = true;
			}
		}
		auto eligible = [&](idx_t i) {
			auto &repo = indexes.repositories[indexes.entries[i].repository];
			return !repo.not_automatic || (repo.but_automatic_upgrades && installed_from_backports);
		};
		string candidate;
		for (auto i : entries) {
			if (eligible(i) &&
			    (candidate.empty() || CompareDebianVersions(indexes.entries[i].version, candidate) > 0)) {
				candidate = indexes.entries[i].version;
			}
		}
		if (candidate.empty() || CompareDebianVersions(candidate, pkg.version) <= 0) {
			continue;
		}
		AptUpgrade upgrade {pkg.name, pkg.architecture, pkg.version, candidate, "", false};
		vector<string> suites;
		for (auto i : entries) {
			auto &repo = indexes.repositories[indexes.entries[i].repository];
			if (!eligible(i) || indexes.entries[i].version != candidate) {
				continue;
			}
			upgrade.is_security = upgrade.is_security || repo.IsSecurity();
			auto suite = repo.suite.empty() ? repo.site : repo.suite;
			if (std::find(suites.begin(), suites.end(), suite) == suites.end()) {
				suites.push_back(suite);
			}
		}
		upgrade.repositories = StringUtil::Join(suites, ",");
		result->upgrades.push_back(std::move(upgrade));
	}

	names.emplace_back("name");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("architecture");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("current_version");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("new_version");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("repository");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("is_security");
	return_types.emplace_back(LogicalType::BOOLEAN);

	return std::move(result);
}

static void AptUpgradableFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.bind_data->CastNoConst<AptUpgradableBindData>();
	auto &local_state = data_p.local_state->Cast<AptLocalState>();
	idx_t count = 0;

	for (idx_t i = local_state.batch_index; i < data.upgrades.size() && count < STANDARD_VECTOR_SIZE; i++) {
		auto &upgrade = data.upgrades[i];
		output.data[0].Append(Value(upgrade.name));
		output.data[1].Append(Value(upgrade.architecture));
		output.data[2].Append(Value(upgrade.current_version));
		output.data[3].Append(Value(upgrade.new_version));
		output.data[4].Append(Value(upgrade.repositories));
		output.data[5].Append(Value::BOOLEAN(upgrade.is_security));
		count++;
	}

	local_state.batch_index += count;
	output.CheckCardinality(count);
}

void RegisterIndexFunctions(ExtensionLoader &loader) {
	RegisterAptTableFunction(
	    loader, TableFunction("apt_repositories", {}, AptRepositoriesFunction, AptRepositoriesBind),
	    "Repositories known from the indexes downloaded by apt update: origin, suite, component, number of packages, "
	    "release date and validity, and whether they are security repositories",
	    "SELECT site, suite, date FROM apt_repositories() ORDER BY date");
	RegisterAptTableFunction(
	    loader, TableFunction("apt_package_origins", {}, AptPackageOriginsFunction, AptPackageOriginsBind),
	    "Repositories publishing each installed package, with the version they publish: an installed package "
	    "without any row is unknown to every repository and never updated by apt",
	    "SELECT name FROM apt_packages() ANTI JOIN apt_package_origins() USING (name, architecture)");
	RegisterAptTableFunction(
	    loader, TableFunction("apt_upgradable", {}, AptUpgradableFunction, AptUpgradableBind),
	    "Installed packages with a newer version available, computed from the downloaded indexes with apt's default "
	    "policy (pins are not applied), and whether the update comes from a security repository",
	    "SELECT name, current_version, new_version FROM apt_upgradable() WHERE is_security");
}

} // namespace duckdb
