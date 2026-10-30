# apt

[![Main Extension Distribution Pipeline](https://github.com/adriens/duckdb-apt/actions/workflows/MainDistributionPipeline.yml/badge.svg)](https://github.com/adriens/duckdb-apt/actions/workflows/MainDistributionPipeline.yml)
[![apt Distributions](https://github.com/adriens/duckdb-apt/actions/workflows/AptDistributions.yml/badge.svg)](https://github.com/adriens/duckdb-apt/actions/workflows/AptDistributions.yml)

This repository is based on https://github.com/duckdb/extension-template, and is the Debian/Ubuntu sibling of [duckdb-brew](https://github.com/adriens/duckdb-brew).

The `apt` extension lets you query the apt/dpkg package manager of Debian-based systems with SQL: installed packages, dependencies, repositories and how they are trusted, where each package comes from, pending updates and the change history. It reads the dpkg and apt files directly (no subprocess), through DuckDB's file system.

On systems without dpkg (macOS, Fedora, ...) every function simply returns no rows.

## Functions

| Function | Kind | Description |
| --- | --- | --- |
| `apt_packages()` | table | Installed packages: `name`, `version`, `architecture`, `section`, `priority`, `installed_size_kb`, `maintainer`, `homepage`, `source`, `description`, `multi_arch`, `auto_installed`, `held`, `installed_time`, `purl` |
| `apt_dependencies()` | table | One row per dependency alternative: `package`, `architecture`, `relation` (`Pre-Depends`, `Depends`, `Recommends`, `Suggests`), `dependency`, `version_constraint`, `alternative` |
| `apt_provides()` | table | Virtual packages provided by installed packages: `package`, `architecture`, `provides`, `version` |
| `apt_sources()` | table | Configured repositories (`sources.list` and `sources.list.d/*.list`/`*.sources`), one row per type × URI × suite: `file`, `format`, `enabled`, `type`, `uri`, `suite`, `components`, `architectures`, `signed_by`, `trusted` |
| `apt_repositories()` | table | Repository indexes downloaded by `apt update`: `site`, `suite`, `codename`, `component`, `origin`, `label`, `architectures`, `packages`, `date`, `valid_until`, `not_automatic`, `but_automatic_upgrades`, `is_security` |
| `apt_package_origins()` | table | Repositories publishing each installed package: `name`, `architecture`, `installed_version`, `available_version`, `is_installed_version`, `origin`, `label`, `suite`, `codename`, `component`, `site` |
| `apt_upgradable()` | table | Installed packages with a newer version available: `name`, `architecture`, `current_version`, `new_version`, `repository`, `is_security` |
| `apt_history()` | table | Installs, upgrades and removals recorded in `/var/log/apt/history.log*`: `start_time`, `end_time`, `action`, `package`, `architecture`, `old_version`, `new_version`, `automatic`, `requested_by`, `commandline`, `error` |
| `apt_version([root])` | scalar | Installed version of apt |
| `apt_version_compare(a, b)` | scalar | Compares two Debian versions like `dpkg --compare-versions`: -1, 0 or 1 |

`SELECT * FROM duckdb_functions() WHERE function_name LIKE 'apt%'` shows the description and an example of each function.

All table functions accept an optional `root` parameter to read the files of another system, e.g. a mounted disk image or container filesystem:

```sql
SELECT * FROM apt_packages(root := '/mnt/backup-server');
```

Good to know:

- `auto_installed` is `true` for packages pulled in as dependencies, `false` for the ones you asked for explicitly (see `apt-mark showmanual`). `held` packages (`apt-mark hold`) are skipped by upgrades. `installed_time` is the last time the package was installed or upgraded. `purl` is the [package URL](https://github.com/package-url/purl-spec) that vulnerability scanners (OSV, Grype, Dependency-Track) understand.
- An installed package with no row in `apt_package_origins()` is unknown to every configured repository (installed from a local `.deb`, or its repository was removed or disabled): apt will never update it.
- `apt_repositories()`, `apt_package_origins()` and `apt_upgradable()` reflect the last `apt update`. Indexes compressed with gzip or zstd are read; other compressions raise an error rather than reporting packages as orphaned.
- `apt_upgradable()` applies apt's default policy (NotAutomatic repositories such as backports only upgrade the packages installed from them), but not the pins of `/etc/apt/preferences`.
- `apt_history()` dates are in local time, `apt_repositories()` dates in UTC. `requested_by` is NULL for automatic changes such as unattended-upgrades.
- Files are read through DuckDB's file system: `SET enable_external_access = false` and `allowed_directories` restrict the extension like any other file access.

## Examples

More in [docs/examples.sql](docs/examples.sql).

```sql
INSTALL apt FROM community;
LOAD apt;

-- What did I install on purpose?
SELECT name, version
FROM apt_packages()
WHERE NOT auto_installed
ORDER BY name;

-- Which packages will apt never update?
SELECT name, version
FROM apt_packages()
ANTI JOIN apt_package_origins()
    USING (name, architecture);

-- Pending security updates
SELECT name, current_version, new_version
FROM apt_upgradable()
WHERE is_security;

-- Who changed what lately?
SELECT start_time, action, package, requested_by
FROM apt_history()
ORDER BY start_time DESC
LIMIT 20;

-- Package URLs for a vulnerability scanner
COPY (SELECT purl FROM apt_packages()) TO 'purls.csv';
```

## Security report

[docs/apt-security-report.qmd](docs/apt-security-report.qmd) is a Quarto notebook that turns these functions into a PDF security report: findings ranked by severity, repository trust and freshness, orphaned and held packages, pending security updates, change history and dependency risk. It needs [Quarto](https://quarto.org), a LaTeX distribution with `xelatex`, [uv](https://docs.astral.sh/uv/) and [Task](https://taskfile.dev):

```sh
task report                       # render docs/apt-security-report.pdf
APT_ROOT=/mnt/image task report   # audit another system
```

The rendered PDF describes a real machine and is git-ignored: share it deliberately. CI renders two reports at every build, attached to the workflow run and to releases: one for the fake Debian system of the tests, one for the GitHub runner.

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

The sources are split by area: `dpkg_status.cpp` (installed packages), `apt_sources.cpp`, `apt_indexes.cpp` (repository indexes, origins, upgrades), `apt_history.cpp`, `apt_versions.cpp` and the shared `apt_common.cpp`.

## Running the tests
```sh
make test
```

- `test/sql/apt_fixtures.test` checks the exact output of every function on the fake Debian system in `test/data/debian` (via `root :=`), so it gives the same result on any machine. `test/data/debian-lz4` checks that unsupported index compressions fail.
- `test/sql/apt_versions.test` checks `apt_version_compare()` against results of `dpkg --compare-versions`.
- `test/sql/apt_sandbox.test` checks that DuckDB's file access restrictions apply.
- `test/sql/apt.test` runs against the host and only checks invariants. `test/sql/apt_host.test` holds strict checks, enabled with `APT_HOST_TESTS=1` on an apt-based system after `apt-get update`.

The build pipeline runs in a manylinux (RPM-based) container, without dpkg. The [apt Distributions](.github/workflows/AptDistributions.yml) workflow therefore runs the built binaries on Debian 12, Debian 13, Ubuntu 22.04 and Ubuntu 24.04 containers, with the host checks enabled, and compares `apt_upgradable()` with `apt list --upgradable`.

## Linting
CI runs the format and clang-tidy checks of `extension-ci-tools`. To run them locally:

```sh
make format-check           # needs Python <= 3.14 for cmake-format: add PYTHON=/path/to/python3.12 if needed
make tidy-check             # needs clang-tidy, e.g. `uv tool install clang-tidy==18.1.8`
```

The `tidy-check` target is overridden in the `Makefile`: the upstream one skips files directly under `src/`.
