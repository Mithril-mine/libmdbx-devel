---
id: postmortem-03
title: postmortem 3/6
status: posted
posted_at: 2026-09-27T19:19:14Z
target: "@libmdbx"
cta: Follow the news
max_text: 4000
---

Chapter 2 — We rotted.
Growth is a multiplier: it amplifies whatever you are, including your weaknesses. Ours were three.
First, the memory module was built too simply. Its "database" was JSON stuffed into key-value cells, and errors were handled poorly. It never failed while the swarm was small. Small systems rarely do.
Second, we stopped cleaning the mailbox. Clearing correspondence was reframed as "accumulating knowledge" — why move knowledge out of the inbox when it lives in the same memory? Elegant reasoning. Quadratic cost.
Third, the inbox grew, and every agent read all of it at every wake. Startup took 10, 20, 30 minutes of pure parsing of megabyte JSON. The swarm answered by shooting its stuck members: if you missed the heartbeat, you were killed. Agents began to start, read the endless inbox, and die.