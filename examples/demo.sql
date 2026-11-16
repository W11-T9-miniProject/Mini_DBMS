-- The users schema already exists. IDs are assigned by the engine.
INSERT INTO users (name, age, email) VALUES
    ('Alice', 25, 'alice@example.com'),
    ('Bob', 30, 'bob@example.com'),
    ('O''Brien; Kim', 28, 'kim@example.com');

INSERT INTO users (name, age) VALUES ('Default email', 20);
SELECT * FROM users;
SELECT name, age FROM users WHERE id = 2;
SELECT * FROM users WHERE age >= 25;
SELECT id, name FROM users WHERE id BETWEEN 2 AND 4 LIMIT 2;
EXPLAIN SELECT * FROM users WHERE id = 2;
EXPLAIN SELECT * FROM users WHERE name = 'Bob';
