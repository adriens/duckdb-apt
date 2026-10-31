-- Builds a DuckDB database from the apt extension: one table per function, with primary and foreign keys, comments,
-- and views answering the usual security questions. Re-running it replaces the previous snapshot.
--
-- Usage (after INSTALL apt FROM community; once):
--   duckdb apt.duckdb -c ".read docs/apt_database.sql"
-- To snapshot another system (mounted disk image, container filesystem):
--   duckdb apt.duckdb -cmd "SET VARIABLE apt_root = '/mnt/image'" -c ".read docs/apt_database.sql"

LOAD apt;

BEGIN TRANSACTION;

-- Views first, then the tables referencing others
DROP VIEW IF EXISTS security_posture;
DROP VIEW IF EXISTS package_overview;
DROP VIEW IF EXISTS orphaned_packages;
DROP VIEW IF EXISTS packages_not_in_repositories;
DROP VIEW IF EXISTS third_party_packages;
DROP VIEW IF EXISTS maintainers;
DROP VIEW IF EXISTS risky_sources;
DROP VIEW IF EXISTS repository_usage;
DROP VIEW IF EXISTS expired_repositories;
DROP VIEW IF EXISTS security_updates;
DROP VIEW IF EXISTS held_packages;
DROP VIEW IF EXISTS dependency_edges;
DROP VIEW IF EXISTS package_dependents;
DROP VIEW IF EXISTS unsatisfied_dependencies;
DROP VIEW IF EXISTS autoremove_candidates;
DROP VIEW IF EXISTS disk_usage_by_section;
DROP VIEW IF EXISTS recent_changes;
DROP VIEW IF EXISTS monthly_activity;
DROP VIEW IF EXISTS user_activity;
DROP VIEW IF EXISTS failed_transactions;
DROP TABLE IF EXISTS upgrades;
DROP TABLE IF EXISTS package_origins;
DROP TABLE IF EXISTS provides;
DROP TABLE IF EXISTS dependencies;
DROP TABLE IF EXISTS repositories;
DROP TABLE IF EXISTS sources;
DROP TABLE IF EXISTS history;
DROP TABLE IF EXISTS packages;
DROP TABLE IF EXISTS snapshot;

-------------------------------------------------------------------------------
-- Tables
-------------------------------------------------------------------------------

CREATE TABLE snapshot (
    created_at TIMESTAMP NOT NULL,
    root VARCHAR,
    apt_version VARCHAR,
    extension_version VARCHAR
);
INSERT INTO snapshot
SELECT
    make_timestamp(epoch_us(now())),
    getvariable('apt_root'),
    apt_version(coalesce(getvariable('apt_root'), '/')),
    (SELECT extension_version FROM duckdb_extensions() WHERE extension_name = 'apt');

CREATE TABLE packages (
    name VARCHAR NOT NULL,
    version VARCHAR NOT NULL,
    architecture VARCHAR NOT NULL,
    section VARCHAR,
    priority VARCHAR,
    installed_size_kb BIGINT,
    maintainer VARCHAR,
    homepage VARCHAR,
    source VARCHAR,
    description VARCHAR,
    multi_arch VARCHAR,
    auto_installed BOOLEAN NOT NULL,
    held BOOLEAN NOT NULL,
    installed_time TIMESTAMP,
    purl VARCHAR,
    PRIMARY KEY (name, architecture)
);
INSERT INTO packages
SELECT * FROM apt_packages(root := getvariable('apt_root'));

CREATE TABLE dependencies (
    dependency_id INTEGER PRIMARY KEY,
    package VARCHAR NOT NULL,
    architecture VARCHAR NOT NULL,
    relation VARCHAR NOT NULL,
    dependency VARCHAR NOT NULL,
    version_constraint VARCHAR,
    alternative BOOLEAN NOT NULL,
    FOREIGN KEY (package, architecture) REFERENCES packages (name, architecture)
);
INSERT INTO dependencies
SELECT row_number() OVER (), * FROM apt_dependencies(root := getvariable('apt_root'));

CREATE TABLE provides (
    provide_id INTEGER PRIMARY KEY,
    package VARCHAR NOT NULL,
    architecture VARCHAR NOT NULL,
    provides VARCHAR NOT NULL,
    version VARCHAR,
    FOREIGN KEY (package, architecture) REFERENCES packages (name, architecture)
);
INSERT INTO provides
SELECT row_number() OVER (), * FROM apt_provides(root := getvariable('apt_root'));

CREATE TABLE sources (
    source_id INTEGER PRIMARY KEY,
    file VARCHAR NOT NULL,
    format VARCHAR NOT NULL,
    enabled BOOLEAN NOT NULL,
    type VARCHAR NOT NULL,
    uri VARCHAR NOT NULL,
    suite VARCHAR NOT NULL,
    components VARCHAR[],
    architectures VARCHAR[],
    signed_by VARCHAR,
    trusted BOOLEAN NOT NULL
);
INSERT INTO sources
SELECT row_number() OVER (), * FROM apt_sources(root := getvariable('apt_root'));

CREATE TABLE repositories (
    repository_id INTEGER PRIMARY KEY,
    site VARCHAR NOT NULL,
    suite VARCHAR,
    codename VARCHAR,
    component VARCHAR,
    origin VARCHAR,
    label VARCHAR,
    architectures VARCHAR[],
    packages BIGINT NOT NULL,
    date TIMESTAMP,
    valid_until TIMESTAMP,
    not_automatic BOOLEAN NOT NULL,
    but_automatic_upgrades BOOLEAN NOT NULL,
    is_security BOOLEAN NOT NULL
);
INSERT INTO repositories
SELECT row_number() OVER (ORDER BY site, suite, component), * FROM apt_repositories(root := getvariable('apt_root'));

CREATE TABLE package_origins (
    name VARCHAR NOT NULL,
    architecture VARCHAR NOT NULL,
    repository_id INTEGER NOT NULL,
    installed_version VARCHAR NOT NULL,
    available_version VARCHAR NOT NULL,
    is_installed_version BOOLEAN NOT NULL,
    PRIMARY KEY (name, architecture, repository_id, available_version),
    FOREIGN KEY (name, architecture) REFERENCES packages (name, architecture),
    FOREIGN KEY (repository_id) REFERENCES repositories (repository_id)
);
INSERT INTO package_origins
SELECT o.name, o.architecture, r.repository_id, o.installed_version, o.available_version, o.is_installed_version
FROM apt_package_origins(root := getvariable('apt_root')) o
JOIN repositories r
  ON r.site = o.site
 AND r.suite IS NOT DISTINCT FROM o.suite
 AND r.component IS NOT DISTINCT FROM o.component;

CREATE TABLE upgrades (
    name VARCHAR NOT NULL,
    architecture VARCHAR NOT NULL,
    current_version VARCHAR NOT NULL,
    new_version VARCHAR NOT NULL,
    repository VARCHAR,
    is_security BOOLEAN NOT NULL,
    PRIMARY KEY (name, architecture),
    FOREIGN KEY (name, architecture) REFERENCES packages (name, architecture)
);
INSERT INTO upgrades
SELECT * FROM apt_upgradable(root := getvariable('apt_root'));

-- No foreign key: history also mentions packages that are no longer installed
CREATE TABLE history (
    history_id INTEGER PRIMARY KEY,
    start_time TIMESTAMP,
    end_time TIMESTAMP,
    action VARCHAR NOT NULL,
    package VARCHAR NOT NULL,
    architecture VARCHAR,
    old_version VARCHAR,
    new_version VARCHAR,
    automatic BOOLEAN NOT NULL,
    requested_by VARCHAR,
    commandline VARCHAR,
    error VARCHAR
);
INSERT INTO history
SELECT row_number() OVER (), * FROM apt_history(root := getvariable('apt_root'));

-------------------------------------------------------------------------------
-- Views
-------------------------------------------------------------------------------

-- Supply chain

CREATE VIEW orphaned_packages AS
SELECT p.*
FROM packages p
ANTI JOIN package_origins o USING (name, architecture);

CREATE VIEW packages_not_in_repositories AS
SELECT
    p.name,
    p.architecture,
    p.version AS installed_version,
    list(DISTINCT o.available_version ORDER BY o.available_version) AS available_versions
FROM packages p
JOIN package_origins o USING (name, architecture)
GROUP BY ALL
HAVING NOT bool_or(o.is_installed_version);

CREATE VIEW third_party_packages AS
SELECT p.*
FROM packages p
WHERE NOT p.auto_installed
  AND NOT EXISTS (
      SELECT 1
      FROM package_origins o
      JOIN repositories r USING (repository_id)
      WHERE o.name = p.name AND o.architecture = p.architecture
        AND (r.origin ILIKE '%ubuntu%' OR r.origin ILIKE '%debian%')
  );

CREATE VIEW maintainers AS
SELECT
    regexp_replace(p.maintainer, '\s*<.*>', '') AS maintainer,
    count(*) AS packages,
    count(*) FILTER (WHERE NOT p.auto_installed) AS installed_on_purpose,
    count(t.name) AS third_party,
    count(o.name) AS orphaned,
    sum(p.installed_size_kb) AS installed_size_kb
FROM packages p
LEFT JOIN third_party_packages t USING (name, architecture)
LEFT JOIN orphaned_packages o USING (name, architecture)
GROUP BY ALL;

CREATE VIEW risky_sources AS
SELECT
    file,
    uri,
    suite,
    trusted AS signature_check_disabled,
    signed_by IS NULL AS any_trusted_key_accepted,
    uri LIKE 'http://%' AS plain_http
FROM sources
WHERE enabled
  AND (trusted OR signed_by IS NULL OR uri LIKE 'http://%');

CREATE VIEW repository_usage AS
SELECT
    r.repository_id,
    r.site,
    r.suite,
    r.component,
    r.origin,
    r.is_security,
    r.date,
    count(DISTINCT (o.name, o.architecture)) FILTER (WHERE o.is_installed_version) AS installed_packages_published,
    count(DISTINCT (o.name, o.architecture)) AS installed_packages_known
FROM repositories r
LEFT JOIN package_origins o USING (repository_id)
GROUP BY ALL;

CREATE VIEW expired_repositories AS
SELECT r.*
FROM repositories r, snapshot s
WHERE r.valid_until < s.created_at;

-- Patching

CREATE VIEW security_updates AS
SELECT u.*, p.held
FROM upgrades u
JOIN packages p USING (name, architecture)
WHERE u.is_security;

CREATE VIEW held_packages AS
SELECT
    p.name,
    p.architecture,
    p.version,
    u.new_version AS missed_version,
    u.is_security AS missed_security_fix
FROM packages p
LEFT JOIN upgrades u USING (name, architecture)
WHERE p.held;

-- Dependencies and cleanup

CREATE VIEW dependency_edges AS
SELECT DISTINCT
    d.package,
    d.architecture,
    d.relation,
    coalesce(p.name, v.package) AS depends_on,
    d.dependency AS declared_as
FROM dependencies d
LEFT JOIN (SELECT DISTINCT name FROM packages) p ON p.name = d.dependency
LEFT JOIN provides v ON p.name IS NULL AND v.provides = d.dependency
WHERE coalesce(p.name, v.package) IS NOT NULL;

CREATE VIEW package_dependents AS
SELECT p.name, p.architecture, count(DISTINCT e.package) AS direct_dependents
FROM packages p
LEFT JOIN dependency_edges e
  ON e.depends_on = p.name AND e.relation IN ('Pre-Depends', 'Depends')
GROUP BY ALL;

CREATE VIEW unsatisfied_dependencies AS
SELECT d.*
FROM dependencies d
WHERE d.relation IN ('Pre-Depends', 'Depends')
  AND NOT d.alternative
  AND d.dependency NOT IN (SELECT name FROM packages)
  AND d.dependency NOT IN (SELECT provides FROM provides);

CREATE VIEW autoremove_candidates AS
SELECT p.name, p.architecture, p.version, p.installed_size_kb
FROM packages p
WHERE p.auto_installed
  AND p.name NOT IN (
      SELECT depends_on FROM dependency_edges WHERE relation IN ('Pre-Depends', 'Depends', 'Recommends')
  );

CREATE VIEW disk_usage_by_section AS
SELECT
    coalesce(section, '(none)') AS section,
    count(*) AS packages,
    sum(installed_size_kb) AS installed_size_kb
FROM packages
GROUP BY ALL;

-- History

CREATE VIEW recent_changes AS
SELECT *
FROM history
WHERE start_time >= (SELECT max(start_time) FROM history) - INTERVAL 30 DAY;

-- The actions are listed explicitly: a view cannot use a PIVOT whose columns come from the data, and this keeps the
-- columns the same whatever the history holds
CREATE VIEW monthly_activity AS
PIVOT (
    SELECT date_trunc('month', start_time)::DATE AS month, action
    FROM history
)
ON action IN ('install', 'reinstall', 'upgrade', 'downgrade', 'remove', 'purge')
USING count(*)
GROUP BY month
ORDER BY month;

CREATE VIEW user_activity AS
SELECT
    coalesce(requested_by, '(automatic)') AS requested_by,
    count(*) AS changes,
    count(*) FILTER (WHERE action IN ('install', 'reinstall')) AS installs,
    count(*) FILTER (WHERE action IN ('upgrade', 'downgrade')) AS upgrades,
    count(*) FILTER (WHERE action IN ('remove', 'purge')) AS removals,
    min(start_time) AS first_change,
    max(start_time) AS last_change
FROM history
GROUP BY ALL;

CREATE VIEW failed_transactions AS
SELECT
    start_time,
    end_time,
    commandline,
    requested_by,
    error,
    list(package ORDER BY package) AS packages
FROM history
WHERE error IS NOT NULL
GROUP BY ALL;

-- Overviews

CREATE VIEW package_overview AS
WITH origins AS (
    SELECT
        o.name,
        o.architecture,
        bool_or(o.is_installed_version) AS installed_version_published,
        list(DISTINCT concat_ws('/', r.suite, r.component)) FILTER (WHERE o.is_installed_version) AS installed_from
    FROM package_origins o
    JOIN repositories r USING (repository_id)
    GROUP BY ALL
)
SELECT
    p.name,
    p.architecture,
    p.version,
    p.section,
    p.installed_size_kb,
    regexp_replace(p.maintainer, '\s*<.*>', '') AS maintainer,
    p.auto_installed,
    p.held,
    p.installed_time,
    o.name IS NULL AS orphaned,
    coalesce(o.installed_version_published, false) AS installed_version_published,
    o.installed_from,
    u.new_version AS available_update,
    coalesce(u.is_security, false) AS security_update,
    d.direct_dependents,
    p.purl
FROM packages p
LEFT JOIN origins o USING (name, architecture)
LEFT JOIN upgrades u USING (name, architecture)
LEFT JOIN package_dependents d USING (name, architecture);

CREATE VIEW security_posture AS
SELECT
    s.created_at AS snapshot_time,
    (SELECT count(*) FROM packages) AS installed_packages,
    (SELECT count(*) FROM packages WHERE NOT auto_installed) AS installed_on_purpose,
    (SELECT count(*) FROM third_party_packages) AS third_party_packages,
    (SELECT count(*) FROM orphaned_packages) AS orphaned_packages,
    (SELECT count(*) FROM packages_not_in_repositories) AS versions_not_in_repositories,
    (SELECT count(*) FROM upgrades) AS pending_updates,
    (SELECT count(*) FROM security_updates) AS pending_security_updates,
    (SELECT count(*) FROM packages WHERE held) AS held_packages,
    (SELECT count(*) FROM risky_sources) AS risky_sources,
    (SELECT count(*) FROM expired_repositories) AS expired_repositories,
    (SELECT count(*) FROM unsatisfied_dependencies) AS unsatisfied_dependencies,
    (SELECT max(date) FROM repositories) AS newest_repository_index,
    (SELECT max(start_time) FROM history) AS last_package_change,
    (SELECT count(DISTINCT start_time) FROM failed_transactions) AS failed_transactions
FROM snapshot s;

-------------------------------------------------------------------------------
-- Documentation, shown by duckdb_tables(), duckdb_views() and duckdb_columns()
-------------------------------------------------------------------------------

COMMENT ON TABLE snapshot IS 'When and from which system this database was built';
COMMENT ON TABLE packages IS 'Installed packages (dpkg database)';
COMMENT ON TABLE dependencies IS 'Dependencies of the installed packages, one row per alternative';
COMMENT ON TABLE provides IS 'Virtual packages provided by the installed packages';
COMMENT ON TABLE sources IS 'Configured repositories, one row per type x URI x suite';
COMMENT ON TABLE repositories IS 'Repository indexes downloaded by apt update';
COMMENT ON TABLE package_origins IS 'Versions of the installed packages published by each repository';
COMMENT ON TABLE upgrades IS 'Installed packages with a newer version available';
COMMENT ON TABLE history IS 'Installs, upgrades and removals recorded by apt';
COMMENT ON VIEW security_posture IS 'One row of key security indicators, for dashboards and reports';
COMMENT ON VIEW package_overview IS 'One row per installed package with everything known about it: origin, pending update, dependents';
COMMENT ON VIEW orphaned_packages IS 'Installed packages no repository provides: apt never updates them';
COMMENT ON VIEW packages_not_in_repositories IS 'Installed versions no repository publishes, although the package is known (local builds, leftovers)';
COMMENT ON VIEW third_party_packages IS 'Packages installed on purpose that neither Ubuntu nor Debian publish';
COMMENT ON VIEW maintainers IS 'Installed packages per maintainer, with size and how many are third-party or orphaned';
COMMENT ON VIEW risky_sources IS 'Enabled repositories with signature checks disabled, no pinned key, or plain HTTP';
COMMENT ON VIEW repository_usage IS 'Repositories and how many installed packages each one publishes';
COMMENT ON VIEW expired_repositories IS 'Repository indexes past their Valid-Until date when the snapshot was taken';
COMMENT ON VIEW security_updates IS 'Pending updates from security repositories';
COMMENT ON VIEW held_packages IS 'Packages held with apt-mark hold, and the update they are missing';
COMMENT ON VIEW dependency_edges IS 'Dependency graph between installed packages, virtual packages resolved to their providers';
COMMENT ON VIEW package_dependents IS 'Number of installed packages depending directly on each package';
COMMENT ON VIEW unsatisfied_dependencies IS 'Hard dependencies satisfied by no installed package: should be empty on a healthy system';
COMMENT ON VIEW autoremove_candidates IS 'Packages installed as dependencies that nothing needs anymore (approximation of apt autoremove)';
COMMENT ON VIEW disk_usage_by_section IS 'Installed size per archive section';
COMMENT ON VIEW recent_changes IS 'Package changes over the last 30 days of history';
COMMENT ON VIEW monthly_activity IS 'Package changes per month, one column per action';
COMMENT ON VIEW user_activity IS 'Package changes per user (automatic changes grouped together)';
COMMENT ON VIEW failed_transactions IS 'apt runs that reported an error, with the packages involved';

COMMENT ON COLUMN snapshot.created_at IS 'When this database was built (UTC)';
COMMENT ON COLUMN snapshot.root IS 'Root directory of the audited system; NULL: the machine that built the database';
COMMENT ON COLUMN snapshot.apt_version IS 'Installed version of apt on the audited system';
COMMENT ON COLUMN snapshot.extension_version IS 'Version of the apt extension that read the data';

COMMENT ON COLUMN packages.name IS 'Package name';
COMMENT ON COLUMN packages.version IS 'Installed version (Debian format: [epoch:]upstream[-revision])';
COMMENT ON COLUMN packages.architecture IS 'Architecture (amd64, arm64, i386...), or all for architecture-independent packages';
COMMENT ON COLUMN packages.section IS 'Archive section (libs, utils, web...)';
COMMENT ON COLUMN packages.priority IS 'Debian priority: required, important, standard, optional or extra';
COMMENT ON COLUMN packages.installed_size_kb IS 'Disk space used once installed, in KiB';
COMMENT ON COLUMN packages.maintainer IS 'Maintainer, as "Name <email>"';
COMMENT ON COLUMN packages.homepage IS 'Upstream project homepage';
COMMENT ON COLUMN packages.source IS 'Source package the binary package is built from';
COMMENT ON COLUMN packages.description IS 'One-line description (synopsis)';
COMMENT ON COLUMN packages.multi_arch IS 'Multi-Arch field: same, foreign or allowed';
COMMENT ON COLUMN packages.auto_installed IS 'Installed as a dependency, not on purpose (apt-mark showauto)';
COMMENT ON COLUMN packages.held IS 'Held with apt-mark hold: skipped by upgrades';
COMMENT ON COLUMN packages.installed_time IS 'Last install or upgrade of the package';
COMMENT ON COLUMN packages.purl IS 'Package URL, as understood by vulnerability scanners';

COMMENT ON COLUMN dependencies.dependency_id IS 'Surrogate key';
COMMENT ON COLUMN dependencies.package IS 'Installed package that declares the dependency';
COMMENT ON COLUMN dependencies.architecture IS 'Architecture of the installed package';
COMMENT ON COLUMN dependencies.relation IS 'Pre-Depends, Depends, Recommends or Suggests';
COMMENT ON COLUMN dependencies.dependency IS 'Package (or virtual package) depended on, without architecture qualifier';
COMMENT ON COLUMN dependencies.version_constraint IS 'Version constraint, e.g. ">= 2.34"; NULL: any version';
COMMENT ON COLUMN dependencies.alternative IS 'Part of an "a | b" alternative: any one of them satisfies the dependency';

COMMENT ON COLUMN provides.provide_id IS 'Surrogate key';
COMMENT ON COLUMN provides.package IS 'Installed package providing the virtual package';
COMMENT ON COLUMN provides.architecture IS 'Architecture of the installed package';
COMMENT ON COLUMN provides.provides IS 'Virtual package name, e.g. mail-transport-agent';
COMMENT ON COLUMN provides.version IS 'Version provided; NULL: unversioned';

COMMENT ON COLUMN sources.source_id IS 'Surrogate key';
COMMENT ON COLUMN sources.file IS 'Configuration file declaring the repository';
COMMENT ON COLUMN sources.format IS 'one-line (.list) or deb822 (.sources)';
COMMENT ON COLUMN sources.enabled IS 'false: ignored by apt (Enabled: no)';
COMMENT ON COLUMN sources.type IS 'deb (binary packages) or deb-src (source packages)';
COMMENT ON COLUMN sources.uri IS 'Repository URI; http:// exposes the software inventory to the network';
COMMENT ON COLUMN sources.suite IS 'Suite or codename, e.g. bookworm-security';
COMMENT ON COLUMN sources.components IS 'Components, e.g. [main, contrib]';
COMMENT ON COLUMN sources.architectures IS 'Architectures restricted to; NULL: all configured architectures';
COMMENT ON COLUMN sources.signed_by IS 'Key the repository must be signed with; NULL: any key of the global keyring';
COMMENT ON COLUMN sources.trusted IS 'trusted=yes: signature checks disabled';

COMMENT ON COLUMN repositories.repository_id IS 'Surrogate key';
COMMENT ON COLUMN repositories.site IS 'Host and path of the repository';
COMMENT ON COLUMN repositories.suite IS 'Suite, from the Release file (e.g. stable-security)';
COMMENT ON COLUMN repositories.codename IS 'Codename, from the Release file (e.g. bookworm-security)';
COMMENT ON COLUMN repositories.component IS 'Component (main, universe...)';
COMMENT ON COLUMN repositories.origin IS 'Origin, from the Release file (e.g. Debian, Ubuntu)';
COMMENT ON COLUMN repositories.label IS 'Label, from the Release file (e.g. Debian-Security)';
COMMENT ON COLUMN repositories.architectures IS 'Architectures whose index was downloaded';
COMMENT ON COLUMN repositories.packages IS 'Number of packages (name and architecture) in the index';
COMMENT ON COLUMN repositories.date IS 'Publication date of the index (UTC)';
COMMENT ON COLUMN repositories.valid_until IS 'apt refuses the index after this date (UTC); NULL: no expiry';
COMMENT ON COLUMN repositories.not_automatic IS 'NotAutomatic: never upgraded from automatically (experimental, backports)';
COMMENT ON COLUMN repositories.but_automatic_upgrades IS 'ButAutomaticUpgrades: upgrades packages already installed from it (backports)';
COMMENT ON COLUMN repositories.is_security IS 'Security repository (suite ending in -security, or label mentioning security)';

COMMENT ON COLUMN package_origins.name IS 'Installed package';
COMMENT ON COLUMN package_origins.architecture IS 'Architecture of the installed package';
COMMENT ON COLUMN package_origins.repository_id IS 'Repository publishing the version';
COMMENT ON COLUMN package_origins.installed_version IS 'Installed version';
COMMENT ON COLUMN package_origins.available_version IS 'Version published by the repository';
COMMENT ON COLUMN package_origins.is_installed_version IS 'The repository publishes the installed version';

COMMENT ON COLUMN upgrades.name IS 'Installed package';
COMMENT ON COLUMN upgrades.architecture IS 'Architecture of the installed package';
COMMENT ON COLUMN upgrades.current_version IS 'Installed version';
COMMENT ON COLUMN upgrades.new_version IS 'Version apt would install (apt default policy, pins not applied)';
COMMENT ON COLUMN upgrades.repository IS 'Suites publishing the new version';
COMMENT ON COLUMN upgrades.is_security IS 'The new version comes from a security repository';

COMMENT ON COLUMN history.history_id IS 'Surrogate key, in chronological order';
COMMENT ON COLUMN history.start_time IS 'Start of the apt run (local time)';
COMMENT ON COLUMN history.end_time IS 'End of the apt run (local time)';
COMMENT ON COLUMN history.action IS 'install, reinstall, upgrade, downgrade, remove or purge';
COMMENT ON COLUMN history.package IS 'Package changed (may no longer be installed)';
COMMENT ON COLUMN history.architecture IS 'Architecture of the package';
COMMENT ON COLUMN history.old_version IS 'Version before the change (upgrade, downgrade, remove, purge)';
COMMENT ON COLUMN history.new_version IS 'Version after the change (install, reinstall, upgrade, downgrade)';
COMMENT ON COLUMN history.automatic IS 'Installed as a dependency of the requested packages';
COMMENT ON COLUMN history.requested_by IS 'User who requested the change; NULL for automatic changes';
COMMENT ON COLUMN history.commandline IS 'Command that made the change';
COMMENT ON COLUMN history.error IS 'Error reported by the apt run, if any';

COMMIT;

SELECT 'packages' AS "table", count(*) AS "rows" FROM packages
UNION ALL SELECT 'dependencies', count(*) FROM dependencies
UNION ALL SELECT 'provides', count(*) FROM provides
UNION ALL SELECT 'sources', count(*) FROM sources
UNION ALL SELECT 'repositories', count(*) FROM repositories
UNION ALL SELECT 'package_origins', count(*) FROM package_origins
UNION ALL SELECT 'upgrades', count(*) FROM upgrades
UNION ALL SELECT 'history', count(*) FROM history;
