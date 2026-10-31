"""Adds the known vulnerabilities of the installed packages to a database built by apt_database.sql.

The installed source packages and their versions are looked up in OSV.dev (https://osv.dev), which publishes the
Debian security tracker and Ubuntu's security notices: this sends the names and versions of the installed packages
to OSV.dev, nothing else. The results are stored in the database:

- vulnerabilities: one row per OSV record (CVE, severity, CVSS vector, link);
- package_vulnerabilities: one row per installed package and vulnerability not fixed in its installed version, with
  the version fixing it, if any, and whether a pending update installs it;
- vulnerability_posture: counts per fix status and severity.

Usage:
    python3 osv_vulnerabilities.py apt.duckdb [--duckdb duckdb] [--load "LOAD apt"]

The extension must be loadable (apt_version_compare() orders the versions); the tables it creates can then be read
without it.
"""

import argparse
import concurrent.futures
import json
import os
import re
import subprocess
import sys
import tempfile
import time
import urllib.request

OSV_API = "https://api.osv.dev/v1"
BATCH_SIZE = 1000
SEVERITIES = {
    # Ubuntu priorities
    "negligible": "negligible", "low": "low", "medium": "medium", "high": "high", "critical": "critical",
    # Debian urgencies
    "unimportant": "negligible", "low*": "low", "medium*": "medium", "high*": "high",
}


def duckdb(executable, database, sql, load=None, read_only=False):
    """Runs SQL with the duckdb CLI and returns the rows of the last statement as dictionaries."""
    env = {key: value for key, value in os.environ.items() if key not in ("AI_AGENT", "CLAUDECODE")}
    command = [executable, "-unsigned", "-json"] + (["-readonly"] if read_only else []) + [database]
    if load:
        command += ["-cmd", load]
    result = subprocess.run(command + ["-c", sql], capture_output=True, text=True, env=env)
    if result.returncode != 0:
        raise RuntimeError(f"duckdb failed: {result.stderr}\n{sql}")
    output = result.stdout.strip()
    return json.loads(output) if output else []


def request(url, payload=None, attempts=4):
    data = json.dumps(payload).encode() if payload is not None else None
    for attempt in range(attempts):
        try:
            http = urllib.request.Request(url, data=data, headers={"Content-Type": "application/json"})
            with urllib.request.urlopen(http, timeout=60) as response:
                return json.loads(response.read())
        except Exception:
            if attempt == attempts - 1:
                raise
            time.sleep(2 ** attempt)


def ecosystems(purl_distro, esm):
    """OSV ecosystems of a distribution, e.g. "ubuntu-24.04" -> ["Ubuntu:24.04:LTS", "Ubuntu:Pro:24.04:LTS"]."""
    match = re.fullmatch(r"(debian|ubuntu)-(\d+(?:\.\d+)?)", purl_distro or "")
    if not match:
        raise SystemExit(f"OSV lookups support Debian and Ubuntu releases only, not \"{purl_distro}\"")
    os_id, version = match.groups()
    if os_id == "debian":
        return [f"Debian:{version.split('.')[0]}"]
    year, month = version.split(".")
    # Ubuntu LTS releases are the .04 ones of even years; Ubuntu Pro (ESM) fixes count when ESM is enabled
    lts = month == "04" and int(year) % 2 == 0
    standard = f"Ubuntu:{version}:LTS" if lts else f"Ubuntu:{version}"
    return [standard] + ([f"Ubuntu:Pro:{version}:LTS"] if lts and esm else [])


# CVSS 3.x base metrics (https://www.first.org/cvss/v3.1/specification-document)
CVSS3_WEIGHTS = {
    "AV": {"N": 0.85, "A": 0.62, "L": 0.55, "P": 0.2},
    "AC": {"L": 0.77, "H": 0.44},
    "UI": {"N": 0.85, "R": 0.62},
    "C": {"H": 0.56, "L": 0.22, "N": 0.0},
    "I": {"H": 0.56, "L": 0.22, "N": 0.0},
    "A": {"H": 0.56, "L": 0.22, "N": 0.0},
}


def cvss3_base_score(vector):
    """Base score of a CVSS 3.0/3.1 vector, e.g. "CVSS:3.1/AV:N/AC:L/PR:N/UI:N/S:U/C:H/I:H/A:H" -> 9.8."""
    if not vector or not vector.startswith("CVSS:3"):
        return None
    try:
        metrics = dict(part.split(":") for part in vector.split("/")[1:])
        changed = metrics["S"] == "C"
        pr = {"N": 0.85, "L": 0.68 if changed else 0.62, "H": 0.5 if changed else 0.27}[metrics["PR"]]
        weight = {key: CVSS3_WEIGHTS[key][metrics[key]] for key in CVSS3_WEIGHTS}
    except (KeyError, ValueError):
        return None
    iss = 1 - (1 - weight["C"]) * (1 - weight["I"]) * (1 - weight["A"])
    impact = 7.52 * (iss - 0.029) - 3.25 * (iss - 0.02) ** 15 if changed else 6.42 * iss
    exploitability = 8.22 * weight["AV"] * weight["AC"] * pr * weight["UI"]
    if impact <= 0:
        return 0.0
    score = min((1.08 if changed else 1) * (impact + exploitability), 10)
    # Round up to one decimal, as the specification does, avoiding floating point artefacts
    integer = round(score * 100000)
    return integer / 100000.0 if integer % 10000 == 0 else (integer // 10000 + 1) / 10.0


def cvss3_severity(score):
    if score is None:
        return None
    return "none" if score == 0 else "low" if score < 4 else "medium" if score < 7 else "high" if score < 9 else "critical"


def matching_affected(record, ecosystem, source):
    for affected in record.get("affected", []):
        package = affected.get("package", {})
        if package.get("ecosystem") == ecosystem and package.get("name") == source:
            yield affected


def severity(record, affected):
    for entry in record.get("severity", []):
        if entry.get("type") == "Ubuntu":
            return SEVERITIES.get(str(entry.get("score")).lower(), "unknown")
    urgency = str((affected or {}).get("ecosystem_specific", {}).get("urgency", "")).lower()
    return SEVERITIES.get(urgency, "unknown")


def fixed_versions(affected):
    for range_ in affected.get("ranges", []):
        for event in range_.get("events", []):
            if "fixed" in event:
                yield event["fixed"]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("database")
    parser.add_argument("--duckdb", default=os.environ.get("DUCKDB", "duckdb"))
    parser.add_argument("--load", default="LOAD apt;", help="statement loading the apt extension")
    args = parser.parse_args()

    context = duckdb(args.duckdb, args.database, """
        SELECT
            any_value(regexp_extract(purl, 'distro=([^&]+)', 1)) AS distro,
            (SELECT count(*) > 0 FROM sources WHERE enabled AND uri LIKE '%esm.ubuntu.com%') AS esm
        FROM packages
    """, read_only=True)[0]
    eco = ecosystems(context["distro"], context["esm"])
    standard = eco[0]
    pairs = duckdb(args.duckdb, args.database, "SELECT DISTINCT source, version FROM packages ORDER BY ALL",
                   read_only=True)
    print(f"Looking up {len(pairs)} source package versions in OSV.dev ({', '.join(eco)})", file=sys.stderr)

    # 1. Vulnerabilities affecting each installed (source, version), in the distribution's ecosystem
    matches = []
    for start in range(0, len(pairs), BATCH_SIZE):
        chunk = pairs[start:start + BATCH_SIZE]
        queries = [{"package": {"name": p["source"], "ecosystem": standard}, "version": p["version"]} for p in chunk]
        while queries:
            results = request(f"{OSV_API}/querybatch", {"queries": queries})["results"]
            next_queries = []
            for query, result in zip(queries, results):
                for vuln in result.get("vulns", []):
                    matches.append({"source": query["package"]["name"], "version": query["version"],
                                    "vuln_id": vuln["id"]})
                if result.get("next_page_token"):
                    next_queries.append({**query, "page_token": result["next_page_token"]})
            queries = next_queries

    # 2. The records, for severity and fixed versions
    ids = sorted({m["vuln_id"] for m in matches})
    print(f"Fetching {len(ids)} vulnerability records", file=sys.stderr)
    with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
        records = dict(zip(ids, pool.map(lambda vuln_id: request(f"{OSV_API}/vulns/{vuln_id}"), ids)))

    vulnerabilities, fixes = [], []
    sources_of = {}
    for match in matches:
        sources_of.setdefault(match["vuln_id"], set()).add(match["source"])
    for vuln_id, record in records.items():
        first_affected = next(
            (a for source in sources_of[vuln_id] for a in matching_affected(record, standard, source)), None)
        cves = [alias for alias in record.get("upstream", []) + record.get("aliases", []) if alias.startswith("CVE-")]
        cvss = next((s["score"] for s in record.get("severity", []) if s.get("type", "") == "CVSS_V3"), None)
        cvss_score = cvss3_base_score(cvss)
        # The distribution's own assessment first, the CVSS base score otherwise
        distribution_severity = severity(record, first_affected)
        if distribution_severity != "unknown" or cvss_score is None:
            final_severity, severity_source = distribution_severity, "distribution"
        else:
            final_severity, severity_source = cvss3_severity(cvss_score), "cvss"
        details = (record.get("summary") or record.get("details") or "").strip().split("\n")[0][:300]
        vulnerabilities.append({
            "vuln_id": vuln_id,
            "cve": cves[0] if cves else None,
            "published": record.get("published"),
            "severity": final_severity,
            "severity_source": severity_source,
            "cvss_vector": cvss,
            "cvss_score": cvss_score,
            "details": details,
            "url": f"https://osv.dev/vulnerability/{vuln_id}",
        })
        for source in sources_of[vuln_id]:
            for ecosystem in eco:
                for affected in matching_affected(record, ecosystem, source):
                    for fixed in fixed_versions(affected):
                        fixes.append({"vuln_id": vuln_id, "source": source, "ecosystem": ecosystem,
                                      "fixed_version": fixed})

    with tempfile.TemporaryDirectory() as directory:
        files = {}
        for name, rows in [("vulnerabilities", vulnerabilities), ("matches", matches), ("fixes", fixes)]:
            files[name] = os.path.join(directory, f"{name}.json")
            with open(files[name], "w") as handle:
                json.dump(rows, handle)
        duckdb(args.duckdb, args.database, LOAD_SQL.format(**files), load=args.load)

    summary = duckdb(args.duckdb, args.database, "FROM vulnerability_posture", read_only=True)
    print(json.dumps(summary, indent=2, default=str), file=sys.stderr)


# The versions are compared with apt_version_compare(), like dpkg, when loading: the tables can then be read without
# the extension. A vulnerability is kept for a package unless one of its fixed versions is already installed.
LOAD_SQL = """
BEGIN TRANSACTION;
DROP VIEW IF EXISTS vulnerability_posture;
DROP TABLE IF EXISTS package_vulnerabilities;
DROP TABLE IF EXISTS vulnerabilities;

CREATE TABLE vulnerabilities (
    vuln_id VARCHAR PRIMARY KEY,
    cve VARCHAR,
    published TIMESTAMP,
    severity VARCHAR NOT NULL,
    severity_source VARCHAR NOT NULL,
    cvss_vector VARCHAR,
    cvss_score DOUBLE,
    details VARCHAR,
    url VARCHAR NOT NULL
);
INSERT INTO vulnerabilities
SELECT vuln_id, cve, published::TIMESTAMP, severity, severity_source, cvss_vector, cvss_score, details, url
FROM read_json('{vulnerabilities}', columns = {{
    vuln_id: 'VARCHAR', cve: 'VARCHAR', published: 'VARCHAR', severity: 'VARCHAR', severity_source: 'VARCHAR',
    cvss_vector: 'VARCHAR', cvss_score: 'DOUBLE', details: 'VARCHAR', url: 'VARCHAR'
}});

CREATE TABLE package_vulnerabilities (
    name VARCHAR NOT NULL,
    architecture VARCHAR NOT NULL,
    version VARCHAR NOT NULL,
    source VARCHAR NOT NULL,
    vuln_id VARCHAR NOT NULL,
    fixed_version VARCHAR,
    fix_status VARCHAR NOT NULL,
    fixed_by_pending_update BOOLEAN NOT NULL,
    PRIMARY KEY (name, architecture, vuln_id),
    FOREIGN KEY (name, architecture) REFERENCES packages (name, architecture),
    FOREIGN KEY (vuln_id) REFERENCES vulnerabilities (vuln_id)
);
INSERT INTO package_vulnerabilities
WITH matches AS (
    SELECT DISTINCT * FROM read_json('{matches}', columns = {{source: 'VARCHAR', version: 'VARCHAR', vuln_id: 'VARCHAR'}})
),
fixes AS (
    SELECT * FROM read_json('{fixes}', columns = {{
        vuln_id: 'VARCHAR', source: 'VARCHAR', ecosystem: 'VARCHAR', fixed_version: 'VARCHAR'
    }})
),
candidates AS (
    SELECT
        p.name,
        p.architecture,
        p.version,
        p.source,
        m.vuln_id,
        bool_or(coalesce(apt_version_compare(p.version, f.fixed_version) >= 0, false)) AS already_fixed,
        any_value(f.fixed_version) FILTER (WHERE apt_version_compare(p.version, f.fixed_version) < 0) AS fixed_version
    FROM packages p
    JOIN matches m ON m.source = p.source AND m.version = p.version
    LEFT JOIN fixes f ON f.vuln_id = m.vuln_id AND f.source = m.source
    GROUP BY ALL
)
SELECT
    c.name,
    c.architecture,
    c.version,
    c.source,
    c.vuln_id,
    c.fixed_version,
    CASE WHEN c.fixed_version IS NULL THEN 'no fix yet' ELSE 'update available' END,
    coalesce(apt_version_compare(u.new_version, c.fixed_version) >= 0, false)
FROM candidates c
LEFT JOIN upgrades u USING (name, architecture)
WHERE NOT c.already_fixed;

CREATE VIEW vulnerability_posture AS
SELECT
    pv.fix_status,
    v.severity,
    count(DISTINCT coalesce(v.cve, v.vuln_id)) AS vulnerabilities,
    count(DISTINCT (pv.name, pv.architecture)) AS packages,
    count(DISTINCT coalesce(v.cve, v.vuln_id)) FILTER (WHERE pv.fixed_by_pending_update) AS fixed_by_pending_updates
FROM package_vulnerabilities pv
JOIN vulnerabilities v USING (vuln_id)
GROUP BY ALL
ORDER BY ALL;

COMMENT ON TABLE vulnerabilities IS 'Known vulnerabilities affecting installed packages, from OSV.dev';
COMMENT ON COLUMN vulnerabilities.vuln_id IS 'OSV identifier, e.g. UBUNTU-CVE-2024-7264 or DEBIAN-CVE-2024-2004';
COMMENT ON COLUMN vulnerabilities.cve IS 'CVE identifier';
COMMENT ON COLUMN vulnerabilities.published IS 'Publication date of the vulnerability (UTC)';
COMMENT ON COLUMN vulnerabilities.severity IS 'negligible, none, low, medium, high, critical or unknown';
COMMENT ON COLUMN vulnerabilities.severity_source IS 'distribution: assessed by Debian or Ubuntu; cvss: from the CVSS base score';
COMMENT ON COLUMN vulnerabilities.cvss_vector IS 'CVSS 3.x vector, when published';
COMMENT ON COLUMN vulnerabilities.cvss_score IS 'CVSS 3.x base score computed from the vector (0 to 10)';
COMMENT ON COLUMN vulnerabilities.details IS 'Description (first line)';
COMMENT ON COLUMN vulnerabilities.url IS 'OSV.dev page of the vulnerability';
COMMENT ON TABLE package_vulnerabilities IS 'Installed packages affected by a known vulnerability: not fixed in the installed version';
COMMENT ON COLUMN package_vulnerabilities.name IS 'Installed package';
COMMENT ON COLUMN package_vulnerabilities.architecture IS 'Architecture of the installed package';
COMMENT ON COLUMN package_vulnerabilities.version IS 'Installed version';
COMMENT ON COLUMN package_vulnerabilities.source IS 'Source package, as named by the security trackers';
COMMENT ON COLUMN package_vulnerabilities.vuln_id IS 'Vulnerability affecting the installed version';
COMMENT ON COLUMN package_vulnerabilities.fixed_version IS 'Version fixing the vulnerability; NULL: no fix published yet';
COMMENT ON COLUMN package_vulnerabilities.fix_status IS 'update available, or no fix yet';
COMMENT ON COLUMN package_vulnerabilities.fixed_by_pending_update IS 'A pending update (upgrades table) installs the fix';
COMMENT ON VIEW vulnerability_posture IS 'Vulnerabilities and affected packages per fix status and severity';
COMMIT;
"""

if __name__ == "__main__":
    main()
