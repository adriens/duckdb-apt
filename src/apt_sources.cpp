#include "apt_common.hpp"

#include "duckdb/common/string_util.hpp"

namespace duckdb {

struct AptSource {
	string file;
	string format;
	bool enabled = true;
	string type;
	string uri;
	string suite;
	vector<string> components;
	vector<string> architectures;
	string signed_by;
	bool trusted = false;
};

struct AptSourcesBindData : public TableFunctionData {
	vector<AptSource> sources;
};

// One-line format: "deb [arch=amd64 signed-by=/usr/share/keyrings/x.gpg] http://host/path suite main contrib"
static void ReadOneLineSources(FileSystem &fs, const string &path, vector<AptSource> &out) {
	AptLineReader reader(fs, path);
	string line;
	while (reader.ReadLine(line)) {
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
static void ReadDeb822Sources(FileSystem &fs, const string &path, vector<AptSource> &out) {
	for (auto &stanza : ReadStanzas(fs, path)) {
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
	auto root = GetRoot(input);

	// apt ignores files of sources.list.d with other extensions (e.g. ".disabled")
	ReadOneLineSources(fs, root + APT_SOURCES_LIST_PATH, result->sources);
	for (auto &name : ListDirectory(fs, root + APT_SOURCES_DIR)) {
		auto path = root + APT_SOURCES_DIR + "/" + name;
		if (StringUtil::EndsWith(name, ".list")) {
			ReadOneLineSources(fs, path, result->sources);
		} else if (StringUtil::EndsWith(name, ".sources")) {
			ReadDeb822Sources(fs, path, result->sources);
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

void RegisterSourceFunctions(ExtensionLoader &loader) {
	RegisterAptTableFunction(loader, TableFunction("apt_sources", {}, AptSourcesFunction, AptSourcesBind),
	                         "Configured repositories (sources.list and sources.list.d, one-line and deb822 "
	                         "formats), one row per type x URI x suite, with how they are trusted",
	                         "SELECT uri, suite FROM apt_sources() WHERE enabled AND signed_by IS NULL");
}

} // namespace duckdb
