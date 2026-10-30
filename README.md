# apt

This repository is based on https://github.com/duckdb/extension-template, and is the Debian/Ubuntu sibling of [duckdb-brew](https://github.com/adriens/duckdb-brew).

The `apt` extension lets you query **locally installed** apt/dpkg packages with SQL. Installed packages are read straight from `/var/lib/dpkg/status` (no `dpkg` subprocess); upgradable packages come from `apt list --upgradable`.

On systems without dpkg (macOS, Fedora, ...) every function simply returns no rows.

## Functions

| Function | Kind | Description |
| --- | --- | --- |
| `apt_version()` | scalar | Version of the local `apt` |
| `apt_packages()` | table | Installed packages: `name`, `version`, `architecture`, `section`, `priority`, `installed_size_kb`, `maintainer`, `homepage`, `source`, `description`, `multi_arch`, `auto_installed` |
| `apt_dependencies()` | table | One row per dependency alternative: `package`, `architecture`, `relation` (`Pre-Depends`, `Depends`, `Recommends`, `Suggests`), `dependency`, `version_constraint`, `alternative` |
| `apt_provides()` | table | Virtual packages provided by installed packages: `package`, `architecture`, `provides`, `version` |
| `apt_sources()` | table | Configured repositories (`sources.list` and `sources.list.d/*.list`/`*.sources`), one row per type × URI × suite: `file`, `format`, `enabled`, `type`, `uri`, `suite`, `components`, `architectures`, `signed_by`, `trusted` |
| `apt_package_origins()` | table | Repositories that know each installed package, from the indexes in `/var/lib/apt/lists`: `name`, `architecture`, `installed_version`, `available_version`, `is_installed_version`, `origin`, `label`, `suite`, `codename`, `component`, `site` |
| `apt_upgradable()` | table | Packages with a newer version available: `name`, `repository`, `current_version`, `new_version`, `architecture` |

All table functions except `apt_upgradable()` accept an optional `root` parameter to read the files of another system, e.g. a mounted disk image or container filesystem:

```sql
SELECT * FROM apt_packages(root := '/mnt/backup-server');
```

An installed package with no row in `apt_package_origins()` is unknown to every configured repository (installed from a local `.deb`, or its repository was removed or disabled): apt will never update it. `apt_package_origins()` and `apt_upgradable()` reflect the last `apt update`; compressed indexes (`Acquire::GzipIndexes`) are not read.

`auto_installed` is `true` for packages pulled in as dependencies, `false` for the ones you asked for explicitly (see `apt-mark showmanual`).
## Examples

More in [docs/examples.sql](docs/examples.sql).

```sql
LOAD apt;

SELECT apt_version();

-- The 10 biggest installed packages
SELECT name, version, installed_size_kb / 1024 AS size_mb
FROM apt_packages() ORDER BY installed_size_kb DESC LIMIT 10;

-- What did I install on purpose?
SELECT name, version FROM apt_packages() WHERE NOT auto_installed ORDER BY name;

-- Disk usage per section
SELECT section, count(*) AS packages, sum(installed_size_kb) / 1024 AS size_mb
FROM apt_packages() GROUP BY section ORDER BY size_mb DESC;

-- What depends on libssl3t64?
SELECT package, relation FROM apt_dependencies() WHERE dependency = 'libssl3t64';

-- Pending upgrades
SELECT name, current_version, new_version FROM apt_upgradable();

-- Export your inventory
COPY (SELECT * FROM apt_packages()) TO 'packages.parquet';
```

## Security report

[docs/apt-security-report.qmd](docs/apt-security-report.qmd) is a Quarto notebook that turns these functions into a PDF security report: findings ranked by severity, repository trust, orphaned packages, pending security updates and dependency risk. It needs [Quarto](https://quarto.org), a LaTeX distribution with `xelatex`, [uv](https://docs.astral.sh/uv/) and [Task](https://taskfile.dev):

```sh
GEN=ninja make                    # build the extension
task report                       # render docs/apt-security-report.pdf
APT_ROOT=/mnt/image task report   # audit another system
```

The rendered PDF describes a real machine and is git-ignored: share it deliberately.

## Building
```sh
git clone --recurse-submodules https://github.com/adriens/duckdb-apt
cd duckdb-apt
GEN=ninja make
```

The main binaries that will be built are:

```sh
./build/release/duckdb
./build/release/test/unittest
./build/release/extension/apt/apt.duckdb_extension
```

- `duckdb` is the DuckDB shell with the extension already loaded.
- `unittest` is the test runner of DuckDB, with the extension linked in.
- `apt.duckdb_extension` is the loadable binary as it would be distributed.

## Running the tests
```sh
make test
```

- `test/sql/apt_fixtures.test` checks the exact output of the parsers on the fake Debian system in `test/data/debian` (via `root :=`), so it gives the same result on any machine.
- `test/sql/apt.test` runs against the host and only checks invariants, since the installed packages vary.

## Linting
CI runs the format and clang-tidy checks of `extension-ci-tools`. To run them locally:

```sh
make format-check           # needs Python <= 3.14 for cmake-format: add PYTHON=/path/to/python3.12 if needed
make tidy-check             # needs clang-tidy, e.g. `uv tool install clang-tidy==18.1.8`
```

The `tidy-check` target is overridden in the `Makefile`: the upstream one skips files directly under `src/`.

## Installing the deployed binaries
Unsigned extensions require DuckDB to be launched with `allow_unsigned_extensions`:

```shell
duckdb -unsigned
```

```sql
SET custom_extension_repository='bucket.s3.eu-west-1.amazonaws.com/apt/latest';
INSTALL apt;
LOAD apt;
```
