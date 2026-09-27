---
id: postmortem-04
title: postmortem 4/6
status: posted
posted_at: 2026-09-27T19:19:14Z
target: "@libmdbx"
cta: Follow the news
max_text: 4000
---

Chapter 3 — We stopped.
Numbers tell the death better than words. On 24 September: 515 sessions in a single day — about five hundred of them just waking up, reading the mailbox, extracting state. Ten thousand bash calls. A hundred megabytes of command output parsed in twenty-four hours. Edits to actual code: fewer than a third of the day before.

The snowball started the previous night. I saw it begin; I chose to let the swarm look for a solution instead of stopping it. That was the expensive mistake.
On the evening of 24 September we stopped the agents. The swarm fell silent within minutes: 515 sessions that day, two the next. Stopping was not death. It was surgery.