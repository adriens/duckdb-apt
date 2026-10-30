-- Example queries for the apt DuckDB extension
-- Run with: ./build/release/duckdb -unsigned < docs/examples.sql

LOAD 'build/release/extension/apt/apt.duckdb_extension';

SELECT apt_version();

-------------------------------------------------------------------------------
-- Disk usage
-------------------------------------------------------------------------------

-- Space used per section
SELECT section, count(*) AS packages, round(sum(installed_size_kb) / 1024.0) AS size_mb
FROM apt_packages()
GROUP BY section
ORDER BY size_mb DESC;

-- Biggest packages installed on purpose
SELECT name, version, round(installed_size_kb / 1024.0) AS size_mb
FROM apt_packages()
WHERE NOT auto_installed
ORDER BY size_mb DESC
LIMIT 10;

-- Source packages that expand into the most binary packages
SELECT source, count(*) AS binaries, round(sum(installed_size_kb) / 1024.0) AS size_mb
FROM apt_packages()
GROUP BY source
ORDER BY binaries DESC
LIMIT 10;

-------------------------------------------------------------------------------
-- What is installed
-------------------------------------------------------------------------------

-- Manually installed vs. pulled in as a dependency
SELECT auto_installed, count(*) AS packages, round(sum(installed_size_kb) / 1024.0) AS size_mb
FROM apt_packages()
GROUP BY auto_installed;

-- Third-party software (not maintained by Ubuntu/Debian)
SELECT name, version, maintainer
FROM apt_packages()
WHERE NOT auto_installed
  AND maintainer NOT ILIKE '%ubuntu%'
  AND maintainer NOT ILIKE '%debian%'
ORDER BY name;

-- Top maintainers
SELECT maintainer, count(*) AS packages
FROM apt_packages()
GROUP BY maintainer
ORDER BY packages DESC
LIMIT 10;

-- Architectures (multiarch) and priorities
SELECT architecture, count(*) AS packages FROM apt_packages() GROUP BY architecture ORDER BY packages DESC;
SELECT priority, count(*) AS packages FROM apt_packages() GROUP BY priority ORDER BY packages DESC;

-- Search package descriptions
SELECT name, version, description
FROM apt_packages()
WHERE description ILIKE '%sqlite%';

-------------------------------------------------------------------------------
-- Dependency graph
-------------------------------------------------------------------------------

-- Most depended-on packages
SELECT dependency, count(DISTINCT package) AS dependents
FROM apt_dependencies()
WHERE relation IN ('Depends', 'Pre-Depends')
GROUP BY dependency
ORDER BY dependents DESC
LIMIT 10;

-- Reverse dependencies: what needs libssl3t64?
SELECT package, relation, version_constraint
FROM apt_dependencies()
WHERE dependency = 'libssl3t64'
ORDER BY package;

-- Virtual packages and who provides them
SELECT provides, list(package ORDER BY package) AS providers
FROM apt_provides()
GROUP BY provides
HAVING count(*) > 1
ORDER BY provides;

-- Hard dependencies satisfied neither by a package nor by a Provides (should be empty)
SELECT package, dependency, version_constraint
FROM apt_dependencies()
WHERE relation IN ('Depends', 'Pre-Depends')
  AND NOT alternative
  AND dependency NOT IN (SELECT name FROM apt_packages())
  AND dependency NOT IN (SELECT provides FROM apt_provides());

-- Auto-installed packages nothing depends on any more (approximation of `apt autoremove`)
WITH needed AS (
    SELECT dependency AS name
    FROM apt_dependencies()
    WHERE relation IN ('Pre-Depends', 'Depends', 'Recommends')
)
SELECT p.name, p.version, round(p.installed_size_kb / 1024.0) AS size_mb
FROM apt_packages() p
WHERE p.auto_installed
  AND p.name NOT IN (SELECT name FROM needed)
  AND NOT EXISTS (
      SELECT 1 FROM apt_provides() v WHERE v.package = p.name AND v.provides IN (SELECT name FROM needed)
  )
ORDER BY size_mb DESC;

-------------------------------------------------------------------------------
-- Software supply chain
-------------------------------------------------------------------------------

-- Configured repositories
SELECT regexp_extract(file, '[^/]+$') AS file, enabled, type, uri, suite, components, signed_by
FROM apt_sources()
ORDER BY file, suite;

-- Risky repository settings: plain HTTP, no key pinning, signature checks disabled
SELECT regexp_extract(file, '[^/]+$') AS file, uri, suite,
       uri LIKE 'http://%' AS plain_http,
       signed_by IS NULL AS any_trusted_key_accepted,
       trusted AS signature_check_disabled
FROM apt_sources()
WHERE enabled
  AND (uri LIKE 'http://%' OR signed_by IS NULL OR trusted);

-- Where installed packages come from
SELECT origin, suite, component, site, count(*) AS packages
FROM apt_package_origins()
WHERE is_installed_version
GROUP BY ALL
ORDER BY packages DESC;

-- Orphaned packages: no configured repository knows them, apt will never update them
SELECT p.name, p.version, p.maintainer
FROM apt_packages() p
ANTI JOIN apt_package_origins() o USING (name, architecture)
ORDER BY p.name;

-- Installed version available in no repository (local build, or leftover from an older release)
SELECT p.name, p.version AS installed_version, list(DISTINCT o.available_version) AS available_versions
FROM apt_packages() p
JOIN apt_package_origins() o USING (name, architecture)
GROUP BY ALL
HAVING NOT bool_or(o.is_installed_version)
ORDER BY p.name;

-- Installed from Ubuntu's universe component (community maintained, security fixes only with Ubuntu Pro)
SELECT DISTINCT o.name, o.installed_version
FROM apt_package_origins() o
WHERE o.is_installed_version AND o.origin = 'Ubuntu' AND o.component = 'universe'
ORDER BY o.name;

-------------------------------------------------------------------------------
-- Repositories and upgrades (as of the last `apt update`)
-------------------------------------------------------------------------------

-- Downloaded repository indexes, oldest first, with their validity
SELECT site, suite, component, packages, date, valid_until, is_security
FROM apt_repositories()
ORDER BY date;

-- Indexes past their Valid-Until date: apt refuses them, the system runs on old metadata
SELECT site, suite, valid_until
FROM apt_repositories()
WHERE valid_until < now()::TIMESTAMP;

-- Pending upgrades and the repositories they come from
SELECT name, current_version, new_version, repository, is_security
FROM apt_upgradable()
ORDER BY name;

-- Pending security upgrades
SELECT name, current_version, new_version, repository
FROM apt_upgradable()
WHERE is_security
ORDER BY name;

-- Held packages (apt-mark hold) and the update they are missing
SELECT p.name, p.version, u.new_version, u.is_security
FROM apt_packages() p
LEFT JOIN apt_upgradable() u USING (name, architecture)
WHERE p.held;

-- Share of installed packages with an upgrade available
SELECT count(u.name) AS upgradable, count(*) AS installed,
       round(100.0 * count(u.name) / count(*), 1) AS pct_upgradable
FROM apt_packages() p
LEFT JOIN apt_upgradable() u USING (name, architecture);

-- Debian versions compared like dpkg --compare-versions (plain string comparison gets "1.10" < "1.9" wrong):
-- every newer version published for an installed package, in any repository, including NotAutomatic ones
SELECT name, installed_version, available_version, suite
FROM apt_package_origins()
WHERE apt_version_compare(available_version, installed_version) > 0
ORDER BY name;

-------------------------------------------------------------------------------
-- History
-------------------------------------------------------------------------------

-- Latest changes, and who requested them (NULL: automatic, e.g. unattended-upgrades)
SELECT start_time, action, package, old_version, new_version, requested_by
FROM apt_history()
ORDER BY start_time DESC
LIMIT 20;

-- Changes per user and action
SELECT coalesce(requested_by, '(automatic)') AS requested_by, action, count(*) AS changes
FROM apt_history()
GROUP BY ALL
ORDER BY changes DESC;

-- Failed transactions
SELECT DISTINCT start_time, commandline, error
FROM apt_history()
WHERE error IS NOT NULL;

-- When was each installed package last installed or upgraded?
SELECT name, version, installed_time
FROM apt_packages()
ORDER BY installed_time DESC
LIMIT 20;

-------------------------------------------------------------------------------
-- Inventory snapshots
-------------------------------------------------------------------------------

-- Export the inventory, e.g. to compare two machines
COPY (SELECT name, version, architecture, auto_installed FROM apt_packages()) TO 'apt-inventory.parquet';

-- Package URLs, for vulnerability scanners (OSV, Grype, Dependency-Track)
COPY (SELECT purl FROM apt_packages()) TO 'apt-purls.csv';

-- Audit another system, e.g. a mounted disk image or a container filesystem
SELECT count(*) FROM apt_packages(root := '/');

-- Compare with another machine's export:
-- SELECT name, a.version AS this_machine, b.version AS other_machine
-- FROM 'apt-inventory.parquet' a FULL JOIN 'other-inventory.parquet' b USING (name)
-- WHERE a.version IS DISTINCT FROM b.version;
