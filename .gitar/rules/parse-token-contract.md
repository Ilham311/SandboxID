---
title: "Parse-token contract guard"
description: "Flag output-string / prop-key edits that can silently break the WebUI parser"
when: "A pull request changes printed/log strings in action.sh, rotate_ids.sh, helpers.sh, or jni/sandboxid.cpp, OR changes the parsing/rendering in webroot/app.js (summarizeAction, summarizeRotate, classifyLine, parseProp, DETAIL_KEYS, ROT_CARDS)"
actions: "Post one review comment asking the author to confirm both sides of the shell↔WebUI contract were updated in lockstep; list which specific token(s) the diff touches and the matching app.js regex, and mark it Critical if the change could turn a failure into a reported success"
---

# Parse-token contract guard

`webroot/app.js` parses the stdout of `action.sh` / `rotate_ids.sh` (and native
`sandboxid` output) and reads `identity.prop` by key. Editing one side without
the other silently mis-reports state — no error is raised.

When this rule matches:

- Cross-reference `.gitar/documents/parse-token-safelist.md` and name the exact
  producer string and consumer regex involved.
- Verify the literal success banners (`OK - persona baru aktif`,
  `OK - fresh persona ready`), the `KEY       : value` row shape for
  `BRAND`/`MODEL`, the `[OK]`/`[WARN]`/`[ERR]`/`==>` log tags, the phrases
  `N step(s) reported failure` and `REBOOT REQUIRED`, and the `identity.prop`
  key names are all still matched.
- Escalate to **Critical** if a failure line could now be classified as success
  (the user would believe a persona applied when it did not).
