#define DUCKDB_EXTENSION_MAIN

#include "apt_extension.hpp"
#include "apt_common.hpp"

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
	loader.SetDescription("Query the apt/dpkg package manager of Debian-based systems with SQL: installed packages, "
	                      "dependencies, repositories, package origins, pending updates and history");
	RegisterPackageFunctions(loader);
	RegisterVersionFunctions(loader);
	RegisterSourceFunctions(loader);
	RegisterIndexFunctions(loader);
	RegisterHistoryFunctions(loader);
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
