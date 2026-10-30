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
| `apt_upgradable()` | table | Packages with a newer version available: `name`, `repository`, `current_version`, `new_version`, `architecture` |

`auto_installed` is `true` for packages pulled in as dependencies, `false` for the ones you asked for explicitly (see `apt-mark showmanual`). `apt_upgradable()` reflects the last `apt update`.

## Examples

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
