#!/usr/bin/env python3
"""Open an issue describing why the Switch build failed.

GitHub serves run logs and artifacts from a separate blob host that is not reachable on
every network, which leaves a failed run unreadable from some environments. An issue is
plain JSON on api.github.com, so it gets the error out through the one host that is
always available - and it keeps a record of what broke when.
"""

import hashlib
import json
import os
import re
import urllib.error
import urllib.request

MAX_BODY = 60000


def tail(path, lines):
    try:
        with open(path, "r", errors="replace") as fh:
            return "".join(fh.readlines()[-lines:])
    except OSError:
        return ""


def read(path):
    try:
        with open(path, "r", errors="replace") as fh:
            return fh.read()
    except OSError:
        return ""


# A link command line lists every object file in the project and runs to tens of
# thousands of characters. Left intact it pushes the actual error past the issue size
# limit, which is exactly what happened to the first link failure.
def elide(line, limit=600):
    if len(line) <= limit:
        return line
    return "{} ...[{} chars elided]... {}".format(line[:300], len(line) - 600, line[-300:])


ERROR_RE = re.compile(
    r"\b(error|fatal error|FAILED|undefined reference|cannot find|ld returned|"
    r"multiple definition|No such file or directory)\b"
)


def errors_only(log, limit=120):
    seen, out = set(), []
    for line in log.splitlines():
        if not ERROR_RE.search(line):
            continue
        line = elide(line.rstrip())
        if line in seen:
            continue
        seen.add(line)
        out.append(line)
        if len(out) == limit:
            break
    return "\n".join(out)


def section(title, text, fence=True):
    if not text.strip():
        return None
    if fence:
        text = "```\n" + text.rstrip() + "\n```"
    return "### " + title + "\n\n" + text


def signature(log):
    """A stable fingerprint of what actually broke.

    Retries - scheduled or manual - would otherwise open one issue per attempt for the
    same failure. The first few distinct error lines are enough to tell two failures
    apart without being sensitive to run ids or timing.
    """
    lines = [
        re.sub(r"\s+", " ", ln).strip()
        for ln in log.splitlines()
        if re.search(r"\b(error|fatal error|FAILED):", ln)
    ]
    seen, uniq = set(), []
    for ln in lines:
        if ln not in seen:
            seen.add(ln)
            uniq.append(ln)
        if len(uniq) == 5:
            break
    return hashlib.sha256("\n".join(uniq).encode()).hexdigest()[:16] if uniq else "nolog"


def api(url, token, data=None, method=None):
    req = urllib.request.Request(
        url,
        data=json.dumps(data).encode() if data is not None else None,
        method=method,
        headers={
            "Authorization": "Bearer " + token,
            "Accept": "application/vnd.github+json",
            "Content-Type": "application/json",
            "User-Agent": "lighthouse-switch-ci",
        },
    )
    with urllib.request.urlopen(req) as resp:
        return json.load(resp)


def main():
    repo = os.environ["REPO"]
    run_id = os.environ["RUN_ID"]
    sha = os.environ["SHA"]

    full = read("switch-build.log")
    # Errors first and deduplicated, so the thing that broke is never the part that gets
    # truncated away.
    parts = [
        p
        for p in (
            section("Errors", errors_only(full)),
            section(
                "Tail of configure/build",
                "\n".join(elide(ln) for ln in full.splitlines()[-250:]),
            ),
            section("CMakeError.log (tail)", tail("build/switch/CMakeFiles/CMakeError.log", 150)),
        )
        if p
    ]
    if not parts:
        parts = ["No log was captured - the job failed before the configure step."]

    token = os.environ["GH_TOKEN"]
    sig = signature(full)
    marker = "<!-- sig:{} -->".format(sig)

    body = "Run: https://github.com/{}/actions/runs/{}\nCommit: {}\n{}\n\n{}".format(
        repo, run_id, sha, marker, "\n\n".join(parts)
    )
    if len(body) > MAX_BODY:
        body = body[:MAX_BODY] + "\n\n...truncated..."

    # Same failure as an issue that is already open: add a line to it instead of filing
    # another one, so a retry loop does not bury the repo in duplicates.
    try:
        for issue in api(
            "https://api.github.com/repos/{}/issues?state=open&per_page=100".format(repo), token
        ):
            if marker in (issue.get("body") or ""):
                api(
                    issue["comments_url"],
                    token,
                    {"body": "Still failing the same way in run {}.".format(run_id)},
                )
                print("commented:", issue["html_url"])
                return
    except urllib.error.HTTPError as exc:
        print("could not list issues ({}), filing a new one".format(exc.code))

    print(
        "created:",
        api(
            "https://api.github.com/repos/{}/issues".format(repo),
            token,
            {"title": "switch build failed (run {})".format(run_id), "body": body},
        )["html_url"],
    )


if __name__ == "__main__":
    main()
