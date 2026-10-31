# DuckDB apt extension

**Query the apt/dpkg package manager of Debian, Ubuntu and derived systems with SQL.**

What is installed, where it comes from, how the repositories are trusted, what apt will never update, which security fixes are pending, and who changed what: as DuckDB tables, ready to be filtered, joined, exported or turned into a security report.

```sql
INSTALL apt FROM community;
LOAD apt;

-- Which installed packages will apt never update?
SELECT name, version
FROM apt_packages()
ANTI JOIN apt_package_origins()
    USING (name, architecture);
```

[Get started](getting-started.md){ .md-button .md-button--primary }
[Function reference](functions.md){ .md-button }

## What you get

<div class="grid cards" markdown>

-   **Inventory**

    ---

    Installed packages with size, maintainer, why they were installed, held packages, last install time and a [package URL](https://github.com/package-url/purl-spec) for vulnerability scanners.

-   **Supply chain**

    ---

    Configured repositories and how they are trusted (`signed-by`, `trusted=yes`, plain HTTP), the repository each package comes from, and the orphaned packages no repository updates anymore.

-   **Patching**

    ---

    Pending updates computed from the downloaded indexes, flagged when they come from a security repository, and the freshness of each repository index.

-   **History**

    ---

    Every install, upgrade and removal recorded by apt, with the user who requested it (or none, for unattended upgrades).

</div>

## Built for audits

- **No subprocess, no root:** the extension reads the dpkg and apt files directly.
- **Sandbox friendly:** files are read through DuckDB's file system, so `SET enable_external_access = false` and `allowed_directories` restrict it like any other file access.
- **Offline audits:** every table function takes a `root` parameter to audit a mounted disk image or a container filesystem.
- **Exact versions:** `apt_version_compare()` orders Debian versions exactly like `dpkg --compare-versions`.
- **Ready-made database and report:** [a SQL script](database.md) snapshots everything into a documented database with keys and views, and [a Quarto notebook](security-report.md) renders a PDF security report from it.

!!! info "Scope"
    The extension covers what apt and dpkg manage. Snaps, Flatpaks, language package managers and manual installs are out of scope.

## Sibling project

[duckdb-brew](https://github.com/adriens/duckdb-brew) does the same for Homebrew.
