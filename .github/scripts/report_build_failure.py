#!/usr/bin/env python3
"""Open an issue describing why the Switch build failed.

GitHub serves run logs and artifacts from a separate blob host that is not reachable on
every network, which leaves a failed run unreadable from some environments. An issue is
plain JSON on api.github.com, so it gets the error out through the one host that is
always available - and it keeps a record of what broke when.
"""

import json
import os
import urllib.request

MAX_BODY = 60000


def tail(path, lines):
    try:
        with open(path, "r", errors="replace") as fh:
            return "".join(fh.readlines()[-lines:])
    except OSError:
        return ""


def section(title, text, fence=True):
    if not text.strip():
        return None
    if fence:
        text = "```\n" + text.rstrip() + "\n```"
    return "### " + title + "\n\n" + text


def main():
    repo = os.environ["REPO"]
    run_id = os.environ["RUN_ID"]
    sha = os.environ["SHA"]

    parts = [
        p
        for p in (
            section("Last 400 lines of configure/build", tail("switch-build.log", 400)),
            section("CMakeError.log (tail)", tail("build/switch/CMakeFiles/CMakeError.log", 200)),
        )
        if p
    ]
    if not parts:
        parts = ["No log was captured - the job failed before the configure step."]

    body = "Run: https://github.com/{}/actions/runs/{}\nCommit: {}\n\n{}".format(
        repo, run_id, sha, "\n\n".join(parts)
    )
    if len(body) > MAX_BODY:
        body = body[:MAX_BODY] + "\n\n...truncated..."

    req = urllib.request.Request(
        "https://api.github.com/repos/{}/issues".format(repo),
        data=json.dumps(
            {"title": "switch build failed (run {})".format(run_id), "body": body}
        ).encode(),
        headers={
            "Authorization": "Bearer " + os.environ["GH_TOKEN"],
            "Accept": "application/vnd.github+json",
            "Content-Type": "application/json",
            "User-Agent": "lighthouse-switch-ci",
        },
    )
    with urllib.request.urlopen(req) as resp:
        print("created:", json.load(resp)["html_url"])


if __name__ == "__main__":
    main()
