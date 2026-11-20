-- One row represents one playback event. Re-running this file appends rows.
INSERT INTO play_history (event_code, user_id, song_id, genre)
VALUES ('PLAY000001', 71, 305, 'rock');

INSERT INTO play_history (event_code, user_id, song_id, genre)
VALUES ('PLAY000002', 32, 108, 'pop');

INSERT INTO play_history (event_code, user_id, song_id, genre)
VALUES ('PLAY000003', 71, 108, 'pop');

SELECT * FROM play_history WHERE user_id = 71;
