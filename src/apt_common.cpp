#include "apt_common.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

#include <algorithm>

namespace duckdb {

static constexpr idx_t READ_CHUNK_SIZE = 1 << 20;

AptLineReader::AptLineReader(FileSystem &fs_p, const string &path, const FileCompressionType &compression) : fs(fs_p) {
	handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ | FileFlags::FILE_FLAGS_NULL_IF_NOT_EXISTS |
	                               FileOpenFlags(compression));
}

bool AptLineReader::ReadLine(string &line) {
	if (!handle) {
		return false;
	}
	while (true) {
		auto newline = buffer.find('\n', position);
		if (newline != string::npos || (end_of_file && position < buffer.size())) {
			auto end = newline == string::npos ? buffer.size() : newline;
			line = buffer.substr(position, end - position);
			position = end + 1;
			if (!line.empty() && line.back() == '\r') {
				line.pop_back();
			}
			return true;
		}
		if (end_of_file) {
			return false;
		}
		buffer.erase(0, position);
		position = 0;
		auto filled = buffer.size();
		buffer.resize(filled + READ_CHUNK_SIZE);
		auto read = handle->Read(&buffer[filled], READ_CHUNK_SIZE);
		buffer.resize(filled + NumericCast<idx_t>(read));
		end_of_file = read == 0;
	}
}

timestamp_t AptLineReader::LastModifiedTime() {
	D_ASSERT(handle);
	return fs.GetLastModifiedTime(*handle);
}

vector<AptStanza> ReadStanzas(FileSystem &fs, const string &path, const FileCompressionType &compression) {
	vector<AptStanza> stanzas;
	AptLineReader reader(fs, path, compression);
	AptStanza current;
	string last_key;
	string line;
	while (reader.ReadLine(line)) {
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

string GetField(const AptStanza &stanza, const string &key) {
	auto entry = stanza.find(key);
	return entry == stanza.end() ? string() : entry->second;
}

Value StringOrNull(const string &value) {
	return value.empty() ? Value(LogicalType::VARCHAR) : Value(value);
}

Value StringListOrNull(const vector<string> &values) {
	if (values.empty()) {
		return Value(LogicalType::LIST(LogicalType::VARCHAR));
	}
	vector<Value> list;
	for (auto &value : values) {
		list.emplace_back(value);
	}
	return Value::LIST(LogicalType::VARCHAR, std::move(list));
}

Value TimestampOrNull(const timestamp_t &value) {
	return value.IsFinite() ? Value::TIMESTAMP(value) : Value(LogicalType::TIMESTAMP);
}

vector<string> SplitWhitespace(const string &input) {
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

vector<int32_t> ExtractNumbers(const string &input) {
	vector<int32_t> numbers;
	int64_t current = -1;
	for (auto c : input) {
		if (StringUtil::CharacterIsDigit(c)) {
			current = (current < 0 ? 0 : current * 10) + (c - '0');
			if (current > NumericLimits<int32_t>::Maximum()) {
				return {};
			}
		} else if (current >= 0) {
			numbers.push_back(NumericCast<int32_t>(current));
			current = -1;
		}
	}
	if (current >= 0) {
		numbers.push_back(NumericCast<int32_t>(current));
	}
	return numbers;
}

vector<string> ListDirectory(FileSystem &fs, const string &directory) {
	vector<string> files;
	if (!fs.DirectoryExists(directory)) {
		return files;
	}
	fs.ListFiles(directory, [&](const string &name, bool is_dir) {
		if (!is_dir) {
			files.push_back(name);
		}
	});
	std::sort(files.begin(), files.end());
	return files;
}

string GetRoot(TableFunctionBindInput &input) {
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

unique_ptr<LocalTableFunctionState> AptInitLocal(ExecutionContext &context, TableFunctionInitInput &input,
                                                 GlobalTableFunctionState *global_state) {
	return make_uniq<AptLocalState>();
}

void RegisterAptTableFunction(ExtensionLoader &loader, TableFunction function, const string &description,
                              const string &example) {
	function.init_local = AptInitLocal;
	function.GetSignature().WithTypedKwargs("options",
	                                        [](TypedKwargs &options) { options.Add("root", LogicalType::VARCHAR); });
	CreateTableFunctionInfo info(std::move(function));
	FunctionDescription documentation;
	documentation.description = description;
	documentation.examples.push_back(example);
	info.descriptions.push_back(std::move(documentation));
	loader.RegisterFunction(std::move(info));
}

} // namespace duckdb
