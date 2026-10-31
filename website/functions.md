# Functions

All table functions take an optional `root` parameter, the directory the apt and dpkg paths are resolved under (default: `/`), e.g. `apt_packages(root := '/mnt/image')`. The database tables of the [database script](database.md) have the same columns.

## `apt_packages()`

Installed packages, from the dpkg database: version, size, maintainer, whether they were installed on purpose (auto_installed), held, last install time and package URL (purl).

```sql
SELECT name, version FROM apt_packages() WHERE NOT auto_installed;
```

| Column | Type | Description |
| --- | --- | --- |
| `name` | `VARCHAR` | Package name |
| `version` | `VARCHAR` | Installed version (Debian format: [epoch:]upstream[-revision]) |
| `architecture` | `VARCHAR` | Architecture (amd64, arm64, i386...), or all for architecture-independent packages |
| `section` | `VARCHAR` | Archive section (libs, utils, web...) |
| `priority` | `VARCHAR` | Debian priority: required, important, standard, optional or extra |
| `installed_size_kb` | `BIGINT` | Disk space used once installed, in KiB |
| `maintainer` | `VARCHAR` | Maintainer, as "Name <email>" |
| `homepage` | `VARCHAR` | Upstream project homepage |
| `source` | `VARCHAR` | Source package the binary package is built from |
| `description` | `VARCHAR` | One-line description (synopsis) |
| `multi_arch` | `VARCHAR` | Multi-Arch field: same, foreign or allowed |
| `auto_installed` | `BOOLEAN` | Installed as a dependency, not on purpose (apt-mark showauto) |
| `held` | `BOOLEAN` | Held with apt-mark hold: skipped by upgrades |
| `installed_time` | `TIMESTAMP` | Last install or upgrade of the package |
| `purl` | `VARCHAR` | Package URL, as understood by vulnerability scanners |

## `apt_dependencies()`

Dependencies of the installed packages (Pre-Depends, Depends, Recommends, Suggests), one row per alternative.

```sql
SELECT package FROM apt_dependencies() WHERE dependency = 'libssl3t64';
```

| Column | Type | Description |
| --- | --- | --- |
| `package` | `VARCHAR` | Installed package that declares the dependency |
| `architecture` | `VARCHAR` | Architecture of the installed package |
| `relation` | `VARCHAR` | Pre-Depends, Depends, Recommends or Suggests |
| `dependency` | `VARCHAR` | Package (or virtual package) depended on, without architecture qualifier |
| `version_constraint` | `VARCHAR` | Version constraint, e.g. ">= 2.34"; NULL: any version |
| `alternative` | `BOOLEAN` | Part of an "a | b" alternative: any one of them satisfies the dependency |

## `apt_provides()`

Virtual packages provided by the installed packages.

```sql
SELECT package FROM apt_provides() WHERE provides = 'mail-transport-agent';
```

| Column | Type | Description |
| --- | --- | --- |
| `package` | `VARCHAR` | Installed package providing the virtual package |
| `architecture` | `VARCHAR` | Architecture of the installed package |
| `provides` | `VARCHAR` | Virtual package name, e.g. mail-transport-agent |
| `version` | `VARCHAR` | Version provided; NULL: unversioned |

## `apt_sources()`

Configured repositories (sources.list and sources.list.d, one-line and deb822 formats), one row per type x URI x suite, with how they are trusted.

```sql
SELECT uri, suite FROM apt_sources() WHERE enabled AND signed_by IS NULL;
```

| Column | Type | Description |
| --- | --- | --- |
| `file` | `VARCHAR` | Configuration file declaring the repository |
| `format` | `VARCHAR` | one-line (.list) or deb822 (.sources) |
| `enabled` | `BOOLEAN` | false: ignored by apt (Enabled: no) |
| `type` | `VARCHAR` | deb (binary packages) or deb-src (source packages) |
| `uri` | `VARCHAR` | Repository URI; http:// exposes the software inventory to the network |
| `suite` | `VARCHAR` | Suite or codename, e.g. bookworm-security |
| `components` | `VARCHAR[]` | Components, e.g. [main, contrib] |
| `architectures` | `VARCHAR[]` | Architectures restricted to; NULL: all configured architectures |
| `signed_by` | `VARCHAR` | Key the repository must be signed with; NULL: any key of the global keyring |
| `trusted` | `BOOLEAN` | trusted=yes: signature checks disabled |

## `apt_repositories()`

Repositories known from the indexes downloaded by apt update: origin, suite, component, number of packages, release date and validity, and whether they are security repositories.

```sql
SELECT site, suite, date FROM apt_repositories() ORDER BY date;
```

| Column | Type | Description |
| --- | --- | --- |
| `site` | `VARCHAR` | Host and path of the repository |
| `suite` | `VARCHAR` | Suite, from the Release file (e.g. stable-security) |
| `codename` | `VARCHAR` | Codename, from the Release file (e.g. bookworm-security) |
| `component` | `VARCHAR` | Component (main, universe...) |
| `origin` | `VARCHAR` | Origin, from the Release file (e.g. Debian, Ubuntu) |
| `label` | `VARCHAR` | Label, from the Release file (e.g. Debian-Security) |
| `architectures` | `VARCHAR[]` | Architectures whose index was downloaded |
| `packages` | `BIGINT` | Number of packages (name and architecture) in the index |
| `date` | `TIMESTAMP` | Publication date of the index (UTC) |
| `valid_until` | `TIMESTAMP` | apt refuses the index after this date (UTC); NULL: no expiry |
| `not_automatic` | `BOOLEAN` | NotAutomatic: never upgraded from automatically (experimental, backports) |
| `but_automatic_upgrades` | `BOOLEAN` | ButAutomaticUpgrades: upgrades packages already installed from it (backports) |
| `is_security` | `BOOLEAN` | Security repository (suite ending in -security, or label mentioning security) |

## `apt_package_origins()`

Repositories publishing each installed package, with the version they publish: an installed package without any row is unknown to every repository and never updated by apt.

```sql
SELECT name FROM apt_packages() ANTI JOIN apt_package_origins() USING (name, architecture);
```

| Column | Type | Description |
| --- | --- | --- |
| `name` | `VARCHAR` | Installed package |
| `architecture` | `VARCHAR` | Architecture of the installed package |
| `installed_version` | `VARCHAR` | Installed version |
| `available_version` | `VARCHAR` | Version published by the repository |
| `is_installed_version` | `BOOLEAN` | The repository publishes the installed version |
| `origin` | `VARCHAR` | Origin of the repository, from its Release file (e.g. Debian, Ubuntu) |
| `label` | `VARCHAR` | Label of the repository, from its Release file |
| `suite` | `VARCHAR` | Suite of the repository (e.g. stable-security) |
| `codename` | `VARCHAR` | Codename of the repository (e.g. bookworm-security) |
| `component` | `VARCHAR` | Component of the repository (main, universe...) |
| `site` | `VARCHAR` | Host and path of the repository |

## `apt_upgradable()`

Installed packages with a newer version available, computed from the downloaded indexes with apt's default policy (pins are not applied), and whether the update comes from a security repository.

```sql
SELECT name, current_version, new_version FROM apt_upgradable() WHERE is_security;
```

| Column | Type | Description |
| --- | --- | --- |
| `name` | `VARCHAR` | Installed package |
| `architecture` | `VARCHAR` | Architecture of the installed package |
| `current_version` | `VARCHAR` | Installed version |
| `new_version` | `VARCHAR` | Version apt would install (apt default policy, pins not applied) |
| `repository` | `VARCHAR` | Suites publishing the new version |
| `is_security` | `BOOLEAN` | The new version comes from a security repository |

## `apt_history()`

Package installs, upgrades and removals recorded by apt (history.log and its rotated .gz files), with who requested them: requested_by is NULL for automatic updates such as unattended-upgrades.

```sql
SELECT start_time, action, package, requested_by FROM apt_history() ORDER BY start_time DESC;
```

| Column | Type | Description |
| --- | --- | --- |
| `start_time` | `TIMESTAMP` | Start of the apt run (local time) |
| `end_time` | `TIMESTAMP` | End of the apt run (local time) |
| `action` | `VARCHAR` | install, reinstall, upgrade, downgrade, remove or purge |
| `package` | `VARCHAR` | Package changed (may no longer be installed) |
| `architecture` | `VARCHAR` | Architecture of the package |
| `old_version` | `VARCHAR` | Version before the change (upgrade, downgrade, remove, purge) |
| `new_version` | `VARCHAR` | Version after the change (install, reinstall, upgrade, downgrade) |
| `automatic` | `BOOLEAN` | Installed as a dependency of the requested packages |
| `requested_by` | `VARCHAR` | User who requested the change; NULL for automatic changes |
| `commandline` | `VARCHAR` | Command that made the change |
| `error` | `VARCHAR` | Error reported by the apt run, if any |

## `apt_version([root])`

Installed version of apt, read from the dpkg database (NULL when apt is not installed). With `root`, the version on the system mounted there.

```sql
SELECT apt_version();
SELECT apt_version('/mnt/image');
```

## `apt_version_compare(a, b)`

Compares two Debian versions exactly like `dpkg --compare-versions`: epoch first, then upstream version and revision, with `~` sorting before everything. Returns -1, 0 or 1 (NULL if either is NULL). A plain string comparison gets `'1.10' < '1.9'` wrong.

```sql
SELECT apt_version_compare('1:1.0', '2.0');   -- 1: the epoch wins
SELECT apt_version_compare('1.0~rc1', '1.0'); -- -1: ~ sorts first
SELECT apt_version_compare('1.10', '1.9');    -- 1
```

