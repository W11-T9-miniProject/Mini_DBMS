-- Indexed lookup: at most one file record.
SELECT * FROM play_history WHERE id = 2;
-- Non-indexed lookup: examine every file record.
SELECT * FROM play_history WHERE song_id = 108;
-- Missing indexed key: zero file records read, not an error.
SELECT * FROM play_history WHERE id = 999999999;
