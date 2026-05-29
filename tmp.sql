CREATE TABLE users (id INT, name TEXT, active BOOL);
INSERT INTO users VALUES (1, 'Maddy', 1);
INSERT INTO users VALUES (2, 'Matt', 0);
SELECT * FROM users WHERE name = 'Maddy' or id = 2;
