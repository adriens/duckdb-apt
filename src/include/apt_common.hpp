#pragma once

#include "duckdb.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <unordered_map>
#include <unordered_set>

namespace duckdb {

constexpr const char *DPKG_STATUS_PATH = "/var/lib/dpkg/status";
constexpr const char *DPKG_INFO_DIR = "/var/lib/dpkg/info";
constexpr const char *APT_EXTENDED_STATES_PATH = "/var/lib/apt/extended_states";
constexpr const char *APT_SOURCES_LIST_PATH = "/etc/apt/sources.list";
constexpr const char *APT_SOURCES_DIR = "/etc/apt/sources.list.d";
constexpr const char *APT_LISTS_DIR = "/var/lib/apt/lists";
constexpr const char *APT_LOG_DIR = "/var/log/apt";
constexpr const char *OS_RELEASE_PATH = "/etc/os-release";
constexpr const char *OS_RELEASE_FALLBACK_PATH = "/usr/lib/os-release";

//! Compressions apt uses for the files it stores. gzip and zstd are decompressed by DuckDB's file system, lz4
//! (the default of apt's index files on Debian and Ubuntu containers) by the extension.
enum class AptCompression : uint8_t { NONE, GZIP, ZSTD, LZ4 };

//! Reads a text file line by line through DuckDB's file system, so that enable_external_access and
//! allowed_directories apply. A missing file reads as empty.
class AptLineReader {
public:
	AptLineReader(FileSystem &fs, const string &path, AptCompression compression = AptCompression::NONE);
	~AptLineReader();
	AptLineReader(const AptLineReader &) = delete;
	AptLineReader &operator=(const AptLineReader &) = delete;

	bool Exists() const {
		return handle != nullptr;
	}
	bool ReadLine(string &line);
	//! Last modification time of the file (requires Exists())
	timestamp_t LastModifiedTime();

private:
	//! Appends the next decompressed bytes to the buffer, sets end_of_file when there are none
	void Refill();
	void RefillLz4();

	FileSystem &fs;
	string path;
	unique_ptr<FileHandle> handle;
	string buffer;
	idx_t position = 0;
	bool end_of_file = false;
	//! LZ4F_dctx, when reading an lz4 file
	void *lz4_context = nullptr;
	string lz4_input;
	idx_t lz4_input_position = 0;
};

//! A deb822 stanza (dpkg status, extended_states, Release, deb822 sources), keyed by field name
using AptStanza = std::unordered_map<string, string>;

//! Parses a deb822 file. Continuation lines are appended to their field, except for Description where only
//! the synopsis is kept. Lines starting with "#" are comments.
vector<AptStanza> ReadStanzas(FileSystem &fs, const string &path, AptCompression compression = AptCompression::NONE);
string GetField(const AptStanza &stanza, const string &key);

//! Empty strings and lists become NULL
Value StringOrNull(const string &value);
Value StringListOrNull(const vector<string> &values);
Value TimestampOrNull(const timestamp_t &value);

vector<string> SplitWhitespace(const string &input);
//! All the numbers of a string, e.g. [2024, 2, 10, 10, 0, 0] for "2024-02-10  10:00:00"
vector<int32_t> ExtractNumbers(const string &input);
//! Sorted names of the regular files of a directory (empty when it does not exist)
vector<string> ListDirectory(FileSystem &fs, const string &directory);

//! The optional "root" parameter: the directory the apt/dpkg paths are resolved under
string GetRoot(TableFunctionBindInput &input);

struct AptLocalState : public LocalTableFunctionState {
	idx_t batch_index = 0;
};
unique_ptr<LocalTableFunctionState> AptInitLocal(ExecutionContext &context, TableFunctionInitInput &input,
                                                 GlobalTableFunctionState *global_state);

//! Registers a table function with the "root" parameter and its documentation
void RegisterAptTableFunction(ExtensionLoader &loader, TableFunction function, const string &description,
                              const string &example);

//===--------------------------------------------------------------------===//
// Installed packages (dpkg_status.cpp)
//===--------------------------------------------------------------------===//
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
	int64_t installed_size_kb = 0;
	bool auto_installed = false;
	bool held = false;
};

//! Fully installed packages: the ones removed with leftover config files are skipped
vector<AptPackage> ReadInstalledPackages(FileSystem &fs, const string &root);

//===--------------------------------------------------------------------===//
// Debian versions (apt_versions.cpp)
//===--------------------------------------------------------------------===//
//! Compares two Debian versions like dpkg --compare-versions: negative, zero or positive
int32_t CompareDebianVersions(const string &a, const string &b);

//===--------------------------------------------------------------------===//
// Repository indexes (apt_indexes.cpp)
//===--------------------------------------------------------------------===//
struct AptRepository {
	string site;
	string suite;
	string codename;
	string component;
	string origin;
	string label;
	vector<string> architectures;
	timestamp_t date = timestamp_t::infinity();
	timestamp_t valid_until = timestamp_t::infinity();
	bool not_automatic = false;
	bool but_automatic_upgrades = false;
	int64_t packages = 0;

	bool IsSecurity() const;
};

//! A version of an installed package published by a repository
struct AptIndexEntry {
	string name;
	string architecture;
	string version;
	idx_t repository;
};

struct AptIndexes {
	vector<AptRepository> repositories;
	vector<AptIndexEntry> entries;
};

//! Reads the indexes downloaded by "apt update". Only the entries of the given "name|architecture" keys are kept.
AptIndexes ReadIndexes(FileSystem &fs, const string &root, const std::unordered_set<string> &wanted);

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//
void RegisterPackageFunctions(ExtensionLoader &loader);
void RegisterVersionFunctions(ExtensionLoader &loader);
void RegisterSourceFunctions(ExtensionLoader &loader);
void RegisterIndexFunctions(ExtensionLoader &loader);
void RegisterHistoryFunctions(ExtensionLoader &loader);

} // namespace duckdb
