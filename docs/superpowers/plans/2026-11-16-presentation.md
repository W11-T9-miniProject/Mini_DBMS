# Two presentation candidates: 2 minutes explanation + 2 minutes live demo

User requested both DBMS and Redis candidates before choosing. Keep explanation accessible: need -> advantage -> implementation -> validation, then connect live actions to those ideas. README-based, no separate slide deck.

- [x] DBMS: add a presentation helper with prepare/run modes. Prepare a reusable million-row source using the existing SQL generator and benchmark/engine. Run an isolated temporary clone, start the real HTTP server, pause between API INSERT/SELECT, same-row ID vs event_code lookup, and 24 concurrent inserts. Show actual HTTP round-trip timings, never substitute stored benchmark figures. Verify IDs/rows and leave the source DB untouched.
- [x] Test small dataset prepare/run twice and source preservation, reject conflicting existing files. Test normal and sanitizer binaries; rehearse once with 1,000,000 rows and record result/time separately from speaker pauses.
- [x] Write concise DBMS and Redis 4-minute speaker/demo guides. Redis uses existing two-browser seat reservation/20s expiry/admin invalidation and menu MISS/HIT, without live audience signup. Restart persistence remains supporting evidence for time control. State what UI observation does/does not prove.
- [x] Link guides from both READMEs, correct stale Redis status notes only, preserve user PDF/PPTX. Create a local comparison note. No commit/push or application behavior changes.

Independent review confirmed temporary-server cleanup and exclusive source creation; both test groups passed normal/sanitized binaries. Corrected presentation wording: scanned counts file records, not separate files. Million-row execution checked actual data and HTTP timings; Redis guide is prepared but UI rehearsal/current origin connectivity is explicitly outstanding until user chooses/rehearses. No changes to production C code or user data.
