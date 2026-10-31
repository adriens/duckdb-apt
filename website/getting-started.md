# Getting started

## Install and load

The extension is distributed through the [DuckDB community extensions repository](https://duckdb.org/community_extensions/extensions/apt). Install it once, then load it in each session, from any DuckDB client:

=== "CLI"

    ```sql
    INSTALL apt FROM community;
    LOAD apt;
    ```

=== "Python"

    ```python
    import duckdb

    con = duckdb.connect()
    con.install_extension("apt", repository="community")
    con.load_extension("apt")
    print(con.sql("SELECT name, version FROM apt_packages() LIMIT 5"))
    ```

=== "R"

    ```r
    library(duckdb)
    con <- dbConnect(duckdb())
    dbExecute(con, "INSTALL apt FROM community; LOAD apt;")
    dbGetQuery(con, "SELECT name, version FROM apt_packages() LIMIT 5")
    ```

Binaries are built for Linux (amd64 and arm64, glibc). On a system without dpkg, every function returns no rows.

## First queries

```sql
-- What did I install on purpose?
SELECT name, version
FROM apt_packages()
WHERE NOT auto_installed
ORDER BY name;

-- Where does the disk space go?
SELECT section, count(*) AS packages, round(sum(installed_size_kb) / 1024) AS size_mb
FROM apt_packages()
GROUP BY section
ORDER BY size_mb DESC;

-- Pending security updates
SELECT name, current_version, new_version
FROM apt_upgradable()
WHERE is_security;

-- Who changed what lately?
SELECT start_time, action, package, requested_by
FROM apt_history()
ORDER BY start_time DESC
LIMIT 20;
```

Every function documents itself:

```sql
SELECT function_name, description, examples
FROM duckdb_functions()
WHERE function_name LIKE 'apt%';
```

## Audit another system

Every table function accepts a `root` parameter: the directory the apt and dpkg paths are resolved under. Point it at a mounted disk image, a backup or a container filesystem:

```sql
SELECT count(*) FROM apt_packages(root := '/mnt/backup-server');

-- the version of apt there
SELECT apt_version('/mnt/backup-server');
```

## Restrict file access

The extension reads every file through DuckDB's file system. In a locked-down DuckDB, it only reads what you allow:

```sql
SET allowed_directories = ['/mnt/image/'];
SET enable_external_access = false;

SELECT count(*) FROM apt_packages(root := '/mnt/image');  -- allowed
SELECT count(*) FROM apt_packages();                       -- Permission Error
```

## Freshness

`apt_repositories()`, `apt_package_origins()` and `apt_upgradable()` read the indexes downloaded by `apt update`: run it first for current answers. Indexes compressed with gzip, zstd or lz4 are read; other compressions raise an error rather than reporting packages as orphaned.

## Next steps

- [Function reference](functions.md): every column of every function.
- [Database](database.md): snapshot everything into a documented DuckDB database with keys and views.
- [Cookbook](cookbook.md): ready-to-use queries.
