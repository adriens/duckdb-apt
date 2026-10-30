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
-- Upgrades (as of the last `apt update`)
-------------------------------------------------------------------------------

-- Pending upgrades and the repository they come from
SELECT name, current_version, new_version, repository
FROM apt_upgradable()
ORDER BY name;

-- Pending security upgrades
SELECT name, current_version, new_version, repository
FROM apt_upgradable()
WHERE repository LIKE '%-security%'
ORDER BY name;

-- Share of installed packages with an upgrade available
SELECT count(u.name) AS upgradable, count(*) AS installed,
       round(100.0 * count(u.name) / count(*), 1) AS pct_upgradable
FROM apt_packages() p
LEFT JOIN apt_upgradable() u USING (name, architecture);

-------------------------------------------------------------------------------
-- Inventory snapshots
-------------------------------------------------------------------------------

-- Export the inventory, e.g. to compare two machines
COPY (SELECT name, version, architecture, auto_installed FROM apt_packages()) TO 'apt-inventory.parquet';

-- Compare with another machine's export:
-- SELECT name, a.version AS this_machine, b.version AS other_machine
-- FROM 'apt-inventory.parquet' a FULL JOIN 'other-inventory.parquet' b USING (name)
-- WHERE a.version IS DISTINCT FROM b.version;
