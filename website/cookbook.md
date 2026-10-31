# Cookbook

Ready-to-use queries. More in [`docs/examples.sql`](https://github.com/adriens/duckdb-apt/blob/main/docs/examples.sql).

## Supply chain

```sql
-- Packages apt will never update: no configured repository knows them
SELECT name, version, maintainer
FROM apt_packages()
ANTI JOIN apt_package_origins() USING (name, architecture);

-- Repositories with risky settings
SELECT uri, suite,
       trusted AS signature_check_disabled,
       signed_by IS NULL AS any_trusted_key_accepted,
       uri LIKE 'http://%' AS plain_http
FROM apt_sources()
WHERE enabled
  AND (trusted OR signed_by IS NULL OR uri LIKE 'http://%');

-- Where installed packages come from
SELECT origin, suite, component, count(*) AS packages
FROM apt_package_origins()
WHERE is_installed_version
GROUP BY ALL
ORDER BY packages DESC;
```

## Patching

```sql
-- Pending security updates
SELECT name, current_version, new_version, repository
FROM apt_upgradable()
WHERE is_security;

-- Held packages and the update they miss
SELECT p.name, p.version, u.new_version, u.is_security
FROM apt_packages() p
LEFT JOIN apt_upgradable() u USING (name, architecture)
WHERE p.held;

-- Repository indexes apt now refuses (past Valid-Until)
SELECT site, suite, valid_until
FROM apt_repositories()
WHERE valid_until < now()::TIMESTAMP;
```

## Dependencies

```sql
-- What depends on libssl3t64?
SELECT package, relation, version_constraint
FROM apt_dependencies()
WHERE dependency = 'libssl3t64';

-- The most depended-on packages
SELECT dependency, count(DISTINCT package) AS dependents
FROM apt_dependencies()
WHERE relation IN ('Depends', 'Pre-Depends')
GROUP BY dependency
ORDER BY dependents DESC
LIMIT 10;
```

## History

```sql
-- Changes per user (NULL: automatic, e.g. unattended-upgrades)
SELECT coalesce(requested_by, '(automatic)') AS requested_by, action, count(*) AS changes
FROM apt_history()
GROUP BY ALL
ORDER BY changes DESC;

-- One row per month, one column per action
PIVOT (SELECT date_trunc('month', start_time) AS month, action FROM apt_history())
ON action
USING count(*)
ORDER BY month;
```

## Versions

```sql
-- Every newer version published for an installed package, compared like dpkg
SELECT name, installed_version, available_version, suite
FROM apt_package_origins()
WHERE apt_version_compare(available_version, installed_version) > 0;
```

## Exports

```sql
-- Package URLs, for vulnerability scanners (OSV, Grype, Dependency-Track)
COPY (SELECT purl FROM apt_packages()) TO 'purls.csv';

-- Snapshot two machines, then compare them
COPY (SELECT name, version, architecture FROM apt_packages()) TO 'laptop.parquet';

SELECT name, a.version AS laptop, b.version AS server
FROM 'laptop.parquet' a
FULL JOIN 'server.parquet' b USING (name)
WHERE a.version IS DISTINCT FROM b.version;
```
