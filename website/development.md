# Development

## Build

```sh
git clone --recurse-submodules https://github.com/adriens/duckdb-apt
cd duckdb-apt
GEN=ninja make
```

The extension depends on [lz4](https://github.com/lz4/lz4), declared in `vcpkg.json`. CI gets it from vcpkg; locally, use vcpkg as described in the [extension template](https://github.com/duckdb/extension-template), or install lz4 (`apt install liblz4-dev`, `brew install lz4`) and set `LZ4_ROOT` to its prefix if CMake does not find it.

The sources are split by area:

| File | Content |
| --- | --- |
| `src/apt_common.cpp` | Reading files through DuckDB (gzip, zstd, lz4), deb822 parsing, shared helpers |
| `src/dpkg_status.cpp` | `apt_packages()`, `apt_dependencies()`, `apt_provides()`, `apt_version()` |
| `src/apt_sources.cpp` | `apt_sources()` |
| `src/apt_indexes.cpp` | `apt_repositories()`, `apt_package_origins()`, `apt_upgradable()` |
| `src/apt_history.cpp` | `apt_history()` |
| `src/apt_versions.cpp` | `apt_version_compare()` |

## Tests

```sh
make test
```

| Test | Checks |
| --- | --- |
| `test/sql/apt_fixtures.test` | The exact output of every function on fake systems in `test/data` (`root :=`): the same on any machine |
| `test/sql/apt_versions.test` | `apt_version_compare()` against results of `dpkg --compare-versions` |
| `test/sql/apt_sandbox.test` | DuckDB's file access restrictions apply |
| `test/sql/apt.test` | Invariants on the host |
| `test/sql/apt_host.test` | Strict checks on an apt-based host, enabled with `APT_HOST_TESTS=1` after `apt-get update` |

## Continuous integration

| Workflow | Does |
| --- | --- |
| Main Extension Distribution Pipeline | Format and clang-tidy checks, builds for Linux amd64 and arm64 in DuckDB's manylinux container |
| apt Distributions | Runs the built binaries on Debian 12, Debian 13, Ubuntu 22.04, Ubuntu 24.04 and Ubuntu 26.04 with the host checks, compares `apt_upgradable()` with `apt list --upgradable`, builds the database, and renders the sample security reports |
| Website | Builds this site with [Zensical](https://zensical.org) and publishes it to GitHub Pages |

The build container has no dpkg: that is why the apt Distributions workflow exists.

## Lint

```sh
make format-check   # needs Python <= 3.14 for cmake-format: add PYTHON=/path/to/python3.12 if needed
make tidy-check     # needs clang-tidy, e.g. uv tool install clang-tidy==18.1.8
```

## This website

```sh
pip install zensical
zensical serve      # http://localhost:8000
```
