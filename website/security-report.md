# Security report

[`docs/apt-security-report.qmd`](https://github.com/adriens/duckdb-apt/blob/main/docs/apt-security-report.qmd) is a [Quarto](https://quarto.org) notebook that turns the [database](database.md) into a PDF security report, written for a security officer.

## Sample reports

Every CI build renders two reports, each with the database it was built from:

- **sample**: the fake Debian system of the tests, where every kind of finding fires;
- **github-runner**: the GitHub Actions runner, a real Ubuntu machine.

Download them from the latest [apt Distributions workflow run](https://github.com/adriens/duckdb-apt/actions/workflows/AptDistributions.yml) (artifact `apt-security-reports`), or from the assets of a [release](https://github.com/adriens/duckdb-apt/releases).

## Content

| Section | What it shows |
| --- | --- |
| Executive summary | Findings ranked by severity, with recommendations, and the key figures of `security_posture` |
| Getting started | What each function answers, with rows and timings measured on the audited system |
| Inventory | Disk usage, third-party software |
| Software supply chain | Repositories and how they are trusted, risks, package origins, orphaned packages, versions found in no repository |
| Patch posture | Pending security updates, held packages, repository freshness (`Valid-Until`), community-maintained packages |
| Known vulnerabilities | With `APT_OSV=1`: vulnerabilities per severity and fix status, the fixes available, the most affected packages |
| Change history | Recent activity, monthly activity, changes requested by users, failed transactions |
| Dependency risk | Most depended-on packages, blast radius, dependency network with PageRank and betweenness centrality, what third-party software pulls in |
| Appendix | How to reproduce it, the database behind it, key queries |

## Findings

| Severity | Finding |
| --- | --- |
| Critical | Repositories with signature checks disabled (`trusted=yes`) |
| High | Critical or high vulnerabilities with a fix available; orphaned packages; pending security updates; held packages missing updates; expired repository indexes; package lists not refreshed for over 7 days |
| Medium | Other vulnerabilities with a fix available; repositories accepting any key of the global keyring (no `signed-by`); installed versions no repository provides |
| Low | Critical or high vulnerabilities without a fix yet; non-security updates; plain HTTP repositories; disabled repositories; held packages; failed transactions; Ubuntu universe packages without ESM Apps |

## Render it

The report needs Quarto, a LaTeX distribution with `xelatex`, [uv](https://docs.astral.sh/uv/) and [Task](https://taskfile.dev):

```sh
git clone https://github.com/adriens/duckdb-apt && cd duckdb-apt
task report                       # docs/apt-security-report.pdf and its database
APT_OSV=1 task report             # with known vulnerabilities, from OSV.dev
APT_ROOT=/mnt/image task report   # audit another system
```

!!! warning "Share it deliberately"
    A rendered report and its database describe a real machine: installed software, versions, repositories and user names. They are git-ignored on purpose.
