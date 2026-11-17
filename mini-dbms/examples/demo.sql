INSERT INTO people VALUES (1, 'Ada', 36);
INSERT INTO people (name, id, age) VALUES ('Grace', 2, 85);
SELECT id, name FROM people WHERE age >= 30 AND age < 50;
