#include "apt_common.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"

#include <algorithm>

namespace duckdb {

struct AptHistoryEntry {
	timestamp_t start_time;
	timestamp_t end_time;
	string action;
	string package;
	string architecture;
	string old_version;
	string new_version;
	bool automatic = false;
	string requested_by;
	string commandline;
	string error;
};

struct AptHistoryBindData : public TableFunctionData {
	vector<AptHistoryEntry> entries;
};

// History dates are local times, e.g. "2024-02-10  10:00:00"
static timestamp_t ParseHistoryDate(const string &value) {
	auto numbers = ExtractNumbers(value);
	if (numbers.size() != 6 || !Date::IsValid(numbers[0], numbers[1], numbers[2]) ||
	    !Time::IsValidTime(numbers[3], numbers[4], numbers[5], 0)) {
		return timestamp_t::infinity();
	}
	return Timestamp::FromDatetime(Date::FromDate(numbers[0], numbers[1], numbers[2]),
	                               Time::FromTime(numbers[3], numbers[4], numbers[5]));
}

// "curl:amd64 (7.88.1-10), libcurl4:amd64 (7.88.1-9, 7.88.1-10)": name, then the versions in parentheses
static void ParseHistoryPackages(const string &action, const string &field, const AptHistoryEntry &transaction,
                                 vector<AptHistoryEntry> &out) {
	idx_t pos = 0;
	while (pos < field.size()) {
		auto open = field.find(" (", pos);
		auto close = open == string::npos ? string::npos : field.find(')', open);
		if (close == string::npos) {
			break;
		}
		auto package = field.substr(pos, open - pos);
		StringUtil::Trim(package);
		auto versions = StringUtil::Split(field.substr(open + 2, close - open - 2), ", ");
		pos = close + 1;
		while (pos < field.size() && (field[pos] == ',' || field[pos] == ' ')) {
			pos++;
		}

		AptHistoryEntry entry = transaction;
		entry.action = StringUtil::Lower(action);
		auto colon = package.rfind(':');
		entry.package = colon == string::npos ? package : package.substr(0, colon);
		entry.architecture = colon == string::npos ? "" : package.substr(colon + 1);
		auto automatic = std::find(versions.begin(), versions.end(), "automatic");
		entry.automatic = automatic != versions.end();
		if (entry.automatic) {
			versions.erase(automatic);
		}
		if (versions.empty()) {
			continue;
		}
		if (action == "Upgrade" || action == "Downgrade") {
			entry.old_version = versions[0];
			entry.new_version = versions.size() > 1 ? versions[1] : "";
		} else if (action == "Remove" || action == "Purge") {
			entry.old_version = versions[0];
		} else {
			entry.new_version = versions[0];
		}
		out.push_back(std::move(entry));
	}
}

static void ReadHistoryFile(FileSystem &fs, const string &path, vector<AptHistoryEntry> &out) {
	auto compression =
	    StringUtil::EndsWith(path, ".gz") ? FileCompressionType::GZIP : FileCompressionType::UNCOMPRESSED;
	static const char *ACTIONS[] = {"Install", "Reinstall", "Upgrade", "Downgrade", "Remove", "Purge"};
	for (auto &stanza : ReadStanzas(fs, path, compression)) {
		AptHistoryEntry transaction;
		transaction.start_time = ParseHistoryDate(GetField(stanza, "Start-Date"));
		transaction.end_time = ParseHistoryDate(GetField(stanza, "End-Date"));
		transaction.commandline = GetField(stanza, "Commandline");
		transaction.error = GetField(stanza, "Error");
		// "adriens (1000)": keep the user name
		transaction.requested_by = GetField(stanza, "Requested-By");
		auto paren = transaction.requested_by.find(" (");
		if (paren != string::npos) {
			transaction.requested_by = transaction.requested_by.substr(0, paren);
		}
		for (auto action : ACTIONS) {
			ParseHistoryPackages(action, GetField(stanza, action), transaction, out);
		}
	}
}

// Rotation number of "history.log[.N][.gz]", or -1 for other files
static int64_t HistoryRotation(const string &name) {
	if (name == "history.log" || name == "history.log.gz") {
		return 0;
	}
	if (!StringUtil::StartsWith(name, "history.log.")) {
		return -1;
	}
	auto number = name.substr(string("history.log.").size());
	if (StringUtil::EndsWith(number, ".gz")) {
		number = number.substr(0, number.size() - 3);
	}
	if (number.empty() || !std::all_of(number.begin(), number.end(), StringUtil::CharacterIsDigit)) {
		return -1;
	}
	return std::stoll(number);
}

static unique_ptr<FunctionData> AptHistoryBind(ClientContext &context, TableFunctionBindInput &input,
                                               vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<AptHistoryBindData>();
	auto &fs = FileSystem::GetFileSystem(context);
	auto log_dir = GetRoot(input) + APT_LOG_DIR;

	// Oldest rotated logs first, so that rows come out in chronological order
	vector<std::pair<int64_t, string>> files;
	for (auto &name : ListDirectory(fs, log_dir)) {
		auto rotation = HistoryRotation(name);
		if (rotation >= 0) {
			files.emplace_back(rotation, name);
		}
	}
	std::sort(files.begin(), files.end(), [](const std::pair<int64_t, string> &a, const std::pair<int64_t, string> &b) {
		return a.first > b.first;
	});
	for (auto &file : files) {
		ReadHistoryFile(fs, log_dir + "/" + file.second, result->entries);
	}

	names.emplace_back("start_time");
	return_types.emplace_back(LogicalType::TIMESTAMP);
	names.emplace_back("end_time");
	return_types.emplace_back(LogicalType::TIMESTAMP);
	names.emplace_back("action");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("package");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("architecture");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("old_version");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("new_version");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("automatic");
	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("requested_by");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("commandline");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("error");
	return_types.emplace_back(LogicalType::VARCHAR);

	return std::move(result);
}

static void AptHistoryFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.bind_data->CastNoConst<AptHistoryBindData>();
	auto &local_state = data_p.local_state->Cast<AptLocalState>();
	idx_t count = 0;

	for (idx_t i = local_state.batch_index; i < data.entries.size() && count < STANDARD_VECTOR_SIZE; i++) {
		auto &entry = data.entries[i];
		output.data[0].Append(TimestampOrNull(entry.start_time));
		output.data[1].Append(TimestampOrNull(entry.end_time));
		output.data[2].Append(Value(entry.action));
		output.data[3].Append(Value(entry.package));
		output.data[4].Append(StringOrNull(entry.architecture));
		output.data[5].Append(StringOrNull(entry.old_version));
		output.data[6].Append(StringOrNull(entry.new_version));
		output.data[7].Append(Value::BOOLEAN(entry.automatic));
		output.data[8].Append(StringOrNull(entry.requested_by));
		output.data[9].Append(StringOrNull(entry.commandline));
		output.data[10].Append(StringOrNull(entry.error));
		count++;
	}

	local_state.batch_index += count;
	output.CheckCardinality(count);
}

void RegisterHistoryFunctions(ExtensionLoader &loader) {
	RegisterAptTableFunction(
	    loader, TableFunction("apt_history", {}, AptHistoryFunction, AptHistoryBind),
	    "Package installs, upgrades and removals recorded by apt (history.log and its rotated .gz files), with who "
	    "requested them: requested_by is NULL for automatic updates such as unattended-upgrades",
	    "SELECT start_time, action, package, requested_by FROM apt_history() ORDER BY start_time DESC");
}

} // namespace duckdb
